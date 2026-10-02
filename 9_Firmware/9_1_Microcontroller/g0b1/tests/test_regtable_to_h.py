#!/usr/bin/env python3
"""Host tests for tools/regtable_to_h.py (run by `make test`, or directly)."""
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "regtable_to_h.py"
spec = importlib.util.spec_from_file_location("regtable_to_h", TOOL)
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)

LMX = "R112\t0x700000\nR111\t0x6F0000\nR0\t0x00241C\n"
ADF = "0x0000 0x18\n0x0012 0x01\n0x7FFF 0xAB\n"


def expect_error(chip, text):
    try:
        rt.parse(chip, text.splitlines())
    except rt.RegTableError:
        return True
    return False


def test_lmx_frames_as_is_and_ordered():
    assert rt.parse("lmx2594", LMX.splitlines()) == [0x700000, 0x6F0000, 0x00241C]


def test_lmx_comments_blank_and_case():
    text = "# TICS Pro\n\n// x\nR1\t0x010ABC\nr0 0x00241c\n"
    assert rt.parse("lmx2594", text.splitlines()) == [0x010ABC, 0x00241C]


def test_adf_packing_addr15_shl8_or_data8():
    assert rt.parse("adf4372", ADF.splitlines()) == [0x000018, 0x001201, 0x7FFFAB]


def test_adf_tabs_and_order_kept():
    assert rt.parse("adf4372", "0x0010\t0x02\n0x0001\t0x03\n".splitlines()) == [0x001002, 0x000103]


def test_rejects_empty_input():
    assert expect_error("lmx2594", "")
    assert expect_error("adf4372", "\n# only a comment\n\n")


def test_rejects_malformed_lines():
    for text in ("R0 0x1234\n", "R0 0x00241CZ\n", "R0\n", "garbage\n", "R0 0x00241C extra\n",
                 "R0 0x00241C\nnot a register\n"):
        assert expect_error("lmx2594", text), text
    for text in ("0x0000 0x1\n", "0x000 0x18\n", "0x0000 0x180\n", "0x0000\n", "R0 0x00241C\n",
                 "0x0000 0x18 0x00\n", "0x8000 0x00\n"):
        assert expect_error("adf4372", text), text


def test_lmx_rejects_frame_not_matching_register_number():
    assert expect_error("lmx2594", "R5\t0x040000\n")     # frame address 4 != R5
    assert expect_error("lmx2594", "R113\t0x710000\n")   # beyond R112
    assert expect_error("lmx2594", "R0\t0x800000\n")     # R/W bit set (a read)


def test_rejects_bad_chip_name_and_settle():
    for kw in ({"chip": "ad9999"}, {"name": "1bad"}, {"name": "has space"}, {"settle_ms": -1},
               {"settle_ms": 65536}):
        args = {"chip": "lmx2594", "name": "T", "settle_ms": 10}
        args.update(kw)
        try:
            rt.render(args["chip"], args["name"], args["settle_ms"], [0], "x")
        except rt.RegTableError:
            continue
        raise AssertionError(kw)


def test_generated_header_shape():
    h = rt.render("lmx2594", "LMX_T", 7, rt.parse("lmx2594", LMX.splitlines()), "in.txt")
    assert "#undef PLL_TABLE_PLACEHOLDER" in h and "#define PLL_TABLE_PLACEHOLDER 0" in h
    assert "0x700000" in h and "0x00241C" in h
    assert h.index("0x700000") < h.index("0x6F0000") < h.index("0x00241C")


def test_generated_headers_compile():
    gcc = shutil.which("gcc")
    if gcc is None:
        print("  (gcc missing, compile check skipped)")
        return
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d)
        for chip, text, name in (("lmx2594", LMX, "LMX_T"), ("adf4372", ADF, "ADF_T")):
            src = d / (chip + ".txt")
            src.write_text(text)
            hdr = d / (chip + ".h")
            r = subprocess.run([sys.executable, str(TOOL), "--chip", chip,
                                "--name", name, "--settle-ms", "10", "-o", str(hdr), str(src)],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr
            c = d / (chip + ".c")
            c.write_text(f'#include "{hdr.name}"\n'
                         "_Static_assert(PLL_TABLE_PLACEHOLDER == 0, \"generated = real\");\n"
                         f"int use(void) {{ return (int){name}.count + (int){name}.settle_ms\n"
                         f"    + (int){name}.words[0] + ({name}.name != 0); }}\n")
            r = subprocess.run([gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                                "-I" + str(ROOT / "Core" / "drivers"), "-I" + str(d), str(c)],
                               capture_output=True, text=True)
            assert r.returncode == 0, r.stderr


def test_cli_rejects_empty_and_writes_nothing():
    with tempfile.TemporaryDirectory() as d:
        src = pathlib.Path(d) / "e.txt"
        src.write_text("\n")
        out = pathlib.Path(d) / "o.h"
        r = subprocess.run([sys.executable, str(TOOL), "--chip", "adf4372", "--name", "X",
                            "-o", str(out), str(src)], capture_output=True, text=True)
        assert r.returncode != 0 and not out.exists()


def main():
    tests = [(n, f) for n, f in sorted(globals().items()) if n.startswith("test_") and callable(f)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("ok  ", name)
        except Exception as e:  # noqa: BLE001 - report every failure
            failed += 1
            print("FAIL", name, repr(e))
    print(f"{len(tests)} tests, {failed} failures")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

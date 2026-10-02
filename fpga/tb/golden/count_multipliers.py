#!/usr/bin/env python3
"""Static multiplier / RAM budget gate for the AERIS-10 FPGA pipeline.

Run from anywhere:  python3 tb/golden/count_multipliers.py [file.v ...]
With no arguments the pipeline list is read from ``PROD_RTL`` in
``run_regression.sh`` (single source of truth).  Exit status 1 if the budget
is exceeded or if the tables below no longer match the RTL.

Budget (spec acceptance): <= 55 multipliers (18x18 equivalents), <= 1 Mbit RAM.

How multipliers are counted
---------------------------
1. Every ``*`` operator in synthesizable code is a "site".  Comments, strings,
   attributes, ``localparam``/``parameter`` lines and ``ifdef SIMULATION`` /
   ``ifdef FORMAL`` regions are ignored (the ``else`` branch of an
   ``ifdef SIMULATION`` is synthesizable and IS counted).
2. The number of sites found in each module must equal the number declared in
   MULT_SPEC; otherwise the gate fails ("table out of date").  A new
   multiplication therefore cannot slip in unnoticed.
3. Each product is converted to 18x18 equivalents: ``ceil(a/18) * ceil(b/18)``
   for an a x b-bit product.  A 12x16, 16x16 or 17x18 product is 1; a 24x16 or
   25x16 product is 2 (one 27x27 DSP mode = two 18x18 blocks, or two 18x18
   blocks chained); a 23x7 product is 2.
4. Two figures are reported per module instance:
   * nominal  - distinct products after resource sharing (sites in mutually
                exclusive branches that use identical operands, e.g. the three
                Doppler window multiplies, are one multiplier);
   * worst    - every site as its own multiplier (no sharing at all).
   The gate is applied to BOTH; the nominal figure is the expected result.
5. Instance multiplicities come from the instantiation hierarchy below
   ``radar_system_top`` (generate-if branches not selected by the default
   parameters are excluded).  ``fft_engine`` is costed per instantiation from
   its literal ``INTERNAL_W`` / ``TWIDDLE_W`` overrides (25-bit range FFT,
   24-bit Doppler FFT).

How RAM is counted
------------------
Every array declaration in synthesizable code must appear in MEM_SPEC (else the
gate fails).  Entries of kind RAM / ROM count against the 1 Mbit budget; kind
REGS (arrays that are flip-flops by design: CIC, FIR delay line, ADC delay) are
reported but not counted.  Width and depth are checked against the declaration
where they can be evaluated from default parameters.
"""
# ruff: noqa: T201  (command-line tool: the printed report is its output)
import math
import os
import re
import sys

MAX_MULT = 55
MAX_RAM_BITS = 1 << 20
TOP = "radar_system_top"
BLOCK = 18

HERE = os.path.dirname(os.path.abspath(__file__))
FPGA_DIR = os.path.normpath(os.path.join(HERE, "..", ".."))

# --- multiplier table -------------------------------------------------------
# module -> list of (label, a_width, b_width, sites, distinct)
# a_width/b_width: int, or a parameter name resolved per instantiation, or
# None for a constant power-of-two scaling (synthesised as a shift, cost 0).
MULT_SPEC = {
    "ddc": [("mixer I and Q (12 x 16)", 12, 16, 2, 2)],
    "fir_lowpass": [("4 phase multipliers (17 x 18)", 17, 18, 4, 4)],
    "fft_engine": [("butterfly twiddle (INTERNAL_W x TWIDDLE_W)",
                    "INTERNAL_W", "TWIDDLE_W", 4, 4)],
    "frequency_matched_filter": [("conjugate multiply (16 x 16)", 16, 16, 4, 4)],
    "doppler_processor_optimized": [
        ("window I and Q (16 x 16), 3 exclusive branches share operands", 16, 16, 6, 2),
        ("address = index * RANGE_BINS (64): shift", None, None, 2, 0),
    ],
    "cfar_ca": [
        ("GO/SO cross products (23 x 7), same 2 products in 2 branches", 23, 7, 4, 2),
        ("alpha * noise sum (8 x 23)", 8, 23, 1, 1),
    ],
}

# --- memory table -----------------------------------------------------------
# Row layout: module, array, width, depth, parent, kind, note.
# width: int or parameter name; parent: restrict to instances of module under
# that parent (needed when one module is instantiated with different sizes).
CHAIN = "matched_filter_processing_chain"
SEG = "matched_filter_multi_segment"
DOP = "doppler_processor_optimized"
CHIRP = "plfm_chirp_controller_enhanced"
USB_FT601 = "usb_data_interface"
USB_FT2232H = "usb_data_interface_ft2232h"

MEM_SPEC = [
    ("fft_engine", "mem_re", "INTERNAL_W", 256, CHAIN, "RAM", "range FFT data re"),
    ("fft_engine", "mem_im", "INTERNAL_W", 256, CHAIN, "RAM", "range FFT data im"),
    ("fft_engine", "cos_rom", "TWIDDLE_W", 64, CHAIN, "ROM", "range FFT twiddles"),
    ("fft_engine", "mem_re", "INTERNAL_W", 16, "xfft_16", "RAM", "Doppler FFT data re"),
    ("fft_engine", "mem_im", "INTERNAL_W", 16, "xfft_16", "RAM", "Doppler FFT data im"),
    ("fft_engine", "cos_rom", "TWIDDLE_W", 4, "xfft_16", "ROM", "Doppler FFT twiddles"),
    ("xfft_16", "in_buf_re", 16, 16, None, "RAM", "Doppler FFT input buffer"),
    ("xfft_16", "in_buf_im", 16, 16, None, "RAM", "Doppler FFT input buffer"),
    ("xfft_16", "out_buf_re", 16, 16, None, "RAM", "Doppler FFT output buffer"),
    ("xfft_16", "out_buf_im", 16, 16, None, "RAM", "Doppler FFT output buffer"),
    ("ref_spectrum_rom", "rom_i", 16, 1280, None, "ROM", "reference spectra i (5 x 256)"),
    ("ref_spectrum_rom", "rom_q", 16, 1280, None, "ROM", "reference spectra q (5 x 256)"),
    (CHAIN, "sig_buf_i", 16, 256, None, "RAM", "segment signal buffer"),
    (CHAIN, "sig_buf_q", 16, 256, None, "RAM", "segment signal buffer"),
    (CHAIN, "prod_buf_i", 16, 256, None, "RAM", "product buffer"),
    (CHAIN, "prod_buf_q", 16, 256, None, "RAM", "product buffer"),
    (SEG, "input_buffer_i", 16, 1024, None, "RAM", "receive window buffer"),
    (SEG, "input_buffer_q", 16, 1024, None, "RAM", "receive window buffer"),
    (DOP, "doppler_i_mem", 16, 2048, None, "RAM", "Doppler frame (64 x 32)"),
    (DOP, "doppler_q_mem", 16, 2048, None, "RAM", "Doppler frame (64 x 32)"),
    (DOP, "window_coeff", 16, 16, None, "ROM", "Hamming window"),
    ("cfar_ca", "mag_mem", 17, 2048, None, "RAM", "CFAR magnitude map (64 x 32)"),
    ("cfar_ca", "col_buf", 17, 64, None, "RAM", "CFAR column buffer"),
    ("mti_canceller", "prev_i", 16, 64, None, "RAM", "MTI history"),
    ("mti_canceller", "prev_q", 16, 64, None, "RAM", "MTI history"),
    (CHIRP, "long_chirp_lut", 8, 3600, None, "ROM", "TX long chirp LUT"),
    (CHIRP, "short_chirp_lut", 8, 60, None, "ROM", "TX short chirp LUT"),
    ("nco", "sin_lut", 16, 64, None, "ROM", "NCO quarter-wave LUT"),
    ("fpga_self_test", "test_bram", 16, 64, None, "RAM", "self-test RAM"),
    (USB_FT2232H, "status_words", 32, 6, None, "REGS", "status words"),
    (USB_FT601, "status_words", 32, 6, None, "REGS", "status words (FT601, off)"),
    ("ddc", "adc_dly", 12, 4, None, "REGS", "ADC alignment delay"),
    ("cic_decimator_4x_enhanced", "integ", 26, 5, None, "REGS", "CIC integrators"),
    ("cic_decimator_4x_enhanced", "comb", 26, 5, None, "REGS", "CIC combs"),
    ("cic_decimator_4x_enhanced", "comb_d", 26, 5, None, "REGS", "CIC comb delays"),
    ("fir_lowpass", "coeff", 18, 16, None, "ROM", "FIR coefficients (half, symmetric)"),
    ("fir_lowpass", "dline", 16, 32, None, "REGS", "FIR delay line"),
]


def strip_for_synthesis(text):
    """Remove comments, strings, attributes and SIMULATION/FORMAL-only regions."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = text.replace("@(*)", "@(_all_)").replace("@*", "@(_all_)")
    text = re.sub(r"\(\*\s*[A-Za-z_].*?\*\)", " ", text, flags=re.S)
    out, stack = [], []
    for line in text.splitlines():
        code = re.sub(r"//.*", "", line)
        code = re.sub(r'"[^"]*"', '""', code)
        m = re.match(r"\s*`(ifdef|ifndef|else|endif)\b\s*(\w*)", code)
        if m:
            kw, sym = m.group(1), m.group(2)
            if kw in ("ifdef", "ifndef"):
                if sym not in ("SIMULATION", "FORMAL"):
                    sys.exit(f"unsupported macro `{kw} {sym}: teach strip_for_synthesis about it")
                stack.append((sym not in ("SIMULATION", "FORMAL")) == (kw == "ifdef"))
            elif kw == "else" and stack:
                stack[-1] = not stack[-1]
            elif kw == "endif" and stack:
                stack.pop()
            continue
        if all(stack):
            out.append(code)
    return "\n".join(out)


def module_blocks(code):
    return [(m.group(1), m.group(2))
            for m in re.finditer(r"^\s*module\s+(\w+)(.*?)^\s*endmodule", code, flags=re.M | re.S)]


def multiply_sites(code):
    sites = []
    for ln, line in enumerate(code.splitlines(), 1):
        if re.match(r"\s*(localparam|parameter)\b", line):
            continue
        if re.match(r"\s*(reg|wire|integer)\b", line):
            line = re.sub(r"\[[^\]]*\]", "[]", line)      # widths / array depths are not products
        n = line.replace("**", "").count("*")
        if n:
            sites.append((ln, n, line.strip()))
    return sites


PARAM_RE = r"#\s*\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)"


def instantiations(code, names):
    """[(child, {param: int literal})] for every instantiation in the code."""
    found = []
    for name in names:
        pat = r"^\s*" + re.escape(name) + r"\s*(" + PARAM_RE + r")?\s*(\w+)\s*\("
        for m in re.finditer(pat, code, flags=re.M):
            params = {}
            if m.group(1):
                for pm in re.finditer(r"\.(\w+)\s*\(\s*(\d+)\s*\)", m.group(1)):
                    params[pm.group(1)] = int(pm.group(2))
            found.append((name, params))
    return found


def default_params(block):
    params = {}
    decl = r"\b(?:parameter|localparam)\s+(?:\[[^\]]*\]\s*)?(?:signed\s+)?(\w+)\s*=\s*([^,;)\n]+)"
    for m in re.finditer(decl, block):
        val = try_eval(m.group(2), params)
        if val is not None:
            params[m.group(1)] = val
    return params


def try_eval(expr, params):
    expr = expr.strip()
    expr = re.sub(r"\b\d+'[sS]?[dD](\d+)\b", r"\1", expr)
    for name, val in sorted(params.items(), key=lambda kv: -len(kv[0])):
        expr = re.sub(r"\b" + re.escape(name) + r"\b", str(val), expr)
    if re.search(r"[A-Za-z_]", expr) or not re.fullmatch(r"[\d\s+\-*/()<>]+", expr):
        return None
    try:
        return int(eval(expr.replace("/", "//"), {"__builtins__": {}}))  # digits and operators only
    except (SyntaxError, ZeroDivisionError):
        return None


def blocks18(a, b):
    return math.ceil(a / BLOCK) * math.ceil(b / BLOCK)


def resolve(spec, params, defaults, what):
    if isinstance(spec, int) or spec is None:
        return spec
    if spec in params:
        return params[spec]
    if spec in defaults:
        return defaults[spec]
    sys.exit(f"cannot resolve {spec} for {what}")


def load_files(args):
    if args:
        names = args
    else:
        with open(os.path.join(FPGA_DIR, "run_regression.sh")) as fh:
            text = fh.read()
        body = re.search(r"^PROD_RTL=\((.*?)^\)", text, flags=re.M | re.S).group(1)
        names = re.findall(r"^\s*(\S+\.v)\s*$", body, flags=re.M)
    code_by_file = {}
    for f in names:
        path = f if os.path.exists(f) else os.path.join(FPGA_DIR, f)
        with open(path) as fh:
            code_by_file[f] = strip_for_synthesis(fh.read())
    return code_by_file


def main():
    code_by_file = load_files(sys.argv[1:])
    code_of, file_of, defaults = {}, {}, {}
    for f, code in code_by_file.items():
        for name, block in module_blocks(code):
            code_of[name], file_of[name] = block, f
            defaults[name] = default_params(block)
    if TOP not in code_of:
        sys.exit(f"top module {TOP} not found")
    children = {m: instantiations(code_of[m], list(code_of)) for m in code_of}

    # generate-if selection in the top: USB_MODE picks exactly one USB module
    usb_mode = defaults[TOP].get("USB_MODE", 1)
    inactive = {"usb_data_interface"} if usb_mode != 0 else {"usb_data_interface_ft2232h"}

    # instance records: module -> list of (parent, params)
    records = {m: [] for m in code_of}

    def visit(mod, parent, params):
        records[mod].append((parent, params))
        for child, cparams in children[mod]:
            if child in inactive and mod == TOP:
                continue
            visit(child, mod, cparams)

    visit(TOP, None, {})
    ok = True

    # ---- multipliers ----
    print(f"{'module':32s} {'inst':>4s} {'sites':>5s} {'nominal':>8s} {'worst':>6s}")
    nominal_total = worst_total = 0
    for f in code_by_file:
        for mod in [m for m in code_of if file_of[m] == f]:
            sites = multiply_sites(code_of[mod])
            n_sites = sum(c for _, c, _ in sites)
            spec = MULT_SPEC.get(mod, [])
            declared = sum(s[3] for s in spec)
            if n_sites != declared:
                print(f"  ERROR: {mod}: {n_sites} multiply sites in RTL, "
                      f"MULT_SPEC declares {declared}")
                for ln, c, src in sites:
                    print(f"      {f}:{ln} ({c}x) {src[:80]}")
                ok = False
                continue
            if not spec:
                continue
            mod_nom = mod_worst = 0
            for _parent, params in records[mod]:
                for _label, a, b, n_sites_row, distinct in spec:
                    aw = resolve(a, params, defaults[mod], mod)
                    bw = resolve(b, params, defaults[mod], mod)
                    cost = 0 if aw is None else blocks18(aw, bw)
                    mod_nom += distinct * cost
                    mod_worst += n_sites_row * cost
            nominal_total += mod_nom
            worst_total += mod_worst
            print(f"{mod:32s} {len(records[mod]):4d} {n_sites:5d} {mod_nom:8d} {mod_worst:6d}")
            for label, _a, _b, n_sites_row, distinct in spec:
                print(f"      {label}: {n_sites_row} site(s), {distinct} distinct")
    print(f"\nTOTAL multipliers, 18x18 equivalents: nominal {nominal_total}, "
          f"worst case {worst_total}  (limit {MAX_MULT})")
    if worst_total > MAX_MULT:
        print("FAIL: multiplier budget exceeded")
        ok = False

    # ---- memories ----
    declared_arrays = {}
    for mod, code in code_of.items():
        array_decl = r"^\s*reg\b[^;\n]*?\b(\w+)\s*\[([^\]:]+):([^\]]+)\]\s*;"
        for m in re.finditer(array_decl, code, flags=re.M):
            declared_arrays.setdefault((mod, m.group(1)), (m.group(2), m.group(3)))
    table_keys = {(mod, arr) for mod, arr, *_ in MEM_SPEC}
    for key in sorted(set(declared_arrays) - table_keys):
        print(f"  ERROR: array {key[0]}.{key[1]} is not in MEM_SPEC")
        ok = False

    print(f"\n{'memory':62s} {'inst':>4s} {'bits':>9s}  kind")
    ram_bits = reg_bits = 0
    for mod, arr, w, depth, parent, kind, note in MEM_SPEC:
        if (mod, arr) not in declared_arrays:
            print(f"  ERROR: {mod}.{arr} not declared in the RTL (MEM_SPEC out of date)")
            ok = False
            continue
        recs = [r for r in records[mod] if parent is None or r[0] == parent]
        if mod in inactive:
            recs = []
        if parent is None and mod not in inactive and not recs:
            print(f"  note: {mod} is not instantiated under {TOP}")
        if parent is None:
            hi, lo = declared_arrays[(mod, arr)]
            span = try_eval(f"({hi})-({lo})", defaults[mod])
            dep_decl = None if span is None else abs(span) + 1
            if dep_decl is not None and dep_decl != depth:
                print(f"  ERROR: {mod}.{arr} depth {dep_decl} in RTL, {depth} in MEM_SPEC")
                ok = False
        bits = sum(resolve(w, params, defaults[mod], mod) * depth for _p, params in recs)
        if kind == "REGS":
            reg_bits += bits
        else:
            ram_bits += bits
        label = f"{mod}.{arr} ({note})"
        print(f"{label:62s} {len(recs):4d} {bits:9d}  {kind}")
    print(f"\nTOTAL RAM+ROM bits: {ram_bits}  ({ram_bits / 1024:.1f} kbit, "
          f"limit {MAX_RAM_BITS // 1024} kbit); "
          f"register arrays (flip-flops, not counted): {reg_bits}")
    if ram_bits > MAX_RAM_BITS:
        print("FAIL: RAM budget exceeded")
        ok = False

    print("RESOURCE GATE: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# Path gate: fail if a tracked file references the old upstream layout
# (9_Firmware, 9_1_Microcontroller, 9_2_FPGA, 9_3_GUI, 7_Components,
# 4_Schematics, BOM_OPTIMIZATION_REPORT) other than as a legacy/... path, or
# to the old `firmware/` directory (renamed `stm32/`).
#
# Exempt: legacy/ (the archive itself) and docs/superpowers/ (specs and plans
# describe the move). A line may opt out with the marker
# `path-gate: legacy-ref` (for constructed paths such as Python
# `"legacy" / "9_Firmware"`).
#
# Exit status: 0 OK, 1 references found, 2 internal error (grep failed).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# A name is allowed right after `legacy/` or after `legacy/9_Firmware/`.
pattern='(?<!legacy/)(?<!legacy/9_Firmware/)(9_Firmware|9_1_Microcontroller|9_2_FPGA|9_3_GUI|7_Components|4_Schematics|BOM_OPTIMIZATION_REPORT)'

# The pre-rename MCU directory `firmware/` (now `stm32/`) as a path: it must
# not be preceded by a path or word character, so `stm32/firmware/` and
# `legacy/.../firmware/` are not flagged, only a top-level `firmware/`.
pattern_fw='(^|[^A-Za-z0-9_./-])firmware/'
# Relative forms `./firmware/`, `../firmware/` and bare directory uses such as
# `cd firmware`, `make -C firmware`, `working-directory: firmware`. Prose
# ("G0B1 firmware", "firmware `STATUS`") does not match.
pattern_fw2='\.\.?/firmware/|(\bcd|-C|working-directory:)[ \t]+["'"'"']?\.?/?firmware(?![A-Za-z0-9_.-])'

errfile=$(mktemp)
listfile=$(mktemp)
trap 'rm -f "$errfile" "$listfile"' EXIT

# `grep -v` and `grep -P` exit 1 when nothing matches (not an error); real
# errors are caught through stderr, because xargs folds every grep failure
# into exit status 123.
git ls-files -z | { grep -zvE '^(legacy/|docs/superpowers/|tools/check_paths\.sh$)' || [ $? -eq 1 ]; } >"$listfile"
hits=$({ xargs -0 grep -InP -e "$pattern" -- <"$listfile" 2>"$errfile" || true; } \
  | { grep -v 'path-gate: legacy-ref' || [ $? -eq 1 ]; })
hits_fw=$({ xargs -0 grep -InP -e "$pattern_fw" -- <"$listfile" 2>>"$errfile" || true; } \
  | { grep -v 'path-gate: legacy-ref' || [ $? -eq 1 ]; })
hits_fw2=$({ xargs -0 grep -InP -e "$pattern_fw2" -- <"$listfile" 2>>"$errfile" || true; } \
  | { grep -v 'path-gate: legacy-ref' || [ $? -eq 1 ]; })
hits_fw="$hits_fw${hits_fw2:+${hits_fw:+$'\n'}$hits_fw2}"
hits="$hits${hits_fw:+${hits:+$'\n'}$hits_fw}"

if [ -s "$errfile" ]; then
  echo "path gate: grep reported errors:" >&2
  cat "$errfile" >&2
  exit 2
fi

if [ -n "$hits" ]; then
  echo "path gate: references to the old layout found:" >&2
  echo "$hits" >&2
  exit 1
fi
echo "path gate: OK"

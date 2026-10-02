#!/usr/bin/env bash
# Path gate: fail if a tracked file references the old upstream layout
# (9_Firmware, 9_1_Microcontroller, 9_2_FPGA, 9_3_GUI, 7_Components,
# 4_Schematics, BOM_OPTIMIZATION_REPORT) other than as a legacy/... path.
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

errfile=$(mktemp)
listfile=$(mktemp)
trap 'rm -f "$errfile" "$listfile"' EXIT

# `grep -v` and `grep -P` exit 1 when nothing matches (not an error); real
# errors are caught through stderr, because xargs folds every grep failure
# into exit status 123.
git ls-files -z | { grep -zvE '^(legacy/|docs/superpowers/|tools/check_paths\.sh$)' || [ $? -eq 1 ]; } >"$listfile"
hits=$({ xargs -0 grep -InP -e "$pattern" -- <"$listfile" 2>"$errfile" || true; } \
  | { grep -v 'path-gate: legacy-ref' || [ $? -eq 1 ]; })

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

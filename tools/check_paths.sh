#!/usr/bin/env bash
# Path gate: fail if a tracked file references the old upstream layout
# (9_Firmware, 9_1_Microcontroller, 9_2_FPGA, 9_3_GUI, 7_Components,
# 4_Schematics, BOM_OPTIMIZATION_REPORT) other than as a legacy/... path.
#
# Exempt: legacy/ (the archive itself), docs/superpowers/ (specs and plans
# describe the move) and docs/bom-optimization.md (historical upstream
# analysis). A line may opt out with the marker `path-gate: legacy-ref`
# (for constructed paths such as Python `"legacy" / "9_Firmware"`).
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

# A name is allowed right after `legacy/` or after `legacy/9_Firmware/`.
pattern='(?<!legacy/)(?<!legacy/9_Firmware/)(9_Firmware|9_1_Microcontroller|9_2_FPGA|9_3_GUI|7_Components|4_Schematics|BOM_OPTIMIZATION_REPORT)'

hits=$(git ls-files -z \
  | grep -zvE '^(legacy/|docs/superpowers/|docs/bom-optimization\.md$|tools/check_paths\.sh$)' \
  | xargs -0 grep -InP -e "$pattern" -- 2>/dev/null \
  | grep -v 'path-gate: legacy-ref' || true)

if [ -n "$hits" ]; then
  echo "path gate: references to the old layout found:" >&2
  echo "$hits" >&2
  exit 1
fi
echo "path gate: OK"

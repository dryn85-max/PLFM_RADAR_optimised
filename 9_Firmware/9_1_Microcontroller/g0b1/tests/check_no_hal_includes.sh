#!/usr/bin/env bash
# Drivers and app modules must not include any stm32g0xx*.h header.
cd "$(dirname "$0")/.." || exit 2
hits=$(grep -rlE '#[[:space:]]*include[[:space:]]*[<"]stm32g0' Core/drivers Core/app 2>/dev/null)
if [ -n "$hits" ]; then
    echo "ERROR: stm32g0xx header included outside Core/hal:" >&2
    echo "$hits" >&2
    exit 1
fi
echo "check_no_hal_includes: ok"

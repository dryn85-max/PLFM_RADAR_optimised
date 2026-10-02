#!/usr/bin/env bash
# Re-vendor the STM32CubeG0 subset used by the G0B1 firmware.
# Fetches three pinned tags, verifies the commit SHA, and copies an allow-list of files
# into Drivers/, Core/ and the firmware root. The result is committed.
#
# Pins:
#   stm32g0xx_hal_driver  v1.4.7           a0cf8a8b96183fdcc2e3b1cf0bcf0825f27bd0c9
#   cmsis_device_g0       v1.4.5           f576c24e123edf3332988ecd49512c0f35f85186
#   cmsis_core            v5.9.0_20250520  b7487de1303e2eb77c402432e368691e9dcfb5f0
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

HAL_REPO=STMicroelectronics/stm32g0xx_hal_driver
HAL_SHA=a0cf8a8b96183fdcc2e3b1cf0bcf0825f27bd0c9
DEV_REPO=STMicroelectronics/cmsis_device_g0
DEV_SHA=f576c24e123edf3332988ecd49512c0f35f85186
CORE_REPO=STMicroelectronics/cmsis_core
CORE_SHA=b7487de1303e2eb77c402432e368691e9dcfb5f0

HAL_MODULES="hal hal_cortex hal_rcc hal_rcc_ex hal_gpio hal_spi hal_spi_ex hal_i2c hal_i2c_ex hal_uart hal_uart_ex hal_tim hal_tim_ex hal_iwdg hal_pwr hal_pwr_ex hal_flash hal_flash_ex hal_dma hal_dma_ex"
# Headers with no matching .c file.
HAL_EXTRA_INC="stm32g0xx_hal_def.h stm32g0xx_hal_gpio_ex.h"

# Deviation from the plan: the sandbox proxy refuses codeload.github.com tarballs,
# so each repo is obtained with `git clone --depth 1 --branch <tag>` instead. An
# existing clone can be reused: CUBE_SRC_DIR (default /home/user/stmicroelectronics)
# must contain stm32g0xx_hal_driver/, cmsis_device_g0/ and cmsis_core/. In both
# cases `git rev-parse HEAD` must equal the pinned SHA, otherwise the script aborts.
CUBE_SRC_DIR="${CUBE_SRC_DIR:-/home/user/stmicroelectronics}"

fetch() { # repo tag sha  -> prints the checkout directory
    local repo=$1 tag=$2 sha=$3 name dir
    name="${repo##*/}"
    if [ -d "$CUBE_SRC_DIR/$name/.git" ]; then
        dir="$CUBE_SRC_DIR/$name"
    else
        dir="$TMP/$name"
        git clone -q --depth 1 --branch "$tag" "https://github.com/$repo" "$dir"
    fi
    local got
    got="$(git -C "$dir" rev-parse HEAD)"
    if [ "$got" != "$sha" ]; then
        echo "SHA mismatch for $repo: got $got, expected $sha" >&2
        exit 1
    fi
    echo "$dir"
}

HAL="$(fetch $HAL_REPO v1.4.7 $HAL_SHA)"
DEV="$(fetch $DEV_REPO v1.4.5 $DEV_SHA)"
CORE="$(fetch $CORE_REPO v5.9.0_20250520 $CORE_SHA)"

D="$ROOT/Drivers"
rm -rf "$D/STM32G0xx_HAL_Driver" "$D/CMSIS"
mkdir -p "$D/STM32G0xx_HAL_Driver/Inc/Legacy" "$D/STM32G0xx_HAL_Driver/Src" \
         "$D/CMSIS/Device/ST/STM32G0xx/Include" "$D/CMSIS/Include" "$ROOT/Core"

# HAL
for m in $HAL_MODULES; do
    cp "$HAL/Inc/stm32g0xx_$m.h" "$D/STM32G0xx_HAL_Driver/Inc/"
    cp "$HAL/Src/stm32g0xx_$m.c" "$D/STM32G0xx_HAL_Driver/Src/"
done
for f in $HAL_EXTRA_INC; do cp "$HAL/Inc/$f" "$D/STM32G0xx_HAL_Driver/Inc/"; done
cp "$HAL/Inc/Legacy/stm32_hal_legacy.h" "$D/STM32G0xx_HAL_Driver/Inc/Legacy/"
# LL headers pulled in by the HAL headers (copy those that exist and are referenced)
for f in "$HAL"/Inc/stm32g0xx_ll_*.h; do
    b="$(basename "$f")"
    if grep -rqs "\"$b\"" "$D/STM32G0xx_HAL_Driver/Inc" "$D/STM32G0xx_HAL_Driver/Src"; then
        cp "$f" "$D/STM32G0xx_HAL_Driver/Inc/"
    fi
done
# second pass: LL headers included by LL headers
for f in "$HAL"/Inc/stm32g0xx_ll_*.h; do
    b="$(basename "$f")"
    if [ ! -e "$D/STM32G0xx_HAL_Driver/Inc/$b" ] && grep -qs "\"$b\"" "$D"/STM32G0xx_HAL_Driver/Inc/stm32g0xx_ll_*.h; then
        cp "$f" "$D/STM32G0xx_HAL_Driver/Inc/"
    fi
done
cp "$HAL/LICENSE.md" "$D/STM32G0xx_HAL_Driver/LICENSE.md"

# CMSIS device
for f in stm32g0xx.h stm32g0b1xx.h system_stm32g0xx.h; do
    cp "$DEV/Include/$f" "$D/CMSIS/Device/ST/STM32G0xx/Include/"
done
cp "$DEV/Source/Templates/gcc/startup_stm32g0b1xx.s" "$ROOT/"
cp "$DEV/Source/Templates/system_stm32g0xx.c" "$ROOT/Core/"
cp "$DEV/LICENSE.md" "$D/CMSIS/Device/ST/STM32G0xx/LICENSE.md"

# CMSIS core
for f in core_cm0plus.h cmsis_compiler.h cmsis_gcc.h cmsis_version.h mpu_armv7.h; do
    cp "$CORE/Core/Include/$f" "$D/CMSIS/Include/"
done
cp "$CORE/LICENSE.md" "$D/CMSIS/LICENSE.md"

echo "Vendored OK into $D"

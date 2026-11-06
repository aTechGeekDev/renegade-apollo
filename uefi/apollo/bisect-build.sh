#!/usr/bin/env bash
# Build three apollo UEFI images whose DSDTs remove progressively more, to find which part
# of the table makes ACPI.sys crash (DRIVER_IRQL_NOT_LESS_OR_EQUAL). Run on the build machine:
#
#   bash bisect-build.sh            -> ~/boot-apollo-A.img, -B.img, -C.img
#   bash bisect-build.sh D="--drop PRTC --no-thermal"   -> just ~/boot-apollo-D.img
#
# A  = full table minus PRTC (ACPI000E Time & Alarm device, driven by ACPI.sys itself)
# B  = A minus every thermal zone (also owned by ACPI.sys)
# C  = B minus everything not needed to boot: PMIC extension, buttons, 2nd USB controller,
#      GPI DMA, BAM, reset-protection. Keeps UFS, PEP, GPIO, SPMI, PMIC, USB + Type-C.
#
# Boot A first. If it reaches setup, PRTC is the culprit; if not, try B, then C.
#
# FB_SHIFT=256 (default) starts the framebuffer 256 rows lower so the bugcheck text Windows prints in
# the top-left corner is not hidden by the punch-hole camera. Multiples of 128 only (page alignment).
# FB_SHIFT=0 builds with the normal full-screen framebuffer. apollo.dsc is restored when the script ends.
set -euo pipefail

EDK=${EDK:-$HOME/edk2-msm}
SACPI=${SACPI:-$HOME/Silicium-ACPI}
GEN=${GEN:-$HOME/make-apollo-dsdt.py}
OUT=${OUT:-$HOME/apollo-dsdt}
SRC=$SACPI/Platforms/Xiaomi/pipa/Decompiled/DSDT.dsl
ACPI=$EDK/Platform/Xiaomi/sm8250/AcpiTables/apollo
DSC=$EDK/Platform/Xiaomi/sm8250/apollo.dsc
FB_SHIFT=${FB_SHIFT:-256}

for f in "$SRC" "$GEN" "$ACPI/Dsdt.asl" "$DSC"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

# Framebuffer shift (debug): apollo.dsc includes sm8250.dsc first, so these override its 0x9c000000 / 2400.
fb_reset() {
    sed -i -e '/^  gQcomTokenSpaceGuid.PcdMipiFrameBufferAddress|/d' \
        -e 's/^  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|.*/  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|2400/' "$DSC"
}
if [ "$FB_SHIFT" -ne 0 ]; then
    { [ $((FB_SHIFT % 128)) -eq 0 ] && [ "$FB_SHIFT" -lt 2400 ]; } \
        || { echo "FB_SHIFT must be a multiple of 128 below 2400" >&2; exit 1; }
    FB_ADDR=$(printf '0x%X' $((0x9C000000 + FB_SHIFT * 1080 * 4)))
    fb_reset
    sed -i "s/^  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|2400\$/  gQcomTokenSpaceGuid.PcdMipiFrameBufferAddress|$FB_ADDR\n  gQcomTokenSpaceGuid.PcdMipiFrameBufferHeight|$((2400 - FB_SHIFT))/" "$DSC"
    grep -q "PcdMipiFrameBufferAddress|$FB_ADDR" "$DSC" || { echo "could not apply FB_SHIFT to $DSC" >&2; exit 1; }
    trap fb_reset EXIT
    echo "=== framebuffer shifted $FB_SHIFT rows: $FB_ADDR, height $((2400 - FB_SHIFT))"
fi

declare -A ARGS=()
NAMES=()
if [ $# -eq 0 ]; then
    set -- A="--drop PRTC" B="--drop PRTC --no-thermal" \
        C="--drop PRTC,PEXT,BTNS,URS1,QGP0,QGP1,QGP2,BAM1,BAM5,BAM6,BAM7,BAMD,BAME,BAMF,BAMG,RPEN --no-thermal"
fi
for kv in "$@"; do
    NAMES+=("${kv%%=*}")
    ARGS[${kv%%=*}]="${kv#*=}"
done

[ -e "$ACPI/Dsdt.asl.full" ] || cp "$ACPI/Dsdt.asl" "$ACPI/Dsdt.asl.full"   # the table that crashed, kept once

for v in "${NAMES[@]}"; do
    echo "=== variant $v: ${ARGS[$v]}"
    mkdir -p "$OUT/$v"
    # shellcheck disable=SC2086
    python3 "$GEN" "$SRC" "$OUT/$v/Dsdt.asl" ${ARGS[$v]} | tail -2
    cp "$OUT/$v/Dsdt.asl" "$ACPI/Dsdt.asl"
    rm -rf "$EDK/workspace/Build/apollo" "$ACPI/DSDT.aml"
    (cd "$EDK" && PATH="$HOME/gccshim:$PATH" ./build.sh --device apollo -A -f > "$HOME/apollo-build-$v.log" 2>&1) \
        || { echo "build $v failed, see ~/apollo-build-$v.log" >&2; tail -20 "$HOME/apollo-build-$v.log" >&2; exit 1; }
    [ -s "$ACPI/DSDT.aml" ] || { echo "variant $v: DSDT.aml was not rebuilt" >&2; exit 1; }
    cp "$EDK/boot-apollo.img" "$HOME/boot-apollo-$v.img"
    echo "variant $v: DSDT.aml $(stat -c%s "$ACPI/DSDT.aml") bytes -> ~/boot-apollo-$v.img"
done

cp "$ACPI/Dsdt.asl.full" "$ACPI/Dsdt.asl"   # leave the tree on the full table
echo "done: ${NAMES[*]/#/boot-apollo-}"

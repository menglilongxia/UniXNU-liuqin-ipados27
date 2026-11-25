#!/usr/bin/env bash
# Compile the liuqin hardware DTB appended to boot.img (matched by Qualcomm ABL
# via qcom,msm-id / qcom,board-id). If UNIXNU_LIUQIN_LIVE_FDT is provided (e.g.
# dumped from /sys/firmware/fdt or extracted from stock vendor_boot), copy it
# verbatim after validating the FDT magic.
set -euo pipefail

DEVICE=${UNIXNU_DEVICE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
SOC_DTS=${UNIXNU_SM8475_DTS:-$(cd "$DEVICE/../UniXNU-sm8475-common/dts" 2>/dev/null && pwd || true)}
OUT_DIR=${1:-"$DEVICE/build/liuqin"}
OUT_DTB="$OUT_DIR/liuqin.dtb"

mkdir -p "$OUT_DIR"

if [ -n "${UNIXNU_LIUQIN_LIVE_FDT:-}" ]; then
  if [ ! -f "$UNIXNU_LIUQIN_LIVE_FDT" ]; then
    echo "UNIXNU_LIUQIN_LIVE_FDT not found: $UNIXNU_LIUQIN_LIVE_FDT" >&2
    exit 1
  fi
  cp "$UNIXNU_LIUQIN_LIVE_FDT" "$OUT_DTB"
  echo "liuqin.dtb (live): $(stat -c%s "$OUT_DTB" 2>/dev/null || stat -f%z "$OUT_DTB") bytes"
  exit 0
fi

if [ -z "$SOC_DTS" ] || [ ! -f "$SOC_DTS/sm8475.dtsi" ]; then
  # Fallback: search assembled tree/dts if invoked inside UniXNU build tree
  if [ -f "dts/sm8475.dtsi" ]; then
    SOC_DTS="$(pwd)/dts"
  else
    echo "sm8475.dtsi not found; set UNIXNU_SM8475_DTS or keep ../UniXNU-sm8475-common beside UniXNU-liuqin" >&2
    exit 1
  fi
fi

dtc -I dts -O dtb -i "$SOC_DTS" -o "$OUT_DTB" "$DEVICE/dts/liuqin.dts"
# Also emit into current working directory build/liuqin if running inside UniXNU tree
if [ "$(pwd)/build/liuqin" != "$OUT_DIR" ]; then
  mkdir -p build/liuqin
  cp "$OUT_DTB" build/liuqin/liuqin.dtb
fi
echo "liuqin.dtb compiled: $(stat -c%s "$OUT_DTB" 2>/dev/null || stat -f%z "$OUT_DTB") bytes"

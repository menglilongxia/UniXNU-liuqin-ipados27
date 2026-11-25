#!/usr/bin/env bash
# Copy UniXNU-liuqin's Mu-Silicium firmware overlay (Platforms/Xiaomi/liuqinPkg
# and Resources/Configs/liuqin.toml) into the target uefi-sheng checkout before
# EDK II builds the firmware volume.
set -euo pipefail

DEVICE=${UNIXNU_DEVICE:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
UEFI=${UNIXNU_UEFI:-}

if [ -z "$UEFI" ] || [ ! -d "$UEFI" ]; then
  echo "prepare-uefi-overlay.sh: UNIXNU_UEFI must point to a uefi-sheng checkout" >&2
  exit 1
fi

mkdir -p "$UEFI/Platforms/Xiaomi" "$UEFI/Resources/Configs"
rm -rf "$UEFI/Platforms/Xiaomi/liuqinPkg"
cp -R "$DEVICE/firmware/Platforms/Xiaomi/liuqinPkg" "$UEFI/Platforms/Xiaomi/liuqinPkg"
cp "$DEVICE/firmware/Resources/Configs/liuqin.toml" "$UEFI/Resources/Configs/liuqin.toml"

echo "liuqin UEFI overlay installed into $UEFI"

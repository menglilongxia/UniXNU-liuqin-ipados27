#!/usr/bin/env python3
"""Install UniXNU-liuqin's Mu-Silicium overlay into the target uefi-sheng checkout."""
import os
import shutil
import sys
from pathlib import Path


def main():
    device = Path(os.environ.get("UNIXNU_DEVICE") or Path(__file__).resolve().parent.parent)
    uefi_env = os.environ.get("UNIXNU_UEFI")
    if not uefi_env:
        sys.exit("prepare-uefi-overlay.py: UNIXNU_UEFI is not set")
    uefi = Path(uefi_env)
    if not uefi.is_dir():
        sys.exit(f"prepare-uefi-overlay.py: UNIXNU_UEFI not found: {uefi}")

    pkg_src = device / "firmware/Platforms/Xiaomi/liuqinPkg"
    pkg_dst = uefi / "Platforms/Xiaomi/liuqinPkg"
    cfg_src = device / "firmware/Resources/Configs/liuqin.toml"
    cfg_dst = uefi / "Resources/Configs/liuqin.toml"

    pkg_dst.parent.mkdir(parents=True, exist_ok=True)
    cfg_dst.parent.mkdir(parents=True, exist_ok=True)
    if pkg_dst.exists():
        shutil.rmtree(pkg_dst)
    shutil.copytree(pkg_src, pkg_dst)
    shutil.copy2(cfg_src, cfg_dst)
    print(f"liuqin UEFI overlay installed into {uefi}")


if __name__ == "__main__":
    main()

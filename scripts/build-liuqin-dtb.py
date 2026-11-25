#!/usr/bin/env python3
"""Compile the liuqin hardware DTB appended to boot.img (matched by Qualcomm ABL)."""
import os
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    device = Path(os.environ.get("UNIXNU_DEVICE") or Path(__file__).resolve().parent.parent)
    cwd = Path.cwd()
    out_targets = [device / "build/liuqin/liuqin.dtb", cwd / "build/liuqin/liuqin.dtb"]

    live_fdt = os.environ.get("UNIXNU_LIUQIN_LIVE_FDT")
    if live_fdt:
        src = Path(live_fdt)
        if not src.is_file():
            sys.exit(f"UNIXNU_LIUQIN_LIVE_FDT not found: {src}")
        for dst in out_targets:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        print(f"liuqin.dtb (live): {src.stat().st_size} bytes")
        return

    candidates = [
        Path(os.environ["UNIXNU_SM8475_DTS"]) if os.environ.get("UNIXNU_SM8475_DTS") else None,
        device.parent / "UniXNU-sm8475-common/dts",
        cwd / "dts",
    ]
    soc_dts = next((p for p in candidates if p and (p / "sm8475.dtsi").is_file()), None)
    if not soc_dts:
        sys.exit("sm8475.dtsi not found; set UNIXNU_SM8475_DTS or keep ../UniXNU-sm8475-common beside UniXNU-liuqin")

    primary = out_targets[0]
    primary.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["dtc", "-I", "dts", "-O", "dtb", "-i", str(soc_dts), "-o", str(primary), str(device / "dts/liuqin.dts")],
        check=True,
    )
    for dst in out_targets[1:]:
        if dst.resolve() != primary.resolve():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(primary, dst)
    print(f"liuqin.dtb compiled: {primary.stat().st_size} bytes")


if __name__ == "__main__":
    main()

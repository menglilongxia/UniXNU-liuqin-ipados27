# UniXNU-liuqin

Xiaomi Pad 6 Pro (`liuqin`, Qualcomm SM8475 / Snapdragon 8+ Gen 1, 12GB + 512GB) device repository following the official **UniXNU Format 2** architecture (`UniXNU-sheng` reference) targeting **iPadOS 27.0.1 (24A446, arm64e)**.

## Architecture Alignment with `UniXNU-sheng`

- **OS Profile**: Official built-in `ipados-27.0.1-24A446` (`arm64e`, `xnu-12377.121.6`) — no forked iPadOS 18 `PongPlatform24` stack.
- **SoC Layer**: Shared `UniXNU-sm8475-common` (`dts/sm8475.dtsi`, SMP Affinity1 patch, Qualcomm Gunyah physical timer patch).
- **UEFI Firmware**: Official `UniXNU/uefi-sheng` (`Silicon/Qualcomm/WaipioPkg`) + `firmware/Platforms/Xiaomi/liuqinPkg` overlay (`LIUQIN_VENDOR_HW=0`, `SimpleFbDxe` 1800×2880 on ABL `0xB8000000`), compatible with HyperOS 1/2/3 without vendor DXE mismatch.
- **Zero-flash bring-up**: Always boot via `fastboot boot` (Android boot image header v4); never overwrite `boot_a`/`boot_b`.

## Quick Start (Stage 1 UEFI on WSL2 / Linux)

```bash
cd ../UniXNU
../venv/bin/python3 ./unixnu defconfig liuqin_ipados27_defconfig -D ../UniXNU-liuqin
../venv/bin/python3 ./unixnu fetch -D ../UniXNU-liuqin
../venv/bin/python3 ./unixnu build package -D ../UniXNU-liuqin --uefi ../uefi-sheng --edk2 ../uefi-sheng/Mu_Basecore
../venv/bin/python3 ./unixnu build firmware -D ../UniXNU-liuqin --uefi ../uefi-sheng --edk2 ../uefi-sheng/Mu_Basecore
../venv/bin/python3 ./unixnu bootimg -D ../UniXNU-liuqin --uefi ../uefi-sheng --dtb ../UniXNU-liuqin/build/liuqin/liuqin.dtb --ramdisk none
```

Boot on the connected tablet (in fastboot mode):

```bash
fastboot boot out/liuqin/ipados-27.0.1-24A446/liuqin_ipados27/boot/boot.img
```

## Stage 2 Kernel Build (GitHub Actions `macos-26`)

Push `UniXNU-liuqin` and `UniXNU-sm8475-common` to GitHub, update `feeds.toml` to point to your `UniXNU-sm8475-common` commit, add repository secret `UNIXNU_UNIQUE_CHIP_ID` (8 hex digits), and trigger `.github/workflows/macos-build.yml`.

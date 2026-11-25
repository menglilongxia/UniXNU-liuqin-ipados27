# AGENTS.md — UniXNU-liuqin

Device repository for Xiaomi Pad 6 Pro (`liuqin`, SM8475, 12GB/512GB) following official UniXNU Format 2 (`UniXNU-sheng` architecture).

## Rules

- Keep all bring-up tests non-destructive (`fastboot boot` only). Never flash `boot`, `vendor_boot`, `uefi`, or `xbl`.
- Keep SoC-wide register maps and XNU patches in `UniXNU-sm8475-common`, never duplicated here.
- Keep `LIUQIN_VENDOR_HW = 0` for HyperOS compatibility until matching stock UEFI modules are explicitly verified.
- Update `porting.toml` with hardware evidence after each real-device test and verify with `python3 ../UniXNU/scripts/porting-record.py check --device .`.

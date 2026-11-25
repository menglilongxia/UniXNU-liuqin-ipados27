# UniXNU-sm8475-common

Shared Qualcomm SM8475 (Snapdragon 8+ Gen 1, `cape`) SoC support for UniXNU Format 2 device repositories (`arm64e` iPadOS 27 / macOS 27 and `arm64` iPadOS 17).

- `soc.toml`: SoC metadata and architectural component selection (`ArmCpu`, `ArmDTPlatform`).
- `dts/sm8475.dtsi`: 8-core Kryo 780 topology (`4x A510 + 3x A710 + 1x X2`), PSCI SMC, GICv3 (`0x17100000`/`0x17180000` stride `0x40000`), UFS (`0x1d84000` / PHY `0x1d80000`), and disabled Adreno 730 / GMU (`0xb290000`) / SMMU nodes.
- `patches/xnu-12377/`: `sheng-platform-power.patch`, `native-smp-secondary.patch`, and `timers-avoid-trapped-physical-timer.patch` (required under Qualcomm Gunyah EL2 hypervisor).

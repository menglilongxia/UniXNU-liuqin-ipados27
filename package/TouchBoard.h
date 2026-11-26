// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board constants and clock-selection helpers for the Xiaomi Pad 6 Pro (liuqin, SM8475) NT36532 touch SPI bus.
// Every address, bit and pin comes from sm8450.dtsi, gcc-sm8450.c, pinctrl-sm8450.c and sm8475-xiaomi-liuqin.dts.
#ifndef SHENG_TOUCH_BOARD_H
#define SHENG_TOUCH_BOARD_H
#include <stdint.h>

// sm8450.dtsi + sm8475-xiaomi-liuqin.dts: touchscreen@0 is on spi4 inside qupv3_id_0@9c0000, engine reg 0x990000 size 0x4000.
#define TB_QUP_WRAPPER_BASE 0x009c0000ULL
#define TB_SE_BASE 0x00990000ULL
#define TB_SE_SIZE 0x4000ULL
// sm8450.dtsi: qupv3_id_0@9c0000 has iommus = <&apps_smmu 0x5a3 0x0>; apps_smmu@15000000.
#define TB_QUP_SMMU_SID 0x5a3
#define TB_APPS_SMMU_BASE 0x15000000ULL
#define TB_APPS_SMMU_SIZE 0xd0000ULL
// Interrupts from sm8450.dtsi: spi4 is GIC_SPI 605, TLMM summary is GIC_SPI 208.
#define TB_SPI_IRQ 605u
#define TB_TLMM_SUMMARY_IRQ 208u
// gcc-sm8450.c (Linux mainline): GCC at 0x100000.
#define TB_GCC_BASE 0x00100000ULL
#define TB_GCC_MAP_SIZE 0x70000ULL
#define TB_GCC_VOTE_ENABLE 0x62008            // enable_reg of every QUP wrap0 branch below
#define TB_GCC_GPLL0_ENABLE 0x62018           // gcc_gpll0 enable_reg, bit 0
#define TB_GCC_GPLL0_MODE 0x0
#define TB_VOTE_WRAP0_CORE_2X (1u<<9)         // gcc_qupv3_wrap0_core_2x_clk, halt 0x3300c
#define TB_VOTE_WRAP0_CORE (1u<<8)            // gcc_qupv3_wrap0_core_clk, halt 0x33000
#define TB_VOTE_WRAP0_M_AHB (1u<<6)           // gcc_qupv3_wrap_0_m_ahb_clk, halt 0x27004
#define TB_VOTE_WRAP0_S_AHB (1u<<7)           // gcc_qupv3_wrap_0_s_ahb_clk, halt 0x27008
#define TB_VOTE_WRAP0_S4 (1u<<14)             // gcc_qupv3_wrap0_s4_clk, halt 0x274dc
#define TB_VOTE_ALL (TB_VOTE_WRAP0_CORE_2X|TB_VOTE_WRAP0_CORE|TB_VOTE_WRAP0_M_AHB|TB_VOTE_WRAP0_S_AHB|TB_VOTE_WRAP0_S4)
#define TB_HALT_CORE_2X 0x3300c
#define TB_HALT_CORE 0x33000
#define TB_HALT_M_AHB 0x27004
#define TB_HALT_S_AHB 0x27008
#define TB_HALT_S4 0x274dc
#define TB_CBCR_CLK_OFF (1u<<31)
// gcc_qupv3_wrap0_s4_clk_src: cmd_rcgr 0x274e4, DFS-managed (clk-rcg2.c SE_* macros).
#define TB_S4_RCG 0x274e4
#define TB_RCG_CMD 0x0
#define TB_RCG_CFG 0x4
#define TB_RCG_DFSR_CMD 0x14                  // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l) (0x1c+4*(l))
#define TB_RCG_PERF_M(l) (0x5c+4*(l))
#define TB_RCG_PERF_N(l) (0x9c+4*(l))
#define TB_DFS_LEVELS 8
// TLMM (sm8450.dtsi pinctrl@f100000; pinctrl-sm8450.c: ctl_reg = 0x1000*gpio, io_reg = +4).
#define TB_TLMM_BASE 0x0f100000ULL
#define TB_GPIO_STRIDE 0x1000
#define TB_GPIO_SPI_MISO 16                   // sm8450 qup_spi4_data_clk: gpio16/17/18, qup_spi4_cs: gpio19
#define TB_GPIO_SPI_MOSI 17
#define TB_GPIO_SPI_CLK 18
#define TB_GPIO_SPI_CS 19
#define TB_GPIO_IRQ 21                        // novatek,irq-gpio = <&tlmm 21 IRQ_TYPE_EDGE_RISING>
#define TB_FUNC_QUP0_SE4 1                    // PINGROUP(16..19, qup4, ...): function slot 1
#define TB_DRIVE_6MA 2                        // drive-strength = <6>: 2 mA steps encoded as (mA/2)-1
#define TB_SPI_MODE 0                         // liuqin.dts touchscreen@0: spi-max-frequency 9600000, mode 0
#define TB_SPI_DEFAULT_HZ 9600000u            // board DT: touchscreen spi-max-frequency = <9600000>
#define TB_SPI_MAX_HZ 9600000u
// Panel dimensions (0.1 px units, matching NT36532 NVT_COORD_SCALE = 10 on 1800 x 2880).
#define TB_PANEL_X_MAX 18000
#define TB_PANEL_Y_MAX 28800

static inline uint32_t tb_pin_cfg(unsigned func, unsigned drive_code) {return (func&15u)<<2|(drive_code&7u)<<6;}   // pull disabled
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE;}
static inline uint32_t tb_tlmm_io(unsigned gpio) {return (uint32_t)gpio*TB_GPIO_STRIDE+4;}

// Source clock rates by RCG source-select value (gcc_parent_map_0: 0 = TCXO, 1 = GPLL0 main, 6 = GPLL0 even).
static inline uint64_t tb_src_hz(unsigned sel) {return sel==0?19200000ULL:sel==1?600000000ULL:sel==6?300000000ULL:0;}
// clk-rcg2.c calc_rate: half-integer divider, then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) {prate*=2;prate/=hid+1;}
    if (mode && n) {prate*=m;prate/=n;}
    return prate;
}
static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src=(cfg>>8)&7,mode=(cfg>>12)&3,hid=cfg&0x1f;
    uint32_t m=0,n=0;
    if (mode) {m=mreg&0xffff;n=((~nreg)&0xffff)+m;}
    return tb_calc_rate(tb_src_hz(src),m,n,mode,hid);
}
struct tb_clock_choice {unsigned level, div;uint64_t sclk_hz, actual_hz;};
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned os, unsigned target, struct tb_clock_choice *out) {
    int found=0;
    for (unsigned l=0;l<n;++l) {
        uint64_t f=level_hz[l];if (!f) continue;
        uint64_t d=((f+(uint64_t)os*target-1)/((uint64_t)os*target));if (d<1) d=1;if (d>0xfff) continue;
        uint64_t actual=f/(os*d);if (actual>target) continue;
        if (!found || actual>out->actual_hz || (actual==out->actual_hz && d<out->div)) {
            out->level=l;out->div=(unsigned)d;out->sclk_hz=f;out->actual_hz=actual;found=1;
        }
    }
    return found?0:-1;
}
#endif

/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8475 (Xiaomi Pad 6 Pro / liuqin) Adreno 730 cold GMU + CP bring-up.
 * Uses APSS DRV2 active TCS3 (AdspProbe uses TCS1), QSMMU V500 (ACTLR 0x32b),
 * Cape GPUCC LUCID_OLE PLLs, and Xiaomi liuqin-signed PAS13 ZAP firmware. */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "PongAdrenoGmu.h"
#include "PongAdrenoClocks.h"
#include "PongAdrenoSmmu.h"
#include "PongAdrenoRegisters.h"
#include "PongAdrenoCp.h"

extern "C" const uint8_t PongAdrenoGmuFirmware[];
extern "C" const size_t PongAdrenoGmuFirmwareBytes;
/* [0]=stage (0=idle,1=db,2=parents,3=clks,4=smmu,5=cp_prep,6=gmu,7=pwr,8=cp_ok,9=pub_ok),
 * [1]=last_result, [2]=gmu_core_ver, [3]=gmu_hfi_ver, [4]=hz_mhz, [5]=metal_stage, [6]=metal_status, [7]=metal_reg */
extern "C" {
volatile uint32_t native_liuqin_gpu_stats[8];
char native_liuqin_gpu_phase[64];
}

namespace {
struct Mapping {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t cpu = 0;
    size_t bytes = 0;
};
struct Memory {
    IOBufferMemoryDescriptor *keep = nullptr;
    uint8_t *cpu = nullptr;
    uint64_t pa = 0;
    size_t bytes = 0;
};

static Mapping windows[PongGpuWindowCount], gcc, rsc, db;
static Memory cache, dummy, hfi, logmem, tables;
static PongClockState clocks;
static PongGpuGmuDevice gmu;
static PongAdrenoSmmuSession context;
static ArmSmmu4KConfig table_config;
static ArmSmmu4KResult table_result;
static ArmSmmu4KRange ranges[4];
static unsigned char visited[32];
static ArmSmmu4KAuditRegion region;
static ArmSmmu4KAuditConfig audit;
static ArmSmmu4KContextConfig config;
static ArmSmmu4KIo smmu_io;
static uint64_t bank_offset;
static unsigned parents_ready, attempted;

static void record(const char *phase, unsigned result, uint64_t a = 0, uint64_t b = 0)
{
    native_liuqin_gpu_stats[1] = result;
    if (phase) {
        size_t n = strlen(phase);
        if (n >= sizeof(native_liuqin_gpu_phase)) n = sizeof(native_liuqin_gpu_phase) - 1;
        memcpy(native_liuqin_gpu_phase, phase, n);
        native_liuqin_gpu_phase[n] = '\0';
    }
    IOLog("LIUQIN ADRENO: %s result=%u a=0x%llx b=0x%llx\n",
          phase ? phase : "?", result, (unsigned long long)a, (unsigned long long)b);
}

static bool map(Mapping &m, uint64_t physical, size_t bytes)
{
    OSSharedPtr<IOMemoryDescriptor> memory(
        IOMemoryDescriptor::withPhysicalAddress(physical, bytes, kIODirectionInOut), OSNoRetain);
    if (memory)
        m.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    if (!m.keep) return false;
    m.cpu = m.keep->getVirtualAddress();
    m.bytes = bytes;
    return m.cpu != 0;
}

static uint32_t rd(const Mapping &m, size_t at)
{
    return *reinterpret_cast<volatile uint32_t *>(m.cpu + at);
}

static void wr(const Mapping &m, size_t at, uint32_t value)
{
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(m.cpu + at) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int barrier(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static uint64_t microseconds()
{
    clock_sec_t s;
    clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return uint64_t(s) * 1000000 + u;
}

static int now(void *, uint64_t *out)
{
    *out = microseconds();
    return 0;
}

static int wait(void *, unsigned us)
{
    IODelay(us);
    return 0;
}

static void clean(const void *memory, size_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t at = reinterpret_cast<uintptr_t>(memory);
    const uintptr_t end = at + bytes;
    for (at &= ~(line - 1); at < end; at += line)
        __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool allocate(Memory &m, size_t bytes)
{
    m.keep = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) | IOOptionBits(kIOMemoryMapperNone),
        bytes, 4096);
    if (!m.keep) return false;
    IOByteCount contiguous = 0;
    m.cpu = static_cast<uint8_t *>(m.keep->getBytesNoCopy());
    m.pa = m.keep->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    m.bytes = bytes;
    if (!m.cpu || !m.pa || (m.pa & 4095) || contiguous < bytes || m.pa + bytes > (UINT64_C(1) << 36))
        return false;
    bzero(m.cpu, bytes);
    clean(m.cpu, bytes);
    return true;
}

static bool poll(const Mapping &m, size_t at, uint32_t mask, uint32_t wanted, unsigned us)
{
    const uint64_t begin = microseconds();
    for (unsigned n = 0; n < us && microseconds() - begin < us; n++) {
        if ((rd(m, at) & mask) == wanted) return true;
        IODelay(1);
    }
    return false;
}

static unsigned retained_corner(const PongPowerResource &rail)
{
    unsigned index = 16, highest = 0;
    for (unsigned i = 0; i < rail.bytes / 2 && i < 16; i++) {
        const unsigned v = rail.aux[2 * i] | (unsigned(rail.aux[2 * i + 1]) << 8);
        if (v > highest) {
            highest = v;
            index = i;
        }
    }
    return index;
}

static bool vote_parents(const PongPowerInputs &in)
{
    const uint32_t caps = rd(rsc, 0xc);
    const unsigned count = (caps >> 12) & 63, ncpt = (caps >> 27) & 31;
    if (count < 8 || ncpt < 2) {
        record("SKIP APSS DRV2 capacity", 1, caps, (uint64_t(count) << 32) | ncpt);
        return false;
    }
    const size_t base = 0xd00 + 3 * 0x2a0;
    if (!rd(rsc, base + 0x18)) {
        record("SKIP active TCS3 busy", 1, rd(rsc, base + 0x14), rd(rsc, base + 0x1c));
        return false;
    }
    const uint32_t cx = retained_corner(in.cx);
    const uint32_t mx = retained_corner(in.mx);
    if (cx == 16 || mx == 16) {
        record("SKIP ARC AUX levels", 1, cx, mx);
        return false;
    }
    const size_t other = 0xd00 + 4 * 0x2a0;
    if (!rd(rsc, other + 0x18)) {
        const uint32_t enabled = rd(rsc, other + 0x1c);
        for (unsigned i = 0; i < ncpt && i < 16; i++) {
            if (enabled & (1u << i)) {
                const uint32_t address = rd(rsc, other + 0x34 + 20 * i);
                if (address == in.mx.address || address == in.cx.address) {
                    record("SKIP conflicting active TCS4", 1, address, enabled);
                    return false;
                }
            }
        }
    }
    wr(rsc, 0xd00, rd(rsc, 0xd00) & ~(1u << 3));
    uint32_t control = rd(rsc, base + 0x14) & ~(1u << 24);
    wr(rsc, base + 0x14, control);
    if (!poll(rsc, base + 0x14, ~0u, control, 500)) {
        record("FAIL TCS trigger clear", 1, rd(rsc, base + 0x14), control);
        return false;
    }
    control &= ~(1u << 16);
    wr(rsc, base + 0x14, control);
    if (!poll(rsc, base + 0x14, ~0u, control, 500)) {
        record("FAIL TCS enable clear", 1, rd(rsc, base + 0x14), control);
        return false;
    }
    wr(rsc, base + 0x1c, 0);
    wr(rsc, 0xd08, 1u << 3);
    if (!poll(rsc, 0xd04, 1u << 3, 0, 500)) {
        record("FAIL stale TCS completion clear", 1, rd(rsc, 0xd04));
        return false;
    }
    const uint32_t addresses[] = {in.mx.address, in.cx.address};
    const uint32_t values[] = {mx, cx};
    for (unsigned i = 0; i < 2; i++) {
        wr(rsc, base + 0x30 + 20 * i, 0x10108);
        wr(rsc, base + 0x34 + 20 * i, addresses[i]);
        wr(rsc, base + 0x38 + 20 * i, values[i]);
    }
    wr(rsc, base + 0x10, 3);
    wr(rsc, base + 0x1c, 3);
    wr(rsc, base + 0x14, 1u << 16);
    if (!poll(rsc, base + 0x14, ~0u, 1u << 16, 500)) {
        record("FAIL TCS AMC enable", 1, rd(rsc, base + 0x14));
        return false;
    }
    wr(rsc, base + 0x14, (1u << 16) | (1u << 24));
    bool complete = false, progressed = false;
    uint32_t first = 0, second = 0, idle_reg = 0;
    const uint64_t began = microseconds();
    for (unsigned n = 0; n < 10000 && microseconds() - began < 100000; n++) {
        first = rd(rsc, base + 0x3c);
        second = rd(rsc, base + 0x50);
        idle_reg = rd(rsc, base + 0x18);
        const bool commands = (first & 0x10100) == 0x10100 && (second & 0x10100) == 0x10100;
        if (!commands || !idle_reg) progressed = true;
        if (commands && idle_reg && (progressed || (rd(rsc, 0xd04) & (1u << 3)))) {
            complete = true;
            break;
        }
        IODelay(10);
    }
    if (!complete) {
        record("FAIL ARC completion", 1, (uint64_t(first) << 32) | second, idle_reg);
        return false;
    }
    wr(rsc, base + 0x1c, 0);
    wr(rsc, 0xd08, 1u << 3);
    wr(rsc, base + 0x14, rd(rsc, base + 0x14) & ~(1u << 24));
    record("parent ARC votes ok", 0, mx, cx);
    return true;
}

static int clock_read(void *, PongClockWindow w, size_t at, uint32_t *v)
{
    const Mapping &m = (w == PongClockGcc) ? gcc : windows[PongGpuClock];
    if (at > m.bytes - 4) return 1;
    *v = rd(m, at);
    return 0;
}

static int clock_write(void *, PongClockWindow w, size_t at, uint32_t v)
{
    const Mapping &m = (w == PongClockGcc) ? gcc : windows[PongGpuClock];
    if (at > m.bytes - 4) return 1;
    wr(m, at, v);
    return 0;
}

static int clock_prerequisite(void *)
{
    const uint64_t rate = 19200000ULL * ((rd(gcc, 0x10) & 0xffff) * 65536ULL + (rd(gcc, 0x14) & 0xffff)) / 65536ULL;
    const uint32_t mode = rd(gcc, 0), opmode = rd(gcc, 4);
    const bool locked = (mode & 0x80000000u) && (mode & 1u) && (opmode & 1u);
    return (!parents_ready || !locked || rate != 600000000ULL) ? 1 : 0;
}

static int smmu_read(void *, size_t at, uint32_t *v)
{
    if (at > windows[PongGpuSmmu].bytes - 4) return 1;
    *v = rd(windows[PongGpuSmmu], at);
    return 0;
}

static int smmu_read64(void *, size_t at, uint64_t *v)
{
    if (at > windows[PongGpuSmmu].bytes - 8) return 1;
    *v = *reinterpret_cast<volatile uint64_t *>(windows[PongGpuSmmu].cpu + at);
    return 0;
}

static int smmu_write(void *, size_t at, uint32_t v)
{
    if (at > windows[PongGpuSmmu].bytes - 4) return 1;
    wr(windows[PongGpuSmmu], at, v);
    return 0;
}

static int smmu_write64(void *, size_t at, uint64_t v)
{
    if (at > windows[PongGpuSmmu].bytes - 8) return 1;
    __asm__ volatile("dsb sy" ::: "memory");
    *reinterpret_cast<volatile uint64_t *>(windows[PongGpuSmmu].cpu + at) = v;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static int read_table(void *, uint64_t pa, uint64_t *out)
{
    if (pa < tables.pa || pa - tables.pa > tables.bytes - 8 || (pa & 7)) return 1;
    *out = *reinterpret_cast<const uint64_t *>(tables.cpu + (pa - tables.pa));
    return 0;
}

static bool start_smmu()
{
    const Mapping &m = windows[PongGpuSmmu];
    const uint32_t id0 = rd(m, 0x20), id1 = rd(m, 0x24), id2 = rd(m, 0x28);
    const unsigned groups = (id0 & 255) > PongAdrenoStreamSlots ? PongAdrenoStreamSlots : (id0 & 255);
    const unsigned banks = id1 & 255;
    const unsigned pages = 1u << (((id1 >> 28) & 7) + 1);
    const bool ex = (id0 >> 8) & 1;
    if ((id1 >> 31) || groups < 2 || banks < 2) return false;
    for (unsigned i = 0; i < groups; i++) {
        const uint32_t smr = rd(m, 0x800 + 4 * i), sc = rd(m, 0xc00 + 4 * i);
        if (!(ex ? !!(sc & 1024) : !!(smr & 0x80000000)) || (sc & 0x30000)) continue;
        const uint32_t mask = (smr >> 16) & (ex ? 65535 : 32767), sid = smr & 65535;
        if ((mask == 0 || mask == 0x400) && ((sid & ~0x400u) == 0 || (sid & ~0x400u) == 5)) continue;
        const unsigned b = sc & 255;
        if (b >= banks) return false;
        const size_t at = (pages + b) * 4096;
        if (!(rd(m, at) & 1)) continue;
        const uint64_t lower = *reinterpret_cast<volatile uint64_t *>(m.cpu + at + 0x20);
        const uint64_t upper = *reinterpret_cast<volatile uint64_t *>(m.cpu + at + 0x28);
        if ((lower >> 48) == 1 || (lower >> 48) == 2 || (upper >> 48) == 1 || (upper >> 48) == 2) {
            record("SKIP ASID conflict", 1, b, sid);
            return false;
        }
    }
    unsigned slots[2] = {~0u, ~0u}, found = 0;
    for (unsigned i = 0; i < groups; i++) {
        const uint32_t smr = rd(m, 0x800 + 4 * i), s2cr = rd(m, 0xc00 + 4 * i);
        const bool valid = ex ? !!(s2cr & 1024) : !!(smr & 0x80000000);
        if (!valid) continue;
        const uint32_t mask = (smr >> 16) & (ex ? 65535 : 32767), sid = smr & 65535;
        const bool matches = !((sid ^ 5) & ~mask & 65535) || !((sid ^ 0x405) & ~mask & 65535);
        if (!matches) continue;
        if ((mask != 0 && mask != 0x400) || (sid & ~0x400u) != 5) return false;
        wr(m, 0xc00 + 4 * i, 0x20000 | (ex ? 1024 : 0));
        if (rd(m, 0xc00 + 4 * i) != (0x20000u | (ex ? 1024u : 0u))) return false;
        if (found < 2) slots[found++] = i;
        else return false;
    }
    for (unsigned i = 0; i < groups && found < 2; i++) {
        const uint32_t smr = rd(m, 0x800 + 4 * i), s2cr = rd(m, 0xc00 + 4 * i);
        const bool valid = ex ? !!(s2cr & 1024) : !!(smr & 0x80000000);
        if (!valid && i != slots[0]) slots[found++] = i;
    }
    if (found != 2) return false;
    unsigned bank = 0;
    for (unsigned b = 1; b < banks; b++) {
        bool other = false;
        for (unsigned i = 0; i < groups; i++) {
            const uint32_t smr = rd(m, 0x800 + 4 * i), sc = rd(m, 0xc00 + 4 * i);
            if ((ex ? !!(sc & 1024) : !!(smr & 0x80000000)) && !(sc & 0x30000) && (sc & 255) == b)
                other = true;
        }
        if (!other) {
            bank = b;
            break;
        }
    }
    if (!bank) return false;
    bank_offset = uint64_t(pages + bank) * 4096;
    wr(m, bank_offset, 0);
    if (rd(m, bank_offset) != 0) return false;
    wr(m, bank_offset + 0x618, 0);
    wr(m, bank_offset + 0x7f0, 0);
    if (!poll(m, bank_offset + 0x7f4, 1, 0, 2000) || (rd(m, bank_offset + 0x58) & 0xc00001feu))
        return false;
    wr(m, bank_offset + 4, 0x32b);
    if (rd(m, bank_offset + 4) != 0x32b) return false;
    const uint32_t scr = (rd(m, 0) & ~13u) | 0x402u | (ex ? 8u : 0u);
    wr(m, 0, scr);
    for (unsigned i = 0; i < 2; i++) {
        wr(m, 0xc00 + 4 * slots[i], 0x20000);
        wr(m, 0x800 + 4 * slots[i], (ex ? 0 : 0x80000000u) | (i ? 0x405u : 5u));
        wr(m, 0xc00 + 4 * slots[i], 0x20000 | (ex ? 1024 : 0));
    }
    if (!allocate(cache, 0xffc000) || !allocate(dummy, 0x2000) || !allocate(hfi, 0x4000) ||
        !allocate(logmem, 0x4000) || !allocate(tables, 32 * 4096))
        return false;
    Memory *shared_pages[] = {&dummy, &hfi, &logmem};
    for (Memory *shared : shared_pages) {
        const NativeAdrenoPhysicalSpan span = {0, shared->pa, shared->bytes};
        if (NativeAdrenoSetPagesCacheMode(&span, 1, true)) return false;
    }
    Memory *mem[] = {&cache, &dummy, &hfi, &logmem};
    const uint64_t iovas[] = {0x4000, 0x60000000, 0x60002000, 0x60006000};
    for (unsigned i = 0; i < 4; i++) ranges[i] = {iovas[i], mem[i]->pa, mem[i]->bytes, i ? 3u : 15u, 0, 2};
    table_config = {32, 36, tables.pa, 8192};
    if (ArmSmmu4KBuild(&table_config, ranges, 4, reinterpret_cast<uint64_t *>(tables.cpu), 32, &table_result))
        return false;
    clean(tables.cpu, tables.bytes);
    region = {ranges, 4, 32, 32 * 512, visited, sizeof(visited)};
    audit = {{nullptr, read_table}, &region, nullptr};
    config = {};
    config.lower = &table_config;
    config.lower_tables = &table_result;
    config.id0 = id0;
    config.id2 = id2;
    config.asid = 1;
    config.continue_after_fault = 1;
    smmu_io = {nullptr, m.bytes, smmu_read, smmu_read64, smmu_write, smmu_write64, barrier};
    PongAdrenoSmmuResult result = {};
    const auto status = PongAdrenoSmmuStart(&smmu_io, 0x03da0000, PongAdrenoSmmuGmu, bank, slots,
                                            &config, &audit, 4096, &context, &result);
    record("GMU native4K context", status, bank, result.phase);
    return status == ArmSmmu4KSuccess;
}

static int healthy(void *)
{
    if (!parents_ready || !clocks.ready || !context.ready) return 1;
    if (rd(windows[PongGpuSmmu], 0x48) & 0x3ffu) return 1;
    if (rd(windows[PongGpuSmmu], bank_offset + 0x58) & 0xc00001feu) return 1;
    return 0;
}

static int gpu_read(void *, unsigned w, size_t at, uint32_t *v)
{
    if (w >= PongGpuWindowCount || at > windows[w].bytes - 4) return 1;
    *v = rd(windows[w], at);
    return 0;
}

static int gpu_write(void *, unsigned w, size_t at, uint32_t v)
{
    if (w >= PongGpuWindowCount || at > windows[w].bytes - 4) return 1;
    wr(windows[w], at, v);
    return 0;
}

static int publish(void *, void *owned_cpu, size_t bytes)
{
    clean(owned_cpu, bytes);
    return 0;
}

static int cp_read(void *, uint32_t reg, uint32_t *value)
{
    const uint64_t pa = 0x03d00000ULL + uint64_t(reg) * 4;
    const unsigned order[] = {PongGpuCore, PongGpuAo, PongGpuRscc, PongGpuGmu};
    for (unsigned w : order) {
        if (pa >= PongAdrenoWindows[w].physical && pa + 4 <= PongAdrenoWindows[w].physical + windows[w].bytes)
            return gpu_read(nullptr, w, size_t(pa - PongAdrenoWindows[w].physical), value);
    }
    return 1;
}

static int cp_write(void *, uint32_t reg, uint32_t value)
{
    const uint64_t pa = 0x03d00000ULL + uint64_t(reg) * 4;
    const unsigned order[] = {PongGpuCore, PongGpuAo, PongGpuRscc, PongGpuGmu};
    for (unsigned w : order) {
        if (pa >= PongAdrenoWindows[w].physical && pa + 4 <= PongAdrenoWindows[w].physical + windows[w].bytes)
            return gpu_write(nullptr, w, size_t(pa - PongAdrenoWindows[w].physical), value);
    }
    return 1;
}

static void run(void *, wait_result_t)
{
    unsigned power_level = 0; /* SM8475 peak: 900 MHz + DDR level 11 */
    PE_parse_boot_argn("liuqin-gpu-pwrlevel", &power_level, sizeof power_level);
    if (power_level >= PongAdrenoPowerLevelCount) {
        record("SKIP invalid pwrlevel", power_level);
        return;
    }
    native_liuqin_gpu_stats[0] = 1;
    if (!map(gcc, 0x100000, 0x1f4200) || !map(rsc, 0x17a20000, 0x10000) || !map(db, 0x80860000, 0x20000)) {
        record("SKIP resource map", 1);
        return;
    }
    PongPowerInputs inputs = {};
    const auto input_status = PongAdrenoReadPowerInputs(
        reinterpret_cast<const uint8_t *>(db.cpu), db.bytes, PongAdrenoChipId, &inputs);
    if (input_status) {
        record("SKIP command-db", input_status);
        return;
    }
    native_liuqin_gpu_stats[0] = 2;
    if (!vote_parents(inputs)) {
        record("FAIL parent votes", 1);
        return;
    }
    parents_ready = 1;
    wr(gcc, 0x81004, rd(gcc, 0x81004) | 1);
    IODelay(10);
    if (!(rd(gcc, 0x81004) & 1)) {
        record("FAIL CFG AHB", 1);
        return;
    }
    if (!map(windows[PongGpuClock], 0x03d90000, 0xa000)) {
        record("SKIP GPUCC map", 1);
        return;
    }
    const uint32_t gx = rd(windows[PongGpuClock], 0x905c);
    if (gx & 0x80000000) {
        record("SKIP GX already on", 1, gx);
        return;
    }
    native_liuqin_gpu_stats[0] = 3;
    PongClockOps clock_ops = {nullptr, clock_read, clock_write, barrier, now, wait, clock_prerequisite};
    PongClockResult cr = {};
    const auto cs = PongAdrenoEnableCxClocks(&clock_ops, &clocks, &cr);
    record("CX clocks", cs, (uint64_t(cr.offset) << 32) | cr.value, cr.phase);
    if (cs) return;
    for (unsigned i = 0; i < PongGpuWindowCount; i++) {
        if (i != PongGpuClock && !map(windows[i], PongAdrenoWindows[i].physical, PongAdrenoWindows[i].bytes)) {
            record("SKIP GPU window map", 1, i);
            return;
        }
    }
    const uint32_t reset = rd(windows[PongGpuGmu], GEN7_GMU_CM3_SYSRESET * 4 - 0x68000);
    if (reset != 1) {
        record("SKIP CM3 not in reset", 1, reset);
        return;
    }
    native_liuqin_gpu_stats[0] = 4;
    if (!start_smmu()) {
        record("FAIL GMU SMMU", 1);
        return;
    }
    native_liuqin_gpu_stats[0] = 5;
    PongCpOps cp = {{nullptr, cp_write, cp_read, barrier, nullptr, nullptr}, smmu_io, record};
    const auto prepared = PongAdrenoCpPrepare(&cp);
    if (prepared) {
        record("FAIL GPU ctx prep", unsigned(prepared));
        return;
    }
    native_liuqin_gpu_stats[0] = 6;
    gmu.io = {nullptr, healthy, gpu_read, gpu_write, barrier, publish, now, wait};
    PongGpuGmuConfig gc = {
        PongAdrenoChipId, PongAdrenoGmuFirmware, PongAdrenoGmuFirmwareBytes,
        {reinterpret_cast<volatile uint32_t *>(cache.cpu), cache.bytes, 0x4000},
        {reinterpret_cast<volatile uint32_t *>(dummy.cpu), dummy.bytes, 0x60000000},
        {reinterpret_cast<volatile uint32_t *>(hfi.cpu), hfi.bytes, 0x60002000},
        {reinterpret_cast<volatile uint32_t *>(logmem.cpu), logmem.bytes, 0x60006000}
    };
    PongGpuGmuResult gr = {};
    const auto gs = PongAdrenoGmuStart(&gmu, &gc, &gr);
    native_liuqin_gpu_stats[2] = gr.core_version;
    native_liuqin_gpu_stats[3] = gr.hfi_version;
    record("GMU fw/HFI", gs, (uint64_t(gr.last_register) << 32) | gr.last_value, gr.phase);
    if (gs) return;
    native_liuqin_gpu_stats[0] = 7;
    PongGpuPowerResult pr = {};
    const auto ps = PongAdrenoGmuStartPower(&gmu, &inputs, power_level, &pr);
    record("GMU power", ps, (uint64_t(pr.command) << 32) | pr.firmware_error, pr.acknowledged);
    if (ps) return;
    native_liuqin_gpu_stats[4] = PongAdrenoPowerLevels[power_level].hz / 1000000u;
    record("GMU freq/bw ok", 0, PongAdrenoPowerLevels[power_level].hz, PongAdrenoPowerLevels[power_level].bus);
    native_liuqin_gpu_stats[0] = 8;
    const auto cp_status = PongAdrenoCpStart(&gmu);
    record("CP/Metal startup", unsigned(cp_status), gr.core_version, gr.hfi_version);
    if (!cp_status) native_liuqin_gpu_stats[0] = 9;
}

static void worker(void *parameter, wait_result_t result)
{
    thread_set_thread_name(current_thread(), "liuqin-gpu");
    run(parameter, result);
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
} // namespace

extern "C" void liuqin_adreno_start_optional(void)
{
    unsigned requested = 0;
    if (!PE_parse_boot_argn("gpu-power", &requested, sizeof(requested)) || requested < 9) return;
    if (__atomic_exchange_n(&attempted, 1, __ATOMIC_ACQ_REL)) return;
    thread_t thread = THREAD_NULL;
    const auto status = kernel_thread_start(worker, nullptr, &thread);
    if (status != KERN_SUCCESS) {
        record("FAIL gpu thread", static_cast<unsigned>(status));
        return;
    }
    thread_deallocate(thread);
}

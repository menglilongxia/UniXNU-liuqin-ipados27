/* SPDX-License-Identifier: MIT */
/* SM8475 Adreno 730 CP, ZAP (PAS 13), QSMMU V500 context, and SPI 300 IRQ
 * bring-up wired to UniXNU's upstream NativeAdreno runtime. */
#include "PongAdrenoCp.h"
#include "PongAdrenoCpProfile.h"
#include "PongAdrenoZap.h"
#include "NativeAdrenoSmmuContext.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoPwrup.h"
#include "AdrenoPM4.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" const uint8_t PongAdrenoSqeFirmware[], PongAdrenoZapMdt[], PongAdrenoZapCode[], PongAdrenoZapSignature[];
extern "C" const size_t PongAdrenoSqeFirmwareBytes, PongAdrenoZapMdtBytes, PongAdrenoZapCodeBytes, PongAdrenoZapSignatureBytes;
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" uint64_t (*native_adreno_vm_now_us)(void);
extern "C" void native_adreno_gpu_interrupt(uint32_t status) __attribute__((weak));
extern "C" IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
extern "C" int get_preemption_level(void);

namespace {
constexpr uint64_t iovas[] = {0x100000, 0x200000, 0x300000, 0x301000, 0x302000};
constexpr size_t sizes[] = {0x13000, 0x8000, 0x1000, 0x1000, 0x1000};
/* SM8475 DT gfx3d_user: SID 0 / mask 0x400; gfx3d_lpac: SID 1 / mask 0x400.
 * SQE boots LPAC before consuming the first PM4 packet; both share bank 0.
 * SID 2 / 0x402 belongs to gfx3d_secure and is never leased. */
constexpr uint16_t gpu_sids[] = {0, 0x400, 1, 0x401};
constexpr unsigned gpu_stream_count = sizeof(gpu_sids) / sizeof(*gpu_sids);
constexpr uint32_t SmmuContextFaultMask = 0xc00001feu;

struct Device {
    PongCpOps ops{};
    NativeAdrenoTablePages tables;
    NativeAdrenoMemory buffers[5], metadata;
    NativeAdrenoSmmuContext context;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoCpInit init;
    NativeAdrenoSubmit submit;
    NativeAdrenoTimer timer;
    NativeAdrenoStatus status;
    NativeAdrenoIrq irq;
    IOLock *scm_lock = nullptr;
    uint32_t irq_fatal = 0, irq_count = 0, irq_cp_status = 0, irq_protect_status = 0;
    PongCpBackend backend{};
    OSSharedPtr<IOMemoryMap> zap_map;
    PongZapState zap{};
    unsigned output_bits = 0;
    bool prepared = false, started = false, poisoned = false, irq_attached = false;
};

Device *gpu = nullptr;

static uint64_t now(void *)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
static uint64_t vm_now() { return now(nullptr); }

static int idle(void *)
{
    if (gpu->irq_attached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && !get_preemption_level()) {
        const uint32_t before = __atomic_load_n(&gpu->irq_count, __ATOMIC_ACQUIRE);
        assert_wait_timeout(event_t(&gpu->irq_count), THREAD_UNINT, 1, NSEC_PER_MSEC);
        if (__atomic_load_n(&gpu->irq_count, __ATOMIC_ACQUIRE) != before)
            thread_wakeup(event_t(&gpu->irq_count));
        thread_block(THREAD_CONTINUE_NULL);
    } else {
        IODelay(20);
    }
    return 0;
}

static void note(const char *phase, unsigned result, uint64_t a = 0, uint64_t b = 0)
{
    gpu->ops.record(phase, result, a, b);
}

static uint32_t rd(uint32_t reg)
{
    uint32_t v = 0;
    if (gpu->ops.registers.read32(gpu->ops.registers.cookie, reg, &v)) gpu->poisoned = true;
    return v;
}
static bool wr(uint32_t reg, uint32_t value)
{
    return !gpu->ops.registers.write32(gpu->ops.registers.cookie, reg, value);
}
static bool wr64(uint32_t reg, uint64_t value)
{
    return wr(reg, uint32_t(value)) && wr(reg + 1, uint32_t(value >> 32));
}
static uint32_t sr(size_t at)
{
    uint32_t v = 0;
    if (gpu->ops.smmu.read32(gpu->ops.smmu.cookie, at, &v)) gpu->poisoned = true;
    return v;
}
static bool sw(size_t at, uint32_t v)
{
    return !gpu->ops.smmu.write32(gpu->ops.smmu.cookie, at, v);
}

static uint32_t classifyIrq(uint32_t status, uint32_t cp_status)
{
    constexpr uint32_t kKnown = (1u << 0) | (1u << 1) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) |
        (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15) | (1u << 16) |
        (1u << 17) | (1u << 18) | (1u << 20) | (1u << 21) | (1u << 22) | (1u << 23) | (1u << 24) |
        (1u << 25) | (1u << 28) | (1u << 29) | (1u << 30) | (1u << 31);
    uint32_t flags = 0;
    if (status & (1u << 20)) flags |= NativeAdrenoIrqFence;
    if (status & (1u << 8)) flags |= NativeAdrenoIrqPreempt;
    if (status & (1u << 0)) flags |= NativeAdrenoIrqIdle;
    if (status & (1u << 23)) flags |= NativeAdrenoIrqHang;
    if (status & ((1u << 24) | (1u << 25))) flags |= NativeAdrenoIrqUche;
    if (status & ((1u << 1) | (1u << 6) | (1u << 7) | (1u << 22))) flags |= NativeAdrenoIrqBusError;
    if (status & ((1u << 28) | (1u << 29))) flags |= NativeAdrenoIrqSecurity;
    if ((status & (1u << 9)) && (cp_status & NativeAdrenoIrq::cpErrorMask())) flags |= NativeAdrenoIrqCpError;
    if (status & ~kKnown) flags |= NativeAdrenoIrqUnknown;
    if (flags & (NativeAdrenoIrqHang | NativeAdrenoIrqCpError | NativeAdrenoIrqUche |
                 NativeAdrenoIrqBusError | NativeAdrenoIrqSecurity | NativeAdrenoIrqUnknown))
        flags |= NativeAdrenoIrqFatal;
    return flags;
}

static bool faulted()
{
    if (!gpu || gpu->poisoned) return true;
    const auto *p = gpu->context.programResult();
    const uint32_t fsr = sr(p->context_offset + 0x58);
    const uint32_t global = sr(0x48);
    const uint32_t irq = rd(GEN7_RBBM_INT_0_STATUS);
    const uint32_t cp = rd(GEN7_CP_HW_FAULT);
    const uint32_t cp_status = (irq & (1u << 9)) ? rd(GEN7_CP_INTERRUPT_STATUS) : 0;
    const uint32_t captured = __atomic_load_n(&gpu->irq_fatal, __ATOMIC_ACQUIRE);
    const bool fatal_irq = (classifyIrq(irq, cp_status) & NativeAdrenoIrqFatal) != 0;
    if ((fsr & SmmuContextFaultMask) || (global & 0x3ffu) || fatal_irq || captured || cp) {
        gpu->poisoned = true;
        note("CP/SMMU fault", 1, (uint64_t(fsr) << 32) | global, (uint64_t(irq | captured) << 32) | cp);
        const uint32_t cause = cp_status | __atomic_load_n(&gpu->irq_cp_status, __ATOMIC_ACQUIRE);
        const uint32_t protect = __atomic_load_n(&gpu->irq_protect_status, __ATOMIC_ACQUIRE) |
            ((cause & (1u << 4)) ? rd(GEN7_CP_PROTECT_STATUS) : 0);
        note("CP cause/protect", cause, (uint64_t(protect) << 32) | rd(GEN7_CP_BV_PROTECT_STATUS),
             (uint64_t(rd(GEN7_CP_RB_RPTR)) << 32) | rd(GEN7_CP_RB_WPTR));
        if (fsr & SmmuContextFaultMask)
            note("SMMU fault FAR", fsr, (uint64_t(sr(p->context_offset + 0x64)) << 32) | sr(p->context_offset + 0x60),
                 (uint64_t(sr(p->context_offset + 0x68)) << 32) | sr(p->context_offset + 0x6c));
    }
    return gpu->poisoned;
}

static bool client_healthy() { return gpu && gpu->started && !faulted(); }

static void interrupt(void *)
{
    NativeAdrenoIrqResult result = {};
    const IOReturn status = gpu->irq.handle(&result);
    __atomic_fetch_add(&gpu->irq_count, 1, __ATOMIC_RELAXED);
    thread_wakeup(event_t(&gpu->irq_count));
    if (native_adreno_gpu_interrupt && !status)
        native_adreno_gpu_interrupt(result.status);
    if (status || (result.flags & NativeAdrenoIrqFatal)) {
        __atomic_fetch_or(&gpu->irq_cp_status, result.cp_status, __ATOMIC_RELAXED);
        __atomic_fetch_or(&gpu->irq_protect_status, result.protect_status, __ATOMIC_RELAXED);
        __atomic_fetch_or(&gpu->irq_fatal, result.status ? result.status : 0x80000000u, __ATOMIC_RELEASE);
        gpu->ops.registers.write32(gpu->ops.registers.cookie, GEN7_RBBM_INT_0_MASK, 0);
        gpu->ops.registers.barrier(gpu->ops.registers.cookie);
    }
}

static int publish_ring(void *, const volatile uint32_t *words, size_t bytes)
{
    const auto &m = gpu->buffers[1];
    const auto base = reinterpret_cast<uintptr_t>(m.mapping());
    const auto at = reinterpret_cast<uintptr_t>(words);
    if (at < base || at - base > m.bytes() || bytes > m.bytes() - (at - base)) return 1;
    const auto result = gpu->buffers[1].publishForDevice(at - base, bytes, false);
    __asm__ volatile("dsb sy" ::: "memory");
    return result != kIOReturnSuccess;
}

static int acquire_fence(void *, volatile uint32_t *words, size_t bytes)
{
    const auto &m = gpu->buffers[2];
    const auto base = reinterpret_cast<uintptr_t>(m.mapping());
    const auto at = reinterpret_cast<uintptr_t>(words);
    if (at < base || at - base > m.bytes() || bytes > m.bytes() - (at - base)) return 1;
    const auto result = gpu->buffers[2].acquireDeviceWrites(at - base, bytes);
    __asm__ volatile("dsb sy" ::: "memory");
    return result != kIOReturnSuccess;
}

static bool wait_fence(uint32_t fence)
{
    const NativeAdrenoRingClock clock{nullptr, now, idle};
    uint32_t observed = 0;
    const auto result = gpu->ring.wait(fence, &clock, 500000, &observed);
    note("CP fence", result, fence, observed);
    if (result != NativeAdrenoRingFenceComplete) {
        note("CP fence fail", rd(GEN7_RBBM_INT_0_STATUS),
             (uint64_t(rd(GEN7_CP_RB_RPTR)) << 32) | rd(GEN7_CP_RB_WPTR),
             (uint64_t(rd(GEN7_RBBM_STATUS)) << 32) | rd(GEN7_CP_HW_FAULT));
    }
    const bool failed = faulted();
    return result == NativeAdrenoRingFenceComplete && !failed;
}

static bool wait_bootstrap(uint32_t wptr, const char *phase)
{
    const uint64_t begin = now(nullptr);
    while (now(nullptr) - begin < 2000000) {
        if (faulted()) return false;
        if (rd(GEN7_CP_RB_RPTR) == wptr && !(rd(GEN7_RBBM_STATUS) & (1u << 23))) {
            note(phase, 0, wptr, rd(GEN7_RBBM_STATUS));
            return true;
        }
        IOSleep(1);
    }
    note(phase, 1, (uint64_t(rd(GEN7_CP_RB_RPTR)) << 32) | rd(GEN7_CP_RB_WPTR),
         (uint64_t(rd(GEN7_RBBM_STATUS)) << 32) | rd(GEN7_CP_HW_FAULT));
    (void)faulted();
    return false;
}

static int scm(void *, uint32_t fn, uint32_t info, const uint64_t args[3], int32_t *transport, int64_t *result)
{
    if (!gpu || !gpu->scm_lock) return 1;
    IOLockLock(gpu->scm_lock);
    uint64_t first = fn, a6 = 0;
    const uint64_t begin = now(nullptr);
    unsigned busy = 0;
    int rc = 1;
    for (unsigned resumes = 0; resumes < 10000 && now(nullptr) - begin < 1000000;) {
        register uint64_t x0 __asm__("x0") = first;
        register uint64_t x1 __asm__("x1") = info;
        register uint64_t x2 __asm__("x2") = args[0];
        register uint64_t x3 __asm__("x3") = args[1];
        register uint64_t x4 __asm__("x4") = args[2];
        register uint64_t x5 __asm__("x5") = 0;
        register uint64_t x6 __asm__("x6") = a6;
        register uint64_t x7 __asm__("x7") = 0;
        __asm__ volatile("smc #0"
            : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
            :
            : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {
            first = (fn & 0x40000000u) ? 0xc2000001u : 0x82000001u;
            a6 = x6;
            ++resumes;
            continue;
        }
        if (int64_t(x0) == -2 && ++busy < 100) {
            first = fn;
            a6 = 0;
            IOSleep(1);
            continue;
        }
        *transport = int32_t(x0);
        *result = int64_t(x1);
        rc = 0;
        break;
    }
    IOLockUnlock(gpu->scm_lock);
    return rc;
}

static int zap_ready(void *) { return gpu->ring.outstanding() || faulted(); }
static int zap_publish(void *, const PongZapMemory *, size_t)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

static bool authorize()
{
    /* On liuqin, XNU's owned RAM starts at 0x840000000; 0x8b91a000/0x2000 is
     * Qualcomm's reserved gpu_micro_code_mem outside XNU's DRAM window. */
    if (gpu->metadata.allocate(4096, gpu->output_bits, true, 4096) || gpu->metadata.makeWriteCombined())
        return false;
    OSSharedPtr<IOMemoryDescriptor> memory(
        IOMemoryDescriptor::withPhysicalAddress(PongZapReservedPhysical, PongZapReservedBytes, kIODirectionInOut),
        OSNoRetain);
    if (memory)
        gpu->zap_map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
    if (!gpu->zap_map) return false;
    PongZapConfig c = {
        PongAdrenoZapMdt, PongAdrenoZapMdtBytes,
        PongAdrenoZapSignature, PongAdrenoZapSignatureBytes, 2,
        PongAdrenoZapCode, PongAdrenoZapCodeBytes, 1,
        {static_cast<volatile uint8_t *>(gpu->metadata.mapping()), gpu->metadata.spans()[0].physical, 4096},
        {reinterpret_cast<volatile uint8_t *>(gpu->zap_map->getVirtualAddress()), PongZapReservedPhysical, PongZapReservedBytes}
    };
    PongZapOps ops = {nullptr, 64, zap_ready, zap_publish, scm};
    PongZapResult r = {};
    const auto status = PongAdrenoStartZap(&ops, &c, &gpu->zap, &r);
    note("PAS13 ZAP auth", status, (uint64_t(r.phase) << 32) | uint32_t(r.transport), uint64_t(r.secure_result));
    return status == PongZapSuccess;
}

static bool hardware_init()
{
    if (!wr(GEN7_GBIF_HALT, 0) || !wr(GEN7_RBBM_GBIF_HALT, 0)) return false;
    const uint32_t cx = rd(GEN7_GBIF_HALT), gx = rd(GEN7_RBBM_GBIF_HALT);
    note("GBIF released", 0, (uint64_t(cx) << 32) | gx,
         (uint64_t(rd(GEN7_GBIF_HALT_ACK)) << 32) | rd(GEN7_RBBM_GBIF_HALT_ACK));
    if (!wr(GEN7_RBBM_SECVID_TSB_CNTL, 0) || !wr64(GEN7_RBBM_SECVID_TSB_TRUSTED_BASE_LO, 0) ||
        !wr(GEN7_RBBM_SECVID_TSB_TRUSTED_SIZE, 0))
        return false;
    for (const auto &p : PongCpGbif)
        if (!wr(p[0], p[1])) return false;
    if (!wr(GEN7_RBBM_INT_0_MASK, 0) || !wr(GEN7_UCHE_GBIF_GX_CONFIG, 0x10240e0) ||
        !wr(GEN7_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff) ||
        !wr64(GEN7_UCHE_TRAP_BASE_LO, 0x1fffffffff000ULL) ||
        !wr64(GEN7_UCHE_WRITE_THRU_BASE_LO, 0x1fffffffff000ULL) ||
        !wr(GEN7_UCHE_CACHE_WAYS, 0x800000) || !wr(GEN7_UCHE_CMDQ_CONFIG, 0x6690e) ||
        !wr(GEN7_CP_AHB_CNTL, 1) || !wr(GEN7_RBBM_PERFCTR_CNTL, 1))
        return false;
    const uint32_t bank = (PongAdrenoHighestBankBit - 13) & 3;
    if (!wr(GEN7_RB_NC_MODE_CNTL, (1u << 11) | (1u << 4) | (bank << 1)) ||
        !wr(GEN7_TPL1_NC_MODE_CNTL, bank << 1) || !wr(GEN7_SP_NC_MODE_CNTL, (2u << 4) | (bank << 1)) ||
        !wr(GEN7_UCHE_MODE_CNTL, bank << 21) ||
        !wr(GEN7_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0xcfffff) || !wr(GEN7_UCHE_CLIENT_PF, 0x81) ||
        !wr(GEN7_RB_CMP_DBG_ECO_CNTL, rd(GEN7_RB_CMP_DBG_ECO_CNTL) | (1u << 11)) ||
        !wr(GEN7_CP_PROTECT_CNTL, 11))
        return false;
    for (uint32_t pipe = 1; pipe <= 2; pipe++)
        if (!wr(GEN7_CP_APERTURE_CNTL_HOST, pipe << 12) || !wr(GEN7_GRAS_NC_MODE_CNTL, bank << 5))
            return false;
    if (!wr(GEN7_CP_APERTURE_CNTL_HOST, 0) || !wr(GEN7_RBBM_NC_MODE_CNTL, 1)) return false;
    for (const auto &p : PongCpProtect)
        if (!wr(p[0], p[1])) return false;
    if (!wr(GEN7_CP_APRIV_CNTL, 0x6f) || !wr(GEN7_CP_BV_APRIV_CNTL, 0xf) || !wr(GEN7_CP_LPAC_APRIV_CNTL, 0xf) ||
        !wr(GEN7_CP_CHICKEN_DBG, 1) || !wr(GEN7_CP_BV_CHICKEN_DBG, 1) || !wr(GEN7_CP_LPAC_CHICKEN_DBG, 1) ||
        !wr(GEN7_GPU_GMU_AO_GMU_CGC_MODE_CNTL, 0x20000) || !wr(GEN7_GPU_GMU_AO_GMU_CGC_DELAY_CNTL, 0x10111) ||
        !wr(GEN7_GPU_GMU_AO_GMU_CGC_HYST_CNTL, 0x5555))
        return false;
    for (const auto &p : PongCpHwcg)
        if (!wr(p[0], p[1])) return false;
    if (!wr(GEN7_RBBM_CLOCK_CNTL, 0x8aa8aa82) || rd(GEN7_RBBM_CLOCK_CNTL) != 0x8aa8aa82) return false;
    constexpr unsigned count = sizeof(PongCpRestore) / sizeof(*PongCpRestore);
    uint32_t values[count] = {};
    for (unsigned i = 0; i < count; i++) values[i] = rd(PongCpRestore[i]);
    const uint32_t dynamic[] = {0, GEN7_RBBM_PERFCTR_CNTL, 1};
    size_t written = 0;
    const NativeAdrenoPwrupCounts counts = {0, count, 1};
    if (NativeAdrenoPwrupList::pack(counts, nullptr, nullptr, PongCpRestore, values, dynamic,
                                    static_cast<uint8_t *>(gpu->buffers[3].mapping()), sizes[3], &written))
        return false;
    __asm__ volatile("dsb sy" ::: "memory");
    return !gpu->poisoned && !gpu->buffers[3].publishForDevice(0, sizes[3], false);
}
} // namespace

IOReturn PongAdrenoCpPrepare(const PongCpOps *ops)
{
    if (gpu || !ops || !ops->record) return kIOReturnBadArgument;
    gpu = new Device;
    if (!gpu) return kIOReturnNoMemory;
    gpu->scm_lock = IOLockAlloc();
    if (!gpu->scm_lock) return kIOReturnNoMemory;
    gpu->ops = *ops;
    native_adreno_vm_now_us = vm_now;
    const uint32_t id0 = sr(0x20), id1 = sr(0x24), id2 = sr(0x28);
    const unsigned count = (id0 & 255) > PongAdrenoStreamSlots ? PongAdrenoStreamSlots : (id0 & 255);
    const unsigned pages = 1u << (((id1 >> 28) & 7) + 1);
    const bool ex = (id0 >> 8) & 1;
    const uint8_t bits[] = {32, 36, 40, 42, 44, 48};
    const unsigned code = (id2 >> 4) & 15;
    if ((id1 >> 31) || count < gpu_stream_count || (id1 & 255) < 2 || code >= 6) return kIOReturnUnsupported;
    gpu->output_bits = bits[code] > 40 ? 40 : bits[code];
    unsigned slots[gpu_stream_count] = {~0u, ~0u, ~0u, ~0u}, found = 0;
    for (unsigned i = 0; i < count; i++) {
        const uint32_t smr = sr(0x800 + 4 * i), sc = sr(0xc00 + 4 * i);
        const bool valid = ex ? !!(sc & 1024) : !!(smr & 0x80000000);
        if (!valid) continue;
        const uint32_t mask = (smr >> 16) & (ex ? 65535 : 32767), sid = smr & 65535;
        bool matches = false;
        for (uint16_t wanted : gpu_sids) matches |= !((sid ^ wanted) & ~mask & 65535);
        if (matches) {
            if ((mask != 0 && mask != 0x400) || (sid & ~0x401u) || found == gpu_stream_count) return kIOReturnBusy;
            slots[found++] = i;
            if (!sw(0xc00 + 4 * i, 0x20000 | (ex ? 1024 : 0))) return kIOReturnIOError;
        } else if (!(sc & 0x30000) && !(sc & 255)) {
            return kIOReturnBusy;
        }
    }
    for (unsigned i = 0; i < count && found < gpu_stream_count; i++) {
        const bool valid = ex ? !!(sr(0xc00 + 4 * i) & 1024) : !!(sr(0x800 + 4 * i) & 0x80000000);
        bool leased = false;
        for (unsigned j = 0; j < found; j++) leased |= slots[j] == i;
        if (!valid && !leased) slots[found++] = i;
    }
    if (found != gpu_stream_count) return kIOReturnNoResources;
    const size_t bank = pages * 4096;
    if (!sw(bank, 0) || sr(bank)) return kIOReturnIOError;
    /* SM8475 qcom,qsmmu-v500 ACTLR = 0x32b. */
    constexpr uint32_t actlr = 0x32b;
    if (!sw(bank + 4, actlr) || sr(bank + 4) != actlr || (sr(bank + 0x58) & SmmuContextFaultMask))
        return kIOReturnIOError;
    for (unsigned i = 0; i < gpu_stream_count; i++) {
        if (!sw(0xc00 + slots[i] * 4, 0x20000) ||
            !sw(0x800 + slots[i] * 4, (ex ? 0 : 0x80000000u) | gpu_sids[i]) ||
            !sw(0xc00 + slots[i] * 4, 0x20000 | (ex ? 1024 : 0)))
            return kIOReturnIOError;
    }
    IOReturn status = gpu->tables.begin(gpu->output_bits);
    if (!status)
        status = gpu->context.prepare(&gpu->tables, 0, 0, UINT64_C(0x100000000), 1048576, 0, 2, 4096, gpu->output_bits, 32);
    if (!status) status = gpu->context.program(&ops->smmu);
    ArmSmmu4KStream streams[gpu_stream_count] = {};
    for (unsigned i = 0; i < gpu_stream_count; i++) streams[i] = {slots[i], gpu_sids[i]};
    if (!status) status = gpu->context.attach(streams, gpu_stream_count);
    NativeAdrenoVmRegion lower = {&gpu->tables, gpu->context.arenaHandle(), gpu->context.stateHandle()};
    if (!status)
        status = gpu->vm.attach(&ops->smmu, gpu->context.configHandle(), gpu->context.streamsHandle(), &lower, nullptr, 131072, 262144);
    for (unsigned i = 0; i < 5 && !status; i++) {
        status = gpu->buffers[i].allocate(sizes[i], gpu->output_bits, true, sizes[i]);
        uint64_t handle = 0;
        if (!status) status = gpu->buffers[i].makeWriteCombined();
        if (!status) status = gpu->vm.map(&gpu->buffers[i], 0, iovas[i], i ? 3 : 5, 0, 2, &handle);
    }
    if (!status && (PongAdrenoSqeFirmwareBytes < 8 || PongAdrenoSqeFirmwareBytes - 4 > sizes[0]))
        status = kIOReturnBadArgument;
    if (!status) {
        memcpy(gpu->buffers[0].mapping(), PongAdrenoSqeFirmware + 4, PongAdrenoSqeFirmwareBytes - 4);
        status = gpu->buffers[0].publishForDevice(0, sizes[0], false);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    note("GPU native4K/SQE prepared", unsigned(status), slots[0], slots[1]);
    gpu->prepared = !status;
    return status;
}

IOReturn PongAdrenoCpStart(PongGpuGmuDevice *gmu)
{
    if (!gpu || !gpu->prepared || gpu->started || !gmu) return kIOReturnNotReady;
    PongGpuPowerResult power = {};
    const auto held = PongAdrenoGmuHoldGpu(gmu, &power);
    note("GX OOB/START", held, power.phase, power.firmware_error);
    if (held) return kIOReturnNotReady;
    const uint64_t began = now(nullptr);
    while ((rd(GEN7_GMU_GFX_PWR_CLK_STATUS) & 3) && now(nullptr) - began < 100000) IODelay(10);
    if (gpu->poisoned || (rd(GEN7_GMU_GFX_PWR_CLK_STATUS) & 3)) {
        note("GX power/clock not on", 1, rd(GEN7_GMU_GFX_PWR_CLK_STATUS));
        return kIOReturnNotReady;
    }
    if (!hardware_init()) {
        note("CP hw_init failed", 1);
        return kIOReturnIOError;
    }
    NativeAdrenoRingCache cache = {nullptr, publish_ring, acquire_fence};
    auto status = gpu->ring.attach(&gpu->ops.registers, &cache, gpu->buffers[1].mapping(), sizes[1], iovas[1],
                                   gpu->buffers[2].mapping(), sizes[2], iovas[2]);
    if (!status && (!wr64(GEN7_CP_BV_RB_RPTR_ADDR_LO, iovas[2] + 8) ||
                    !wr64(GEN7_CP_SQE_INSTR_BASE_LO, iovas[0]) ||
                    !wr(GEN7_CP_SQE_CNTL, 1)))
        status = kIOReturnIOError;
    NativeAdrenoCpInitConfig config = {iovas[0], iovas[3], 1};
    uint32_t unused_fence = 0, fence = 0;
    if (!status) status = gpu->init.run(&gpu->ops.registers, &gpu->ring, &config, &unused_fence, false);
    const uint32_t init_wptr = gpu->ring.writePointer();
    note("CP_ME_INIT submitted", unsigned(status), init_wptr, (uint64_t(rd(GEN7_CP_RB_RPTR)) << 32) | rd(GEN7_CP_RB_WPTR));
    if (status || !wait_bootstrap(init_wptr, "CP_ME_INIT drained")) return kIOReturnIOError;
    if (!authorize()) return kIOReturnNotPermitted;
    uint32_t words[2] = {};
    if (AdrenoPM4Type7(0x66, 1, words) != AdrenoPM4Success || gpu->ring.submitUnfenced(words, 2))
        return kIOReturnIOError;
    const uint32_t ns_wptr = gpu->ring.writePointer();
    if (!wait_bootstrap(ns_wptr, "CP nonsecure drained")) return kIOReturnIOError;
    auto *scratch = static_cast<volatile uint32_t *>(gpu->buffers[4].mapping());
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t write[4] = {0, uint32_t(iovas[4]), uint32_t(iovas[4] >> 32), 0x7300302};
    if (AdrenoPM4Type7(0x3d, 3, write) != AdrenoPM4Success || gpu->ring.submit(write, 4, &fence) || !wait_fence(fence))
        return kIOReturnIOError;
    __asm__ volatile("dsb sy" ::: "memory");
    if (scratch[0] != write[3]) {
        note("CP nonsecure write incomplete", 1, scratch[0]);
        return kIOReturnIOError;
    }
    status = gpu->irq.attach(&gpu->ops.registers);
    uint32_t route = 0;
    uint64_t affinity = 0;
    /* SM8475 GPU IRQ: GIC_SPI 300 (architectural INTID 332), level-high. */
    if (!status) status = armcpu_spi_attach(332, false, interrupt, nullptr, &route, &affinity);
    if (!status) status = gpu->irq.programMask(nullptr);
    note("GPU IRQ 332", unsigned(status), route, affinity);
    if (status) {
        wr(GEN7_RBBM_INT_0_MASK, 0);
        return status;
    }
    gpu->irq_attached = true;
    status = gpu->submit.attach(&gpu->vm, &gpu->ring);
    NativeAdrenoTimeIo timer = {gpu->ops.registers.cookie, gpu->ops.registers.read32};
    if (!status) status = gpu->timer.attach(&timer, NativeAdrenoTimeCp);
    uint64_t ticks = 0;
    if (!status) status = gpu->timer.read(&ticks);
    if (status || !ticks) return status ? status : kIOReturnNotReady;
    gpu->ring.setTimeline(true);
    gpu->started = true;
    native_adreno_wc_enabled = 1;
    gpu->backend = {
        &gpu->vm, &gpu->submit, &gpu->ring, &gpu->timer, &gpu->status,
        {nullptr, now, idle},
        {PongAdrenoChipId, PongAdrenoGmemBytes, 0, 0x1fffffffff000ULL, 0x20000000, 0xA0000000,
         PongAdrenoHighestBankBit, 6, 1, 4, NativeAdrenoDeviceClientIova, 0},
        gpu->output_bits, client_healthy
    };
    note("CP ready; nonsecure fence ok", 0, fence, ticks);
    return PongAdrenoPublishClient(&gpu->backend) ? kIOReturnSuccess : kIOReturnNoResources;
}

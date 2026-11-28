// SPDX-License-Identifier: MIT
// Persistent UFS diagnostic log writer for Xiaomi Pad 6 Pro (liuqin, SM8475).
// Enabled with boot-arg log=1: every 2 s writes structured GPU/Metal/Display/HW
// slots and the streaming XNU kernel message buffer (msgbufp) to the bounded
// 32 MiB log window at the end of userdata (NativeUfs native_ufs_log_*), using
// the exact SHLOG001 v5 binary format decoded by scripts/read-liuqin-log.py.
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>
#include <string.h>
#include <stdint.h>

/* ArmCpu strong hooks required for static arm64e kernel linking (zero DYLD_CHAINED_IMPORT). */
void (*arm64_platform_sleep_hook)(int boot_cpu) = 0;
__attribute__((weak, visibility("hidden"))) void *sheng_pmu_fixup = 0;
__attribute__((weak, visibility("hidden"))) int unixnu_armcpu_spi_mask_refuse(unsigned int irq, int refuse)
{
    (void)irq;
    (void)refuse;
    return -536870201; /* kIOReturnUnsupported: UFS stays in polling mode */
}

/* NativeUfs bounded log window exports (strongly defined in NativeUfs.cpp). */
extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **);
extern int native_ufs_log_write(uint64_t, uint32_t, const void *);
extern int native_ufs_log_sync(void);
extern int native_ufs_log_panic(void);
extern unsigned native_ufs_store_gpt_check(void);

/* XNU kernel message buffer (bsd/sys/msgbuf.h leading fields). */
struct boot_log_msgbuf {
    int msg_magic, msg_size, msg_bufx, msg_bufr;
    char *msg_bufc;
};
extern struct boot_log_msgbuf *msgbufp;

/* Optional board/GPU/display telemetry (build-sheng-source-storage.py generates
 * hidden weak zero defaults in BootLogDefaults.c for every weak_import below). */
struct boot_log_metal_diag {uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps; char opener[3][17]; char reader[3][17]; char name_reader[17]; char starter[3][17]; char report[64]; char report2[64];};
extern struct boot_log_metal_diag native_adreno_metal_diag __attribute__((weak_import));
extern volatile uint32_t native_liuqin_gpu_stats[8] __attribute__((weak_import));
extern char native_liuqin_gpu_phase[64] __attribute__((weak_import));
extern char native_liuqin_gpu_log[2048] __attribute__((weak_import));
extern uint32_t native_adreno_publish_stats(uint32_t *) __attribute__((weak_import));
extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
extern volatile uint32_t native_legacy_stage __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_rejected_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_reason __attribute__((weak_import));
extern volatile uint32_t native_legacy_gpu_fallbacks __attribute__((weak_import));
extern volatile uint32_t native_legacy_gpu_last_status __attribute__((weak_import));
extern volatile uint32_t native_legacy_formats[10] __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct[14] __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_n __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_last __attribute__((weak_import));
extern volatile uint64_t native_legacy_copy_max __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_check[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_running __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern volatile uint32_t native_touch_stage_done __attribute__((weak_import));
extern volatile uint32_t native_touch_frames __attribute__((weak_import));
extern volatile int32_t native_buttons_stats[10] __attribute__((weak_import));
extern volatile int32_t native_backlight_stats[16] __attribute__((weak_import));
extern volatile uint32_t native_usb_stats[54] __attribute__((weak_import));
extern volatile int32_t native_adsp_stats[16] __attribute__((weak_import));
extern volatile int32_t native_battery_stats[16] __attribute__((weak_import));
extern volatile uint32_t native_board_clock[16] __attribute__((weak_import));
extern char native_iokit_busy_text[4][192] __attribute__((weak_import));

/* SHLOG001 v5 on-disk layout (matches UniXNU-sheng/scripts/read-sheng-log.py). */
enum {
    LogBlock = 4096,
    LogMsgFirst = 1,
    LogMsgBlocks = 32,
    LogSlotFirst = 33,
    LogSlotBlocks = 16,
    LogPeriodMs = 2000,
    LogMaxErrors = 3,
};

struct boot_log_header {
    char magic[8];                 /* "SHLOG001" */
    uint32_t version, header_bytes;
    uint64_t boot_id;              /* CNTPCT when the writer started */
    uint64_t first_lba;            /* absolute 4096-byte LBA on LUN 0 */
    uint32_t window_blocks, msg_first, msg_blocks, msg_bytes;
    uint32_t slot_first, slot_blocks, slot_count, slots_written;
    uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
    char tag[32];
    uint32_t export_first, export_blocks, export_bytes, export_generation;
    uint32_t export_below;
    uint32_t diag_first, diag_bytes;
    uint32_t msg_mode, msg_resyncs, msg_head_lo, msg_head_hi;
};
_Static_assert(__builtin_offsetof(struct boot_log_header, msg_head_hi) == 160, "SHLOG001 v5 msg_head_hi offset mismatch");
_Static_assert(sizeof(struct boot_log_header) == 168, "SHLOG001 v5 header size mismatch");

/* Writer state observed by SourceStorage.cpp boot_log_check:
 * 0 off, 1 waiting, 2 writing, 3 stopped after errors, 4 stopped (GPT changed), 5 panic flushed. */
volatile unsigned int boot_log_state, boot_log_rounds, boot_log_errors;
const char *volatile boot_log_open_refusal = "";
volatile uint64_t boot_log_first_lba;

static int boot_log_started;
static volatile int boot_log_panicked;
static uint8_t *boot_log_buffer;
static uint8_t *boot_log_panic_buffer;
static uint8_t *boot_log_msg_ring;
static struct boot_log_header *volatile boot_log_live;

static struct {
    const char *bufc;
    int size, bufx, synced;
    uint64_t head, written;
} msg_stream;

static uint64_t log_ticks(void)
{
    uint64_t t = 0;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t));
    return t;
}

static size_t put_line(char *out, size_t at, size_t cap, const char *line)
{
    if (!line || !*line || at + 2 >= cap) return at;
    size_t len = strlen(line);
    if (at + len + 2 > cap) len = cap - at - 2;
    memcpy(out + at, line, len);
    at += len;
    out[at++] = '\n';
    out[at] = '\0';
    return at;
}

static uint32_t copy_msgbuf(uint8_t *out)
{
    memset(out, 0, LogMsgBlocks * LogBlock);
    struct boot_log_msgbuf *m = msgbufp;
    if (!m || !m->msg_bufc || m->msg_size <= 0) return 0;
    int size = m->msg_size, x = m->msg_bufx;
    if (x < 0 || x >= size) x = 0;
    uint32_t n = 0;
    int i = size > (int)(LogMsgBlocks * LogBlock) ? size - (int)(LogMsgBlocks * LogBlock) : 0;
    for (; i < size && n < LogMsgBlocks * LogBlock; ++i) {
        char c = m->msg_bufc[(x + i) % size];
        if (c) out[n++] = (uint8_t)c;
    }
    return n;
}

static void msg_append(uint8_t c)
{
    if (!c) return;
    const uint64_t cap = (uint64_t)LogMsgBlocks * LogBlock;
    boot_log_msg_ring[msg_stream.head % cap] = c;
    ++msg_stream.head;
}

static void msg_stream_round(uint32_t *resyncs)
{
    struct boot_log_msgbuf *m = msgbufp;
    if (!m || !m->msg_bufc || m->msg_size <= 0) return;
    const int size = m->msg_size;
    int x = m->msg_bufx;
    if (x < 0 || x >= size) x = 0;
    if (!msg_stream.synced || msg_stream.bufc != m->msg_bufc || msg_stream.size != size) {
        if (msg_stream.synced) ++*resyncs;
        const int window = (int)(LogMsgBlocks * LogBlock);
        for (int i = size > window ? size - window : 0; i < size; ++i)
            msg_append((uint8_t)m->msg_bufc[(x + i) % size]);
        msg_stream.bufc = m->msg_bufc;
        msg_stream.size = size;
        msg_stream.synced = 1;
    } else {
        for (int i = msg_stream.bufx; i != x; i = (i + 1 == size) ? 0 : i + 1)
            msg_append((uint8_t)m->msg_bufc[i]);
    }
    msg_stream.bufx = x;
}

static int msg_stream_write(void)
{
    if (msg_stream.written == msg_stream.head) return 1;
    uint64_t first = msg_stream.written / LogBlock;
    uint64_t end = (msg_stream.head + LogBlock - 1) / LogBlock;
    if (end - first > LogMsgBlocks) first = end - LogMsgBlocks;
    int ok = 1;
    while (ok && first < end) {
        const uint32_t at = (uint32_t)(first % LogMsgBlocks);
        uint32_t n = (uint32_t)(end - first);
        if (n > LogMsgBlocks - at) n = LogMsgBlocks - at;
        ok = native_ufs_log_write(LogMsgFirst + at, n, boot_log_msg_ring + (uint64_t)at * LogBlock);
        first += n;
    }
    if (ok) msg_stream.written = msg_stream.head;
    return ok;
}

static size_t compose_slot(char *out, size_t cap, uint32_t slot, unsigned now, const char *tag, uint64_t freq)
{
    size_t at = 0;
    char row[320];
    snprintf(row, sizeof row, "liuqin slot=%u t=%us rounds=%u errors=%u tag=%s",
             slot, now, boot_log_rounds, boot_log_errors, tag ? tag : "");
    at = put_line(out, at, cap, row);

    if (&native_liuqin_gpu_stats && &native_liuqin_gpu_phase) {
        snprintf(row, sizeof row, "[gpu] st=%u rc=%u mhz=%u gmu=0x%x/0x%x mtl=%u/0x%x/%u phase=%.60s",
                 native_liuqin_gpu_stats[0], native_liuqin_gpu_stats[1], native_liuqin_gpu_stats[4],
                 native_liuqin_gpu_stats[2], native_liuqin_gpu_stats[3],
                 native_liuqin_gpu_stats[5], native_liuqin_gpu_stats[6], native_liuqin_gpu_stats[7],
                 native_liuqin_gpu_phase);
        at = put_line(out, at, cap, row);
    }

    if (&native_adreno_publish_stats && &native_adreno_cp_aperture && &native_adreno_table_switches && &native_adreno_mount_fail) {
        uint32_t closed = 0;
        const uint32_t opened = native_adreno_publish_stats(&closed);
        snprintf(row, sizeof row,
                 "[metal] pub=%u/%u ap=0x%x/0x%x/0x%x/m%u sw=cp:%u/cpu:%u/cr:%u/ds:%u/live:%u/pk:%u/e:0x%x/s:%u mnt=%u/0x%x/q:%u/n:%u",
                 opened, closed,
                 native_adreno_cp_aperture[0], native_adreno_cp_aperture[1],
                 native_adreno_cp_aperture[2], native_adreno_cp_aperture[3],
                 native_adreno_table_switches[0], native_adreno_table_switches[1],
                 native_adreno_table_switches[2], native_adreno_table_switches[3],
                 native_adreno_table_switches[4], native_adreno_table_switches[5],
                 native_adreno_table_switches[6], native_adreno_table_switches[7],
                 native_adreno_mount_fail[0], native_adreno_mount_fail[1],
                 native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
        at = put_line(out, at, cap, row);
    }

    if (&native_adreno_metal_diag) {
        const struct boot_log_metal_diag *d = &native_adreno_metal_diag;
        snprintf(row, sizeof row,
                 "[metal diag] opens=%u busy=%u bad=%u closes=%u rpt=%u prop=%u name=%u calls=%u sel=%u st=0x%x mb=%u/%u marks=%u infl=%u",
                 d->opens, d->busy, d->bad, d->closes, d->reports, d->property_reads, d->name_reads,
                 d->calls, d->last_selector, d->last_status, d->mb_maps, d->mb_calls, d->marks_total, d->inflight);
        at = put_line(out, at, cap, row);
        snprintf(row, sizeof row,
                 "[metal who] open=%.16s,%.16s start=%.16s,%.16s rpt=%.63s | rpt2=%.63s",
                 d->opener[0], d->opener[1], d->starter[0], d->starter[1], d->report, d->report2);
        at = put_line(out, at, cap, row);
    }

    if (&native_legacy_stage) {
        const uint64_t copy_last_us = (&native_legacy_copy_last && freq) ? (native_legacy_copy_last * 1000000ULL / freq) : 0;
        const uint64_t copy_max_us = (&native_legacy_copy_max && freq) ? (native_legacy_copy_max * 1000000ULL / freq) : 0;
        snprintf(row, sizeof row,
                 "[display] st=%u ok=%u rej=%u/%u gpu_fb=%u/0x%x copy=%llu last=%lluus max=%lluus fmt=%u/%u/%u/no=%u:%08x dir=%u gpu=%u/%u cpu=%u/%u",
                 native_legacy_stage,
                 &native_legacy_swap_accepted_count ? native_legacy_swap_accepted_count : 0u,
                 &native_legacy_swap_rejected_count ? native_legacy_swap_rejected_count : 0u,
                 &native_legacy_rej_reason ? native_legacy_rej_reason : 0u,
                 &native_legacy_gpu_fallbacks ? native_legacy_gpu_fallbacks : 0u,
                 &native_legacy_gpu_last_status ? native_legacy_gpu_last_status : 0u,
                 (unsigned long long)(&native_legacy_copy_n ? native_legacy_copy_n : 0ULL),
                 (unsigned long long)copy_last_us, (unsigned long long)copy_max_us,
                 &native_legacy_formats ? native_legacy_formats[0] : 0u,
                 &native_legacy_formats ? native_legacy_formats[1] : 0u,
                 &native_legacy_formats ? native_legacy_formats[2] : 0u,
                 &native_legacy_formats ? native_legacy_formats[3] : 0u,
                 &native_legacy_formats ? native_legacy_formats[4] : 0u,
                 &native_legacy_formats ? native_legacy_formats[5] : 0u,
                 &native_legacy_formats ? native_legacy_formats[6] : 0u,
                 &native_legacy_formats ? native_legacy_formats[7] : 0u,
                 &native_legacy_formats ? native_legacy_formats[8] : 0u,
                 &native_legacy_formats ? native_legacy_formats[9] : 0u);
        at = put_line(out, at, cap, row);
    }

    if (&native_ufs_dpu_state && &native_legacy_direct) {
        snprintf(row, sizeof row,
                 "[dpu] smmu=0x%x chk=0x%x/0x%x/0x%x/0x%x flips=%u map=%u fail=%u slow=%u max=%uus fb=%u boot=%u wait=%u/%u/%u",
                 native_ufs_dpu_state,
                 &native_ufs_dpu_check ? native_ufs_dpu_check[0] : 0u,
                 &native_ufs_dpu_check ? native_ufs_dpu_check[1] : 0u,
                 &native_ufs_dpu_check ? native_ufs_dpu_check[2] : 0u,
                 &native_ufs_dpu_check ? native_ufs_dpu_check[3] : 0u,
                 native_legacy_direct[0], native_legacy_direct[1], native_legacy_direct[2],
                 native_legacy_direct[3], native_legacy_direct[4], native_legacy_direct[7],
                 native_legacy_direct[8], native_legacy_direct[9], native_legacy_direct[12],
                 native_legacy_direct[13]);
        at = put_line(out, at, cap, row);
    }

    snprintf(row, sizeof row,
             "[hw] smp=%u touch=%u/%u btn=%d/%d/%d/%d bl=%d/%d/req=%d/wr=%d/err=%d cpu=%.64s",
             &armcpu_smp_running ? armcpu_smp_running : 0u,
             &native_touch_stage_done ? native_touch_stage_done : 0u,
             &native_touch_frames ? native_touch_frames : 0u,
             &native_buttons_stats ? native_buttons_stats[0] : 0,
             &native_buttons_stats ? native_buttons_stats[4] : 0,
             &native_buttons_stats ? native_buttons_stats[5] : 0,
             &native_buttons_stats ? native_buttons_stats[8] : 0,
             &native_backlight_stats ? native_backlight_stats[0] : 0,
             &native_backlight_stats ? native_backlight_stats[3] : 0,
             &native_backlight_stats ? native_backlight_stats[1] : 0,
             &native_backlight_stats ? native_backlight_stats[2] : 0,
             &native_backlight_stats ? native_backlight_stats[9] : 0,
             &native_cpufreq_row ? native_cpufreq_row : "");
    at = put_line(out, at, cap, row);

    snprintf(row, sizeof row,
             "[io] usb=%d/id=0x%x/smmu=0x%x/rep=0x%x/pwr=%u adsp=%d/err=%d/fw=%d/ch=0x%x bat=%d/%d%%/mv=%d/ma=%d rtc=0x%x/%u",
             &native_usb_stats ? (int32_t)native_usb_stats[0] : 0,
             &native_usb_stats ? native_usb_stats[1] : 0u,
             &native_usb_stats ? native_usb_stats[7] : 0u,
             &native_usb_stats ? native_usb_stats[8] : 0u,
             &native_usb_stats ? native_usb_stats[12] : 0u,
             &native_adsp_stats ? native_adsp_stats[0] : 0,
             &native_adsp_stats ? native_adsp_stats[1] : 0,
             &native_adsp_stats ? native_adsp_stats[7] : 0,
             &native_adsp_stats ? native_adsp_stats[11] : 0,
             &native_battery_stats ? native_battery_stats[0] : 0,
             &native_battery_stats ? native_battery_stats[1] : 0,
             &native_battery_stats ? native_battery_stats[2] : 0,
             &native_battery_stats ? native_battery_stats[3] : 0,
             &native_board_clock ? native_board_clock[0] : 0u,
             &native_board_clock ? native_board_clock[7] : 0u);
    at = put_line(out, at, cap, row);

    if (&native_iokit_busy_text && native_iokit_busy_text[0][0]) {
        snprintf(row, sizeof row, "[iokit] %.190s", native_iokit_busy_text[0]);
        at = put_line(out, at, cap, row);
    }

    if (&native_liuqin_gpu_log && native_liuqin_gpu_log[0]) {
        at = put_line(out, at, cap, "[gpu log]");
        at = put_line(out, at, cap, native_liuqin_gpu_log);
    }
    return at;
}

static void writer(void *arg, wait_result_t wait_result)
{
    (void)arg;
    (void)wait_result;
    thread_set_thread_name(current_thread(), "liuqin-log");
    boot_log_state = 1;
    uint64_t first = 0, blocks = 0;
    const char *why = "open failed";
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        if (native_ufs_log_open(&first, &blocks, &why)) break;
        IOSleep(500);
    }
    if (!blocks || blocks <= LogSlotFirst + LogSlotBlocks) {
        boot_log_open_refusal = why ? why : "window unavailable";
        boot_log_state = 3;
        return;
    }
    boot_log_first_lba = first;
    const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
    static struct boot_log_header h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "SHLOG001", 8);
    h.version = 5;
    h.header_bytes = sizeof h;
    h.boot_id = log_ticks();
    h.first_lba = first;
    h.window_blocks = (uint32_t)blocks;
    h.msg_first = LogMsgFirst;
    h.msg_blocks = LogMsgBlocks;
    h.slot_first = LogSlotFirst;
    h.slot_blocks = LogSlotBlocks;
    h.slot_count = slot_count;
    h.gpt_open = h.gpt_last = native_ufs_store_gpt_check();
    char tag[32] = "liuqin";
    PE_parse_boot_argn("build-tag", tag, sizeof tag);
    tag[sizeof tag - 1] = '\0';
    strlcpy(h.tag, tag, sizeof h.tag);

    uint64_t freq = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) freq = 19200000ULL;
    const uint64_t start = log_ticks();

    boot_log_live = &h;
    boot_log_state = 2;
    uint32_t errors = 0;

    for (;;) {
        if (boot_log_panicked) return;
        const uint64_t began = log_ticks();
        const unsigned now = (unsigned)((began - start) / freq);
        int ok = 1;

        if (boot_log_msg_ring) msg_stream_round(&h.msg_resyncs);

        memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
        const size_t text = compose_slot((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now, h.tag, freq);
        uint32_t slot_used = (uint32_t)(text / LogBlock + 1);
        if (slot_used > LogSlotBlocks) slot_used = LogSlotBlocks;
        ok &= native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, slot_used, boot_log_buffer);

        if (!boot_log_msg_ring) {
            h.msg_bytes = copy_msgbuf(boot_log_buffer);
            ok &= native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_buffer);
            h.msg_mode = 0;
        } else {
            ok &= msg_stream_write();
            const uint64_t cap = (uint64_t)LogMsgBlocks * LogBlock;
            const uint64_t head = msg_stream.written;
            h.msg_mode = 1;
            h.msg_head_lo = (uint32_t)head;
            h.msg_head_hi = (uint32_t)(head >> 32);
            h.msg_bytes = (uint32_t)(head < cap ? head : cap);
        }

        if (ok) ++h.slots_written;
        else ++h.write_errors;

        h.uptime_s = now;
        h.rounds = ++boot_log_rounds;
        h.state = boot_log_state;
        if (!(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
        if (boot_log_panicked) return;

        memset(boot_log_buffer, 0, LogBlock);
        memcpy(boot_log_buffer, &h, sizeof h);
        ok &= native_ufs_log_write(0, 1, boot_log_buffer);
        ok &= native_ufs_log_sync();

        if (!ok) boot_log_errors = ++errors;
        else errors = 0;
        if (errors >= LogMaxErrors) {
            boot_log_state = 3;
            return;
        }
        if (h.gpt_last != h.gpt_open) {
            boot_log_state = 4;
            return;
        }
        IOSleep(LogPeriodMs);
    }
}

void boot_log_panic_flush(void)
{
    if (__atomic_exchange_n(&boot_log_panicked, 1, __ATOMIC_ACQ_REL)) return;
    const struct boot_log_header *live = boot_log_live;
    if (!live || !boot_log_panic_buffer || !native_ufs_log_panic()) return;
    struct boot_log_header h = *live;
    h.msg_bytes = copy_msgbuf(boot_log_panic_buffer);
    h.msg_mode = 0;
    h.rounds++;
    h.state = 5;
    native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_panic_buffer);
    memset(boot_log_panic_buffer, 0, LogBlock);
    memcpy(boot_log_panic_buffer, &h, sizeof h);
    native_ufs_log_write(0, 1, boot_log_panic_buffer);
    native_ufs_log_sync();
}

void boot_log_tick(unsigned now)
{
    (void)now;
    if (__atomic_exchange_n(&boot_log_started, 1, __ATOMIC_ACQ_REL)) return;
    uint32_t mode = 0;
    if (!PE_parse_boot_argn("log", &mode, sizeof mode) || mode != 1) return;
    boot_log_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
    if (!boot_log_buffer) {
        boot_log_open_refusal = "no log buffer memory";
        boot_log_state = 3;
        return;
    }
    boot_log_panic_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
    boot_log_msg_ring = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
    if (boot_log_msg_ring) memset(boot_log_msg_ring, 0, LogMsgBlocks * LogBlock);
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start_priority(writer, 0, BASEPRI_KERNEL, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
}

void boot_log_checkpoint(const char *tag)
{
    (void)tag;
}

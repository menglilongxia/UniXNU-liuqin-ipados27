// SPDX-License-Identifier: BSD-2-Clause-Patent
// Liuqin bring-up stub. The full sheng UFS log writer (BootLog.c) also composes
// the BootProgress, VM and verified-root rows, whose symbols live in sheng's
// board-page components that liuqin does not have yet. SourceStorage only needs
// the writer state; the full source can replace this file once those board
// pages are ported.
volatile unsigned int boot_log_state, boot_log_rounds, boot_log_errors;
const char *volatile boot_log_open_refusal = "boot log not ported (liuqin bring-up)";

// ArmCpu references arm64_platform_sleep_hook as weak_import; static arm64e
// kernel linking requires a concrete symbol definition when platform sleep
// patch is not enabled.
void (*arm64_platform_sleep_hook)(int boot_cpu) = 0;

void boot_log_tick(unsigned now)
{
    (void) now;
}

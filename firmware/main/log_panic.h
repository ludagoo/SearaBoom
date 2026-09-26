#ifndef LOG_PANIC_H
#define LOG_PANIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* RTC snapshot + ship text. Host tests include this (no ESP types).
 *
 * Panic prints go to UART, not the vprintf hook. The 8 KB log ring lives
 * in PSRAM, so it dies on reboot and the post-crash boot line can wrap
 * before the first successful POST. Keep reason + backtrace in RTC
 * (survives PANIC/WDT, not power-on) and ship it first on the next boot.
 *
 * A crash-loop overwrites the snapshot; only the last panic is kept.
 */

#define LOG_PANIC_MAGIC 0xA5B00C02u
#define LOG_PANIC_FRAMES 16
#define LOG_PANIC_REASON 80

typedef struct {
    uint32_t magic;
    uint32_t pc;
    uint32_t frames[LOG_PANIC_FRAMES];
    uint32_t sps[LOG_PANIC_FRAMES];
    uint8_t nframes;
    uint8_t core;
    char reason[LOG_PANIC_REASON];
} log_panic_dump_t;

static inline int log_panic_valid(const log_panic_dump_t *dump)
{
    return dump && dump->magic == LOG_PANIC_MAGIC;
}

static inline size_t log_panic_format(char *out, size_t cap,
                                      long long ms,
                                      const char *fw,
                                      const char *reset,
                                      unsigned crashes,
                                      unsigned seq,
                                      const log_panic_dump_t *dump)
{
    if (!out || cap == 0) {
        return 0;
    }
    size_t used = 0;
    fw = fw ? fw : "";
    reset = reset ? reset : "UNKNOWN";

    int n = snprintf(out, cap,
                     "I (%lld) log_shipper: boot fw=%s reset=%s crashes=%u seq=%u\n",
                     ms, fw, reset, crashes, seq);
    if (n < 0) {
        out[0] = 0;
        return 0;
    }
    if ((size_t)n >= cap) {
        out[cap - 1] = 0;
        return cap - 1;
    }
    used = (size_t)n;
    if (!log_panic_valid(dump)) {
        return used;
    }

    char reason[LOG_PANIC_REASON];
    memcpy(reason, dump->reason, LOG_PANIC_REASON);
    reason[LOG_PANIC_REASON - 1] = 0;
    const char *r = reason[0] ? reason : "panic";
    n = snprintf(out + used, cap - used,
                 "W (%lld) log_shipper: panic reason=%s core=%u pc=0x%08x\n"
                 "W (%lld) log_shipper: Backtrace:",
                 ms, r, (unsigned)dump->core, (unsigned)dump->pc, ms);
    if (n < 0) {
        out[used] = 0;
        return used;
    }
    if ((size_t)n >= cap - used) {
        out[cap - 1] = 0;
        return cap - 1;
    }
    used += (size_t)n;

    uint8_t nf = dump->nframes;
    if (nf > LOG_PANIC_FRAMES) {
        nf = LOG_PANIC_FRAMES;
    }
    for (uint8_t i = 0; i < nf; i++) {
        n = snprintf(out + used, cap - used, " 0x%08x:0x%08x",
                     (unsigned)dump->frames[i], (unsigned)dump->sps[i]);
        if (n < 0 || (size_t)n >= cap - used) {
            out[cap - 1] = 0;
            return cap - 1;
        }
        used += (size_t)n;
    }
    if (used + 1 < cap) {
        out[used++] = '\n';
    }
    out[used] = 0;
    return used;
}

#endif

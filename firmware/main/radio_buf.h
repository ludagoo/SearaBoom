#ifndef RADIO_BUF_H
#define RADIO_BUF_H

/*
 * HTTP Icecast ringbuffer watermarks. Host-testable (no ESP headers).
 *
 * Capacity is 256 KB (262144). A 64 kbps stream is ~8 KB/s, so while the
 * mixer is reading, fill ≈ produce and the rb hovers where play started.
 *
 * Live 0.5.25 started at ~100 KB and treated 128 KB as need=131072, so the
 * box hovered at rb≈98–101kB on the 96 KB (32 KB) log-band edge and logged
 * stall rb-drop every ~200–300 ms. 100 KB of AAC is already ~12 s; waiting
 * for 224 KB is ~15 s after ident and ~20–28 s after a soft restart.
 *
 * This policy stops the storm and the loudness pump with a small hysteresis
 * above that hover, not a near-full mute.
 */

#define SB_HTTP_RB_SIZE (256 * 1024)

/* First audible play: 112 KB. ~1.5 s above the live ~100 KB ident hover
 * (12 KB / ~8 KB/s), so fill sits inside the 96–128 KB log band instead of
 * on the 96 KB edge. From empty, ~14 s vs ~28 s at 224 KB. */
#define SB_HTTP_START_BYTES (112 * 1024)

/* After jitter underrun, refill to the same 112 KB before the mixer eats
 * PCM again. Was 128 KB (need=131072) — the live hover never climbed past
 * it — then 224 KB (too much silence). */
#define SB_HTTP_RECOVER_BYTES (112 * 1024)

/* Mute radio and refill. 96 KB is just below the live 98–101 KB hover, so
 * a healthy ~100–112 KB buffer stays PLAYING. 16 KB of hysteresis vs
 * recover is ~2 s muted, not 15 s. */
#define SB_HTTP_UNDERRUN_BYTES (96 * 1024)

#define SB_WIFI_RSSI_WEAK_DBM (-78)
#define SB_WIFI_HTTP_LOW_BYTES SB_HTTP_UNDERRUN_BYTES
/* Spoken wifi-weak hold still refills toward 200 KB (main). Clip covers it. */
#define SB_WIFI_HTTP_RESUME_BYTES (200 * 1024)
#define SB_HTTP_SLOW_LOW_BYTES (64 * 1024)
#define SB_HTTP_SLOW_RESUME_BYTES SB_HTTP_RECOVER_BYTES

/* Gate: WAIT (boot / go_live) and REFILL (post-underrun) both need start/
 * recover. PLAY drops to REFILL at the underrun line. */
#define RADIO_BUF_WAIT 0
#define RADIO_BUF_PLAY 1
#define RADIO_BUF_REFILL 2

static inline int radio_buf_start_ready(int filled)
{
    return filled >= SB_HTTP_START_BYTES;
}

static inline int radio_buf_recover_ready(int filled)
{
    return filled >= SB_HTTP_RECOVER_BYTES;
}

static inline int radio_buf_underrun(int filled)
{
    return filled >= 0 && filled < SB_HTTP_UNDERRUN_BYTES;
}

static inline int radio_buf_play_need(int wifi_weak)
{
    return wifi_weak ? SB_WIFI_HTTP_RESUME_BYTES : SB_HTTP_RECOVER_BYTES;
}

/* already_playing: 0 = boot/start, 1 = in PLAY, 2 = REFILL. */
static inline int radio_buf_gate(int now, int filled)
{
    if (now == RADIO_BUF_PLAY) {
        return radio_buf_underrun(filled) ? RADIO_BUF_REFILL : RADIO_BUF_PLAY;
    }
    if (now == RADIO_BUF_REFILL) {
        return radio_buf_recover_ready(filled) ? RADIO_BUF_PLAY : RADIO_BUF_REFILL;
    }
    return radio_buf_start_ready(filled) ? RADIO_BUF_PLAY : RADIO_BUF_WAIT;
}

#endif

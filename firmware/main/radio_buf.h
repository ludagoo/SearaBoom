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
 * stall rb-drop every ~200–300 ms. 0.5.26 used 112 KB start/recover and
 * muted below 96 KB — a small gap under that hover.
 *
 * One fill height: 224 KB (229376) for start, recover, wifi-weak resume,
 * and http-slow resume. radio_buf_play_need() returns that height whether
 * wifi is weak or not. The wifi-weak clip can still play; only the byte
 * target is shared. Leave PLAY at 64 KB (65536), not 96 KB. From empty,
 * ~28 s of silence at ~8 KB/s; from the underrun line, ~20 s. That long
 * quiet while refilling is the policy. Ring stays 256 KB.
 */

#define SB_HTTP_RB_SIZE (256 * 1024)

/* First audible play: 224 KB. Same height as recover / wifi-weak /
 * http-slow resume. From empty ~28 s at ~8 KB/s. */
#define SB_HTTP_START_BYTES (224 * 1024)

/* After jitter underrun, refill to the same 224 KB before the mixer eats
 * PCM again. Long quiet is intentional — do not shrink this toward the
 * leave-PLAY line. */
#define SB_HTTP_RECOVER_BYTES SB_HTTP_START_BYTES

/* Mute radio and refill. 64 KB, not 96 KB. Do not keep a small gap under
 * the fill target. */
#define SB_HTTP_UNDERRUN_BYTES (64 * 1024)

#define SB_WIFI_RSSI_WEAK_DBM (-78)
#define SB_WIFI_HTTP_LOW_BYTES SB_HTTP_UNDERRUN_BYTES
/* Spoken wifi-weak hold uses the same 224 KB fill. Clip covers it. */
#define SB_WIFI_HTTP_RESUME_BYTES SB_HTTP_START_BYTES
#define SB_HTTP_SLOW_LOW_BYTES (64 * 1024)
#define SB_HTTP_SLOW_RESUME_BYTES SB_HTTP_START_BYTES

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
    (void)wifi_weak;
    return SB_HTTP_START_BYTES;
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

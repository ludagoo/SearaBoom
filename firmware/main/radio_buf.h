#ifndef RADIO_BUF_H
#define RADIO_BUF_H

/*
 * HTTP Icecast ringbuffer watermarks. Host-testable (no ESP headers).
 *
 * Capacity is 256 KB (262144). A 64 kbps stream is ~8 KB/s, so while the
 * mixer is reading, fill ≈ produce and the rb hovers where play started.
 * Starting or resuming at ~100–128 KB therefore sits on the rb-drop line
 * forever (live: rb≈98–101kB / 262144 need=131072).
 */

#define SB_HTTP_RB_SIZE (256 * 1024)

/* First audible play: 224 KB of 256 KB (87.5%). Leaves 32 KB so the HTTP
 * writer is not stuck on a completely full rb. Was implicit ~100 KB after
 * the ident prefetch. */
#define SB_HTTP_START_BYTES (224 * 1024)

/* After jitter underrun / rb-drop, refill to the same high-water before
 * the mixer eats PCM again. Was SB_HTTP_SLOW_RESUME_BYTES = 128 KB
 * (need=131072) — half full, and the skinny mark the live box never
 * climbed past. */
#define SB_HTTP_RECOVER_BYTES (224 * 1024)

/* Playing fill is skinny: mute radio and refill. Same 128 KB line that
 * already logs stall rb-drop. */
#define SB_HTTP_UNDERRUN_BYTES (128 * 1024)

#define SB_WIFI_RSSI_WEAK_DBM (-78)
#define SB_WIFI_HTTP_LOW_BYTES SB_HTTP_UNDERRUN_BYTES
#define SB_WIFI_HTTP_RESUME_BYTES SB_HTTP_RECOVER_BYTES
#define SB_HTTP_SLOW_LOW_BYTES (64 * 1024)
#define SB_HTTP_SLOW_RESUME_BYTES SB_HTTP_RECOVER_BYTES

/* Gate: WAIT (boot / go_live) and REFILL (post-underrun) both need the
 * high-water. PLAY drops to REFILL at the underrun line. */
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

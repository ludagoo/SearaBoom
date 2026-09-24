#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "radio_player.h"
#include "radio_buf.h"
#include "listen_stats.h"
#include "pcm_upmix.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_mem.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "raw_stream.h"
#include "aac_decoder.h"
#include "downmix.h"
#include "board.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "ringbuf.h"

static const char *TAG = "radio_player";

#define SB_STREAM_STALL_MS 15000
#define SB_STREAM_STALL_GRACE_MS 35000
/* HTTP Icecast with no music_info by then → HTTPS (TLS). */
#define SB_HTTP_FALLBACK_MS 8000
/* HTTPS (or native http:// URL) with no music_info: DNS/open can die
 * after fallback. Stall check needs music_info; HTTP fallback needs
 * s_using_http; a live STA never hits "wifi back". Retry the join. */
#define SB_JOIN_RETRY_MS 20000
#define SB_STREAM_HARD_RESTART_AFTER 3
/* HTTP rb watermarks live in radio_buf.h (start/recover 224 KB, mute below 64 KB). */
#define SB_HTTP_SLOW_RECONNECT_MS 25000
#define SB_HTTP_SLOW_GROW_BYTES (16 * 1024)
#define SB_HTTP_SLOW_RECONNECT_MAX 3
#define SB_HTTP_SLOW_DEBOUNCE_MS 2000
/* Growth window for "internet lenta" while prebuffering. Speak if fill
 * grew less than SB_HTTP_SLOW_GROW_BYTES over this window. Must stay
 * below SB_HTTP_SLOW_RECONNECT_MS: the loop resets s_prebuffer_since_ms
 * (and stall grace) on every 25 s reconnect. Healthy 8 KB/s grows
 * ~96 KB in 12 s. The critical path (fill < 64 KB while PLAYING) is
 * separate. */
#define SB_HTTP_SLOW_PREBUF_SPEAK_MS 12000
#if SB_HTTP_SLOW_PREBUF_SPEAK_MS >= SB_HTTP_SLOW_RECONNECT_MS
#error SB_HTTP_SLOW_PREBUF_SPEAK_MS must be below the 25 s reconnect reset
#endif
#define SB_BUFFER_PROMPT_COOLDOWN_MS (3 * 60 * 1000)
#define SB_MIX_SR 44100
#define SB_SLOT_RADIO 0
#define SB_SLOT_CLIP 1
#define SB_PCM_VOICE_ABS 2000
#define SB_PCM_HOLD_ABS 400

#define SB_ALC_MIN_DB (-36)
#define SB_ALC_CURVE2_MAX_DB 2
#define SB_ALC_CLICK22_DB 4
#define SB_ALC_CLICK23_DB 6
#define SB_ALC_CLICK24_DB 9
#define SB_ALC_CLICK25_DB 12
#define SB_ALC_CLICK26_DB 15
/* After +15: +6 dB clicks to +63. Knob 27–34. */
#define SB_ALC_CLICK27_DB 21
#define SB_ALC_CLICK28_DB 27
#define SB_ALC_CLICK29_DB 33
#define SB_ALC_CLICK30_DB 39
#define SB_ALC_CLICK31_DB 45
#define SB_ALC_CLICK32_DB 51
#define SB_ALC_CLICK33_DB 57
#define SB_ALC_CLICK34_DB 63
#if SB_ALC_CLICK34_DB != 63
#error "click 34 is listen-locked at I2S ALC max +63"
#endif
/* Knob 22+ I2S ALC (dB). 1–21 stay v2. Click 34 / +63 is the product ceiling. */
static const int s_extra_alc[] = {
    SB_ALC_CLICK22_DB,
    SB_ALC_CLICK23_DB,
    SB_ALC_CLICK24_DB,
    SB_ALC_CLICK25_DB,
    SB_ALC_CLICK26_DB,
    SB_ALC_CLICK27_DB,
    SB_ALC_CLICK28_DB,
    SB_ALC_CLICK29_DB,
    SB_ALC_CLICK30_DB,
    SB_ALC_CLICK31_DB,
    SB_ALC_CLICK32_DB,
    SB_ALC_CLICK33_DB,
    SB_ALC_CLICK34_DB,
};
_Static_assert(
    SB_VOLUME_MAX == SB_VOL_CURVE2_MAX + (int)(sizeof(s_extra_alc) / sizeof(s_extra_alc[0])),
    "extra ALC table must cover knobs 22..max");
/* Downmix reads slots in series. A mute-slot wait of 20 ticks (20 ms at
 * 1 kHz) on an empty rb adds 20 ms to every 256-sample block (~6 ms) and
 * I2S underruns — choppy welcome. Unused slots must not block: ADF treats
 * TIMEOUT as silence and continues. Mix then blocks on I2S/clip.
 * Timeout 0 on the BYPASS base slot cuts audio (ADF #1010/#1069). 1 tick
 * yields without that. */
#define SB_MIX_MUTE_TIMEOUT 1
#define SB_MIX_CLIP_TIMEOUT 40
/* Increased from 50 to 200 ticks to tolerate transient HTTP/network jitter.
 * The radio PCM rb is 32 KB (~180 ms at 44100 stereo). A 50 ms timeout was
 * too aggressive: even with strong RSSI, TCP retransmits or server slowness
 * can briefly starve the AAC decoder, causing mix underruns and rb-drop storms.
 * 200 ms gives the HTTP→AAC→PCM pipeline time to recover without restarting. */
#define SB_MIX_RADIO_TIMEOUT 200

#define SB_BEEP_HZ 2000
#define SB_BEEP_MS 55
/* Peak ~-3.5 dBFS. Old 4200 was ~-17.8 dBFS and vanished under a hot mix. */
#define SB_BEEP_AMP 22000
#define SB_BEEP_FRAMES ((SB_MIX_SR * SB_BEEP_MS) / 1000)
#define SB_LIMIT_HZ 880
#define SB_LIMIT_MS 120
#define SB_LIMIT_AMP 26000
#define SB_LIMIT_FRAMES ((SB_MIX_SR * SB_LIMIT_MS) / 1000)
/* Stream duck while a beep overlays (pre-ALC). 4 = -12 dB. Add-only
 * overlay cannot sit above a 0 dBFS station. */
#define SB_BEEP_CONTENT_DUCK 4
#define SB_BEEP_DUCK_FULL 256
/* Radio 0 dB when listening. Spoken clips duck the station 18 dB (was
 * -12) so ident/prompts sit well above the stream. Same I2S ALC after. */
#define SB_MIX_RADIO_GAIN_DB 0
#define SB_MIX_RADIO_DUCK_DB (-18)
#define SB_MIX_CLIP_MUTE_DB (-60)
#define SB_MIX_CLIP_GAIN_DB 0
/* source_info transit_time. Do not JUMP to BYPASS until this elapses. */
#define SB_MIX_TRANSIT_MS 150

#define SB_PROBE_WIN 512
#define SB_PROBE_START_LO 5500
#define SB_PROBE_START_HI 10500
#define SB_PROBE_END_LO 2000
#define SB_PROBE_END_HI 4500

/* Brasilstream groups listeners by User-Agent. VLC 3.0.21 is a known
 * client string so the boxes count as ordinary players. */
static const char *stream_user_agent(void)
{
    return "VLC/3.0.21";
}

static audio_pipeline_handle_t s_mix_pipe;
static audio_pipeline_handle_t s_radio_pipe;
static audio_element_handle_t s_downmix;
static audio_element_handle_t s_tap;
static audio_element_handle_t s_i2s;
static audio_element_handle_t s_http;
static audio_element_handle_t s_aac;
static audio_element_handle_t s_radio_m2s;
static audio_element_handle_t s_radio_raw;
static audio_event_iface_handle_t s_mix_evt;
static audio_event_iface_handle_t s_radio_evt;
static ringbuf_handle_t s_mute_radio;
static ringbuf_handle_t s_mute_clip;
static ringbuf_handle_t s_radio_pcm;
static ringbuf_handle_t s_clip_pcm;

static int s_volume = SB_DEFAULT_VOLUME;
static bool s_running;
static bool s_prefetching;
static bool s_prebuffering;
static int64_t s_prebuffer_since_ms;
static int s_prebuffer_last_filled;
static int s_prebuffer_reconnects;
static int64_t s_prebuffer_snap_ms;
static bool s_got_music_info;
static bool s_have_out;
static bool s_clip_active;

static volatile bool s_pcm_heard;
static volatile bool s_pcm_flowing;
static volatile int s_pcm_peak_max;
static volatile int s_pcm_peak_now;
static volatile int64_t s_pcm_last_voice_us;
static volatile bool s_amp_gated;
static volatile bool s_tune_static;
static bool s_mix_clip_mode;
static int64_t s_mix_off_until_ms;
static bool s_radio_pcm_paused;
static uint32_t s_static_rng = 0xA5110E11u;
static int32_t s_static_lp;
static uint32_t s_static_ph;
static uint32_t s_static_sweep;
static int64_t s_mix_hold_restart_until_ms;
static volatile int64_t s_last_pcm_ms;
static int64_t s_stall_grace_until_ms;
static int s_restart_backoff_ms = 500;
static int s_stall_strikes;
static char s_url[256];
static bool s_using_http;
static bool s_force_https;
static int64_t s_radio_open_ms;
static volatile bool s_want_stop;
static volatile bool s_wifi_weak_latched;
static bool s_hold_radio;
static int64_t s_hold_started_ms;
static int s_hold_last_filled;
static int s_hold_reconnects;
static int64_t s_http_slow_prompt_ms;
static int64_t s_wifi_weak_prompt_ms;
static int64_t s_http_slow_low_since;
static int64_t s_hold_snap_ms;
static volatile bool s_need_hard_on_wifi;
static volatile bool s_wifi_rejoin;
static volatile bool s_sta_lost_pending;
static volatile bool s_stop_clip;
static int s_rb_drop_band = -1;
static int s_http_evt_st = -1;
static int s_aac_evt_st = -1;

static int http_rb_filled(void);
static void stream_snap(const char *why);
static void i2s_alc_gate(void);
static void drain_mix_evt(void);
static bool sta_associated(void);
static void mix_route_clip_and_radio(void);
static void radio_pcm_pause(void);
static void radio_pcm_resume(void);

static int16_t *s_beep_pcm;
static int16_t *s_limit_pcm;
static const int16_t *s_beep_src;
static int s_beep_len;
static volatile int s_beep_pos = -1;
static bool s_beep_ready;

static volatile bool s_probe_on;
static int s_probe_prev;
static int s_probe_zc;
static int s_probe_n;
static int s_probe_win_peak;
static int s_probe_first_hz;
static int s_probe_last_hz;
static int64_t s_probe_first_us;
static int64_t s_probe_last_us;

static int volume_to_alc(int volume)
{
    if (volume < SB_VOLUME_MIN) {
        volume = SB_VOLUME_MIN;
    }
    if (volume > SB_VOLUME_MAX) {
        volume = SB_VOLUME_MAX;
    }
    /* vol_curve v5: knobs 1–21 match v2. 22–34 add headroom, not a re-span.
     * Product ceiling is 34 / +63. volume>34 clamps; do not add steps. */
    if (volume <= SB_VOL_CURVE2_MAX) {
        return SB_ALC_MIN_DB
            + ((volume - 1) * (SB_ALC_CURVE2_MAX_DB - SB_ALC_MIN_DB))
            / (SB_VOL_CURVE2_MAX - 1);
    }
    int idx = volume - (SB_VOL_CURVE2_MAX + 1);
    int n = (int)(sizeof(s_extra_alc) / sizeof(s_extra_alc[0]));
    if (idx < 0) {
        idx = 0;
    }
    if (idx >= n) {
        idx = n - 1;
    }
    return s_extra_alc[idx];
}

int radio_player_alc_db(int volume)
{
    return volume_to_alc(volume);
}

esp_err_t radio_player_set_volume(int volume)
{
    if (volume < SB_VOLUME_MIN) {
        volume = SB_VOLUME_MIN;
    }
    if (volume > SB_VOLUME_MAX) {
        volume = SB_VOLUME_MAX;
    }
    s_volume = volume;
    if (s_i2s && !s_amp_gated) {
        i2s_alc_volume_set(s_i2s, volume_to_alc(volume));
    }
    ESP_LOGI(TAG, "volume=%d alc=%d dB%s", s_volume, volume_to_alc(s_volume),
             s_amp_gated ? " (amp gated)" : "");
    return ESP_OK;
}

int radio_player_get_volume(void)
{
    return s_volume;
}

int radio_player_nudge_volume(int delta)
{
    int v = s_volume + delta;
    if (v < SB_VOLUME_MIN) {
        v = SB_VOLUME_MIN;
    }
    if (v > SB_VOLUME_MAX) {
        v = SB_VOLUME_MAX;
    }
    ESP_LOGI(TAG, "nudge %d%+d -> %d max=%d", s_volume, delta, v, SB_VOLUME_MAX);
    radio_player_set_volume(v);
    return v;
}

bool radio_player_has_music_info(void)
{
    return s_got_music_info;
}

static void fill_tone(int16_t *dst, int frames, int hz, int amp)
{
    int edge = frames / 3;
    if (edge < 1) {
        edge = 1;
    }
    const float w = 6.28318530718f * (float)hz / (float)SB_MIX_SR;
    for (int i = 0; i < frames; i++) {
        int a = amp;
        if (i < edge) {
            a = (a * i) / edge;
        } else if (i > frames - 1 - edge) {
            a = (a * (frames - 1 - i)) / edge;
        }
        dst[i] = (int16_t)((float)a * sinf(w * (float)i));
    }
}

static void beep_prepare(void)
{
    if (s_beep_ready) {
        return;
    }
    if (!s_beep_pcm) {
        s_beep_pcm = audio_calloc(SB_BEEP_FRAMES, sizeof(int16_t));
    }
    if (!s_limit_pcm) {
        s_limit_pcm = audio_calloc(SB_LIMIT_FRAMES, sizeof(int16_t));
    }
    if (!s_beep_pcm || !s_limit_pcm) {
        ESP_LOGW(TAG, "beep pcm alloc failed");
        return;
    }
    fill_tone(s_beep_pcm, SB_BEEP_FRAMES, SB_BEEP_HZ, SB_BEEP_AMP);
    fill_tone(s_limit_pcm, SB_LIMIT_FRAMES, SB_LIMIT_HZ, SB_LIMIT_AMP);
    s_beep_ready = true;
}

static void beep_play(const int16_t *src, int frames)
{
    if (!s_have_out || !src || frames <= 0) {
        return;
    }
    beep_prepare();
    if (!s_beep_ready) {
        return;
    }
    s_beep_src = src;
    s_beep_len = frames;
    s_beep_pos = 0;
}

void radio_player_beep(void)
{
    beep_play(s_beep_pcm, SB_BEEP_FRAMES);
}

void radio_player_beep_limit(void)
{
    beep_play(s_limit_pcm, SB_LIMIT_FRAMES);
}

static void amp_gate(void)
{
    i2s_alc_gate();
    s_amp_gated = true;
}

static void amp_apply_saved(const char *why)
{
    if (!s_i2s) {
        return;
    }
    i2s_alc_volume_set(s_i2s, volume_to_alc(s_volume));
    s_amp_gated = false;
    ESP_LOGI(TAG, "amp ungated alc=%d dB%s", volume_to_alc(s_volume),
             why ? why : "");
}

void radio_player_ungate(void)
{
    amp_apply_saved("");
}

/* Overlay mix output. ADF tone_stream is a flash-MP3 reader and needs a mix
 * slot; radio-only BYPASS ignores extra slots, so a pipeline tone would duck
 * the station or stay silent. Duck content with the same edge as fill_tone
 * so the beep sits well above a hot stream without a clicky gain jump. */
static int beep_content_scale(int pos, int len)
{
    int edge = len / 3;
    int ducked = SB_BEEP_DUCK_FULL / SB_BEEP_CONTENT_DUCK;
    if (edge < 1) {
        edge = 1;
    }
    if (pos < edge) {
        return SB_BEEP_DUCK_FULL
            - ((SB_BEEP_DUCK_FULL - ducked) * pos) / edge;
    }
    if (pos > len - 1 - edge) {
        return SB_BEEP_DUCK_FULL
            - ((SB_BEEP_DUCK_FULL - ducked) * (len - 1 - pos)) / edge;
    }
    return ducked;
}

static void mix_beep_s16le(int16_t *samples, int frames, int channels)
{
    int pos = s_beep_pos;
    const int16_t *src = s_beep_src;
    int len = s_beep_len;
    if (pos < 0 || !src || len <= 0 || pos >= len || channels < 1) {
        return;
    }
    for (int i = 0; i < frames && pos < len; i++, pos++) {
        int16_t tone = src[pos];
        int scale = beep_content_scale(pos, len);
        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            int32_t mixed = ((int32_t)samples[idx] * scale) / SB_BEEP_DUCK_FULL
                            + tone;
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            samples[idx] = (int16_t)mixed;
        }
    }
    s_beep_pos = (pos >= len) ? -1 : pos;
}

/* Between-stations hiss + a faint drifting whistle. Overlay is after
 * pcm_note so it does not count as station energy. Stop when the tap
 * sees radio. Hiss is the floor — no impulse pops. */
#define SB_STATIC_HISS 8000
#define SB_STATIC_WHISTLE 500
#define SB_PH_PER_HZ 97391u

static int16_t analog_tune_sample(void)
{
    uint32_t x = s_static_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_static_rng = x;
    int n = (int)(int16_t)x;
    /* Pink-ish leaky integrator plus a little white: analog FM hiss. */
    s_static_lp += (n - s_static_lp) / 6;
    int hiss = (s_static_lp * 5 + n * 2) * (SB_STATIC_HISS / 7) / 32768;
    s_static_sweep++;
    uint32_t hz = 450u + ((s_static_sweep >> 9) & 0x3FFu);
    s_static_ph += hz * SB_PH_PER_HZ;
    int saw = (int)(int16_t)(s_static_ph >> 16);
    int whistle = (saw * SB_STATIC_WHISTLE) / 32768;
    int v = hiss + whistle;
    if (v > 32767) {
        v = 32767;
    } else if (v < -32768) {
        v = -32768;
    }
    return (int16_t)v;
}

static void mix_tune_static_s16le(int16_t *samples, int frames, int channels)
{
    if (!samples || frames <= 0 || channels < 1) {
        return;
    }
    for (int i = 0; i < frames; i++) {
        int16_t n = analog_tune_sample();
        for (int ch = 0; ch < channels; ch++) {
            int idx = i * channels + ch;
            int32_t mixed = (int32_t)samples[idx] + n;
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            samples[idx] = (int16_t)mixed;
        }
    }
}

static void probe_window_done(int zc, int n, int peak, int64_t now)
{
    if (n < 8 || peak < SB_PCM_VOICE_ABS) {
        s_probe_prev = 0;
        return;
    }
    int hz = (int)((zc * (int64_t)SB_MIX_SR) / (2 * n));
    if (s_probe_first_us == 0) {
        s_probe_first_us = now;
        s_probe_first_hz = hz;
    }
    s_probe_last_us = now;
    s_probe_last_hz = hz;
}

static void pcm_note_s16(const int16_t *s, int n, int channels)
{
    int peak = 0;
    int step = channels > 0 ? channels : 1;
    for (int i = 0; i < n; i++) {
        int a = s[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak) {
            peak = a;
        }
    }
    s_pcm_peak_now = peak;
    if (peak > s_pcm_peak_max) {
        s_pcm_peak_max = peak;
    }
    int64_t now = esp_timer_get_time();
    if (peak >= SB_PCM_HOLD_ABS) {
        s_pcm_last_voice_us = now;
        /* Station energy, not clip/static. Fill static stops here so a
         * later prebuffer does not keep hissing over the stream. */
        if (s_tune_static && !s_clip_active && !s_prebuffering) {
            s_tune_static = false;
            ESP_LOGI(TAG, "tune static off peak=%d", peak);
        }
    }
    if (s_probe_on) {
        for (int i = 0; i < n; i += step) {
            int v = s[i];
            int a = v < 0 ? -v : v;
            if (a > s_probe_win_peak) {
                s_probe_win_peak = a;
            }
            if (s_probe_n > 0 && ((s_probe_prev < 0) != (v < 0))) {
                s_probe_zc++;
            }
            s_probe_prev = v;
            s_probe_n++;
            if (s_probe_n >= SB_PROBE_WIN) {
                probe_window_done(s_probe_zc, s_probe_n, s_probe_win_peak, now);
                s_probe_zc = 0;
                s_probe_n = 0;
                s_probe_win_peak = 0;
            }
        }
    }
    if (peak < SB_PCM_VOICE_ABS) {
        return;
    }
    if (!s_pcm_heard) {
        s_pcm_heard = true;
        ESP_LOGI(TAG, "PCM reached amp peak=%d", peak);
        /* Ident/clips may ungate at the saved knob. Stream stays gated
         * until go_live / ensure_radio applies that knob, then routes. */
        if (s_amp_gated && s_i2s && s_clip_active && !s_running) {
            amp_apply_saved(" (clip)");
        }
    }
}

void radio_player_pcm_arm(void)
{
    s_pcm_heard = false;
    s_pcm_last_voice_us = 0;
    s_pcm_peak_max = 0;
    s_pcm_peak_now = 0;
}

bool radio_player_pcm_heard(void)
{
    return s_pcm_heard;
}

bool radio_player_pcm_flowing(void)
{
    if (!s_pcm_flowing) {
        return false;
    }
    /* Mixer tap stamps this; treat as stalled if PCM has not moved recently. */
    return (esp_timer_get_time() / 1000 - s_last_pcm_ms) < 500;
}

int radio_player_pcm_peak(void)
{
    return s_pcm_peak_max;
}

int radio_player_pcm_tap_peak(void)
{
    return s_pcm_peak_now;
}

bool radio_player_pcm_has_energy(void)
{
    int64_t last = s_pcm_last_voice_us;
    if (last <= 0) {
        return false;
    }
    /* Same 500 ms window as pcm_flowing, but mute-slot zeros do not count. */
    return (esp_timer_get_time() - last) < 500000;
}

bool radio_player_pcm_finished(int silence_ms)
{
    if (!s_pcm_heard || silence_ms <= 0) {
        return false;
    }
    int64_t last = s_pcm_last_voice_us;
    if (last <= 0) {
        return false;
    }
    return (esp_timer_get_time() - last) >= (int64_t)silence_ms * 1000;
}

void radio_player_probe_arm(void)
{
    s_probe_on = true;
    s_probe_prev = 0;
    s_probe_zc = 0;
    s_probe_n = 0;
    s_probe_win_peak = 0;
    s_probe_first_hz = 0;
    s_probe_last_hz = 0;
    s_probe_first_us = 0;
    s_probe_last_us = 0;
    radio_player_pcm_arm();
}

void radio_player_probe_result(radio_probe_result_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->peak = s_pcm_peak_max;
    out->start_hz = s_probe_first_hz;
    out->end_hz = s_probe_last_hz;
    if (s_probe_first_us > 0 && s_probe_last_us > s_probe_first_us) {
        out->dur_ms = (int)((s_probe_last_us - s_probe_first_us) / 1000);
    }
    out->start_ok = (s_probe_first_hz >= SB_PROBE_START_LO && s_probe_first_hz <= SB_PROBE_START_HI);
    out->end_ok = (s_probe_last_hz >= SB_PROBE_END_LO && s_probe_last_hz <= SB_PROBE_END_HI);
    s_probe_on = false;
}

static esp_err_t tap_open(audio_element_handle_t self)
{
    audio_element_info_t info = {0};
    info.sample_rates = SB_MIX_SR;
    info.bits = 16;
    info.channels = 2;
    audio_element_setinfo(self, &info);
    return ESP_OK;
}

static int tap_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r = audio_element_input(self, in_buffer, in_len);
    if (r <= 0) {
        return r;
    }
    s_last_pcm_ms = esp_timer_get_time() / 1000;
    s_pcm_flowing = true;
    if ((r & 1) == 0) {
        pcm_note_s16((int16_t *)in_buffer, r / 2, 2);
        if (s_beep_pos >= 0) {
            mix_beep_s16le((int16_t *)in_buffer, r / 4, 2);
        }
        if (s_tune_static && !s_clip_active && !s_hold_radio) {
            mix_tune_static_s16le((int16_t *)in_buffer, r / 4, 2);
        }
    }
    return audio_element_output(self, in_buffer, r);
}

static audio_element_handle_t tap_init(void)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = tap_open;
    cfg.process = tap_process;
    cfg.tag = "tap";
    cfg.out_rb_size = 8 * 1024;
    cfg.task_stack = 3 * 1024;
    cfg.task_prio = 5;
    cfg.task_core = 0;
    cfg.stack_in_ext = false; /* mix path: keep next to I2S */
    cfg.buffer_len = 2048;
    return audio_element_init(&cfg);
}

static int http_stream_event(http_stream_event_msg_t *msg)
{
    if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
        esp_http_client_handle_t client = (esp_http_client_handle_t)msg->http_client;
        esp_http_client_set_header(client, "Icy-MetaData", "0");
        esp_http_client_set_header(client, "User-Agent", stream_user_agent());
    }
    if (msg->event_id == HTTP_STREAM_POST_REQUEST
        || msg->event_id == HTTP_STREAM_ON_RESPONSE
        || msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
        audio_element_set_codec_fmt(msg->el, ESP_CODEC_TYPE_UNKNOW);
    }
    return ESP_OK;
}

static void board_codec_start(void)
{
    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle && board_handle->audio_hal) {
        audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    }
}

static void store_url_volume(const char *url, int volume)
{
    s_volume = volume;
    if (!url) {
        s_url[0] = 0;
        return;
    }
    if (strncmp(s_url, url, sizeof(s_url)) != 0) {
        s_force_https = false;
        s_using_http = false;
    }
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = 0;
}

static bool url_https_to_http(char *dst, size_t dst_sz, const char *src)
{
    if (!src || strncmp(src, "https://", 8) != 0) {
        return false;
    }
    if (dst_sz < strlen(src)) {
        return false;
    }
    snprintf(dst, dst_sz, "http://%s", src + 8);
    return true;
}

static const char *apply_stream_uri(void)
{
    static char s_http_url[256];
    s_using_http = false;
    if (!s_force_https && url_https_to_http(s_http_url, sizeof(s_http_url), s_url)) {
        s_using_http = true;
        if (s_http) {
            audio_element_set_uri(s_http, s_http_url);
        }
        return s_http_url;
    }
    if (s_http) {
        audio_element_set_uri(s_http, s_url);
    }
    return s_url;
}

static void radio_soft_restart(const char *reason);

static void radio_flush_pcm(void)
{
    if (s_radio_pcm) {
        rb_reset(s_radio_pcm);
    }
}

static void radio_prebuffer_begin(const char *why)
{
    int64_t now = esp_timer_get_time() / 1000;
    bool from_soft = why && strcmp(why, "soft-restart") == 0;
    s_prebuffering = true;
    s_prebuffer_since_ms = now;
    s_prebuffer_last_filled = http_rb_filled();
    s_prebuffer_snap_ms = 0;
    /* Do not zero s_prebuffer_reconnects on soft-restart: the loop increments
     * it then calls radio_soft_restart, which would never reach hard restart.
     * Hold uses s_hold_reconnects and does not go through this reset. */
    if (!from_soft) {
        s_prebuffer_reconnects = 0;
    }
    ESP_LOGI(TAG, "prebuffer %s until rb>=%d (now=%d/%d)",
             why ? why : "start", SB_HTTP_START_BYTES, s_prebuffer_last_filled,
             SB_HTTP_RB_SIZE);
    if (!from_soft) {
        stream_snap(why ? why : "prebuffer");
    }
    /* Analog between-stations hiss until the tap sees radio energy.
     * Slot 0 stays on s_radio_pcm; pause upmix so fill is not eaten. */
    s_tune_static = true;
    radio_pcm_pause();
}

static bool radio_prebuffer_release_if_ready(void)
{
    if (!s_prebuffering) {
        return false;
    }
    /* Do not wait for a clip. HTTP-full binds radio even if sintonizando
     * is still up (clip&&radio). Analog fill static is a tap overlay, not
     * a clip, so slot 0 can stay on s_radio_pcm. */
    if (!s_got_music_info || s_hold_radio) {
        return false;
    }
    int filled = http_rb_filled();
    /* WAIT and REFILL share the 224 KB start/recover watermark. */
    if (radio_buf_gate(RADIO_BUF_WAIT, filled) != RADIO_BUF_PLAY) {
        return false;
    }
    s_prebuffering = false;
    s_prebuffer_since_ms = 0;
    s_last_pcm_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "prebuffer ready rb=%d/%d", filled, SB_HTTP_RB_SIZE);
    /* Ensure_mix / ident start gated. Clips ungate while prefetching;
     * go_live gates again. Put the saved knob on I2S when the station
     * should actually play (vol 14 was still silent with peak=1). */
    amp_apply_saved(" (prebuffer ready)");
    mix_route_clip_and_radio();
    radio_pcm_resume();
    stream_snap("prebuffer-ready");
    return true;
}

static void radio_mark_started(void)
{
    s_running = true;
    s_prefetching = false;
    int64_t now = esp_timer_get_time() / 1000;
    s_last_pcm_ms = now;
    s_stall_grace_until_ms = now + SB_STREAM_STALL_GRACE_MS;
    s_stall_strikes = 0;
    s_restart_backoff_ms = 500;
    /* Mixer stays off the radio slot until HTTP rb reaches start (224 KB). */
    radio_prebuffer_begin("start");
}

static audio_event_iface_handle_t make_evt(void)
{
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    return audio_event_iface_init(&evt_cfg);
}

static void mix_use_rb(int slot, ringbuf_handle_t rb, int timeout)
{
    if (!s_downmix || !rb) {
        return;
    }
    /* Bind first, then timeout. A timeout-0 poke on the live base slot
     * cuts BYPASS audio (ADF #1010/#1069) and does not abort an in-flight
     * rb_read anyway. */
    downmix_set_input_rb(s_downmix, rb, slot);
    downmix_set_input_rb_timeout(s_downmix, timeout, slot);
}

static void mix_mute_slot(int slot)
{
    mix_use_rb(slot, slot == SB_SLOT_RADIO ? s_mute_radio : s_mute_clip,
               SB_MIX_MUTE_TIMEOUT);
}

/* Prefetch/prebuffer/hold must not eat decoded PCM (HTTP needs to fill).
 * QA 2f8bae9: swapping slot 0 mute→s_radio_pcm on a running BYPASS mixer
 * left pcmrb at 16384/16384 (downmix still on mute). Pause the upmix
 * instead; slot 0 stays on s_radio_pcm once go_live binds it stopped. */
static void radio_pcm_pause(void)
{
    if (!s_radio_m2s || s_radio_pcm_paused) {
        return;
    }
    if (audio_element_pause(s_radio_m2s) != ESP_OK) {
        ESP_LOGW(TAG, "radio pcm pause failed st=%d",
                 (int)audio_element_get_state(s_radio_m2s));
        return;
    }
    s_radio_pcm_paused = true;
    ESP_LOGI(TAG, "radio pcm pause");
}

static void radio_pcm_resume(void)
{
    if (!s_radio_m2s || !s_radio_pcm_paused) {
        return;
    }
    if (audio_element_resume(s_radio_m2s, 0, pdMS_TO_TICKS(2000)) != ESP_OK) {
        ESP_LOGW(TAG, "radio pcm resume failed st=%d",
                 (int)audio_element_get_state(s_radio_m2s));
        return;
    }
    s_radio_pcm_paused = false;
    ESP_LOGI(TAG, "radio pcm resume");
}

/* Software ALC does not cover leftover DMA. Keep the ALC field at -36
 * across driver start and set_clk; IDF 5 auto_clear wipes TX DMA. */
static void i2s_alc_gate(void)
{
    if (s_i2s) {
        i2s_alc_volume_set(s_i2s, SB_ALC_MIN_DB);
    }
}

static void i2s_set_clk_gated(void)
{
    if (!s_i2s) {
        return;
    }
    i2s_alc_gate();
    i2s_stream_set_clk(s_i2s, SB_MIX_SR, 16, 2);
    i2s_alc_gate();
}

static void mix_rewind(void)
{
    if (!s_mix_pipe) {
        return;
    }
    audio_pipeline_stop(s_mix_pipe);
    audio_pipeline_wait_for_stop(s_mix_pipe);
    audio_pipeline_reset_ringbuffer(s_mix_pipe);
    audio_pipeline_reset_elements(s_mix_pipe);
    audio_pipeline_reset_items_state(s_mix_pipe);
    audio_pipeline_change_state(s_mix_pipe, AEL_STATE_INIT);
}

static void mix_restart(void)
{
    if (!s_mix_pipe || !s_downmix) {
        return;
    }
    audio_element_state_t st = audio_element_get_state(s_downmix);
    audio_element_state_t i2s_st = s_i2s ? audio_element_get_state(s_i2s) : AEL_STATE_RUNNING;
    ESP_LOGW(TAG, "mix restart mix=%d i2s=%d", (int)st, (int)i2s_st);
    bool restore = s_i2s && !s_amp_gated;
    i2s_alc_gate();
    mix_rewind();
    s_mix_clip_mode = false;
    s_mix_off_until_ms = 0;
    /* Bind while stopped. Live downmix_set_input_rb(slot 0) does not
     * start draining s_radio_pcm (QA 2f8bae9: pcmrb stuck at cap). */
    mix_route_clip_and_radio();
    if (audio_pipeline_run(s_mix_pipe) != ESP_OK) {
        ESP_LOGE(TAG, "mix restart run failed");
        return;
    }
    i2s_set_clk_gated();
    if (restore) {
        amp_apply_saved(" (mix restart)");
    }
}

static void mix_restart_if_needed(void)
{
    if (!s_mix_pipe || !s_downmix) {
        return;
    }
    if (s_mix_hold_restart_until_ms) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now < s_mix_hold_restart_until_ms) {
            return;
        }
        s_mix_hold_restart_until_ms = 0;
    }
    audio_element_state_t st = audio_element_get_state(s_downmix);
    audio_element_state_t i2s_st = s_i2s ? audio_element_get_state(s_i2s) : AEL_STATE_RUNNING;
    if (st != AEL_STATE_FINISHED && st != AEL_STATE_STOPPED && st != AEL_STATE_ERROR
        && i2s_st != AEL_STATE_FINISHED && i2s_st != AEL_STATE_STOPPED && i2s_st != AEL_STATE_ERROR) {
        return;
    }
    mix_restart();
}

static void teardown_radio(void)
{
    s_radio_pcm = NULL;
    s_running = false;
    s_prefetching = false;
    s_prebuffering = false;
    s_radio_pcm_paused = false;
    s_prebuffer_since_ms = 0;
    s_hold_radio = false;
    s_hold_started_ms = 0;
    s_hold_reconnects = 0;
    s_need_hard_on_wifi = false;
    s_wifi_rejoin = false;
    s_sta_lost_pending = false;
    s_tune_static = false;
    mix_route_clip_and_radio();
    vTaskDelay(pdMS_TO_TICKS(30));
    if (s_radio_raw) {
        ringbuf_handle_t old = audio_element_get_input_ringbuf(s_radio_raw);
        if (old) {
            rb_abort(old);
        }
    }
    if (s_radio_pipe) {
        audio_pipeline_stop(s_radio_pipe);
        audio_pipeline_wait_for_stop(s_radio_pipe);
        audio_pipeline_terminate(s_radio_pipe);
        if (s_http) {
            audio_pipeline_unregister(s_radio_pipe, s_http);
        }
        if (s_aac) {
            audio_pipeline_unregister(s_radio_pipe, s_aac);
        }
        if (s_radio_m2s) {
            audio_pipeline_unregister(s_radio_pipe, s_radio_m2s);
        }
        if (s_radio_raw) {
            audio_pipeline_unregister(s_radio_pipe, s_radio_raw);
        }
        audio_pipeline_remove_listener(s_radio_pipe);
        audio_pipeline_deinit(s_radio_pipe);
        s_radio_pipe = NULL;
    }
    if (s_http) {
        audio_element_deinit(s_http);
        s_http = NULL;
    }
    if (s_aac) {
        audio_element_deinit(s_aac);
        s_aac = NULL;
    }
    if (s_radio_m2s) {
        audio_element_deinit(s_radio_m2s);
        s_radio_m2s = NULL;
    }
    if (s_radio_raw) {
        audio_element_deinit(s_radio_raw);
        s_radio_raw = NULL;
    }
    if (s_radio_evt) {
        audio_event_iface_destroy(s_radio_evt);
        s_radio_evt = NULL;
    }
    s_running = false;
    s_prefetching = false;
    s_prebuffering = false;
    s_prebuffer_since_ms = 0;
    s_got_music_info = false;
    s_using_http = false;
    s_radio_open_ms = 0;
}

static void teardown_mix(void)
{
    teardown_radio();
    if (s_mix_pipe) {
        audio_pipeline_stop(s_mix_pipe);
        audio_pipeline_wait_for_stop(s_mix_pipe);
        audio_pipeline_terminate(s_mix_pipe);
        if (s_downmix) {
            audio_pipeline_unregister(s_mix_pipe, s_downmix);
        }
        if (s_tap) {
            audio_pipeline_unregister(s_mix_pipe, s_tap);
        }
        if (s_i2s) {
            audio_pipeline_unregister(s_mix_pipe, s_i2s);
        }
        audio_pipeline_remove_listener(s_mix_pipe);
        audio_pipeline_deinit(s_mix_pipe);
        s_mix_pipe = NULL;
    }
    if (s_downmix) {
        audio_element_deinit(s_downmix);
        s_downmix = NULL;
    }
    if (s_tap) {
        audio_element_deinit(s_tap);
        s_tap = NULL;
    }
    if (s_i2s) {
        audio_element_deinit(s_i2s);
        s_i2s = NULL;
    }
    if (s_mix_evt) {
        audio_event_iface_destroy(s_mix_evt);
        s_mix_evt = NULL;
    }
    if (s_mute_radio) {
        rb_destroy(s_mute_radio);
        s_mute_radio = NULL;
    }
    if (s_mute_clip) {
        rb_destroy(s_mute_clip);
        s_mute_clip = NULL;
    }
    s_have_out = false;
    s_clip_active = false;
}

static esp_err_t ensure_mix(int volume)
{
    if (s_have_out) {
        radio_player_set_volume(volume);
        return ESP_OK;
    }
    /* Gate before the I2S driver or pins can clock the always-on amp. */
    s_amp_gated = true;
    s_volume = volume;
    board_codec_start();
    if (!s_mix_evt) {
        s_mix_evt = make_evt();
    }

    downmix_cfg_t mix_cfg = DEFAULT_DOWNMIX_CONFIG();
    mix_cfg.downmix_info.source_num = 2;
    mix_cfg.downmix_info.mode = ESP_DOWNMIX_WORK_MODE_BYPASS;
    mix_cfg.downmix_info.output_type = ESP_DOWNMIX_OUTPUT_TYPE_TWO_CHANNEL;
    mix_cfg.downmix_info.out_ctx = ESP_DOWNMIX_OUT_CTX_NORMAL;
    mix_cfg.task_stack = 4 * 1024;
    mix_cfg.task_prio = 6;
    mix_cfg.stack_in_ext = false; /* I2S feeder: internal stack */
    mix_cfg.out_rb_size = 8 * 1024;
    s_downmix = downmix_init(&mix_cfg);
    esp_downmix_input_info_t src[2] = {
        {.samplerate = SB_MIX_SR, .channel = 2, .bits_num = 16,
         .gain = {SB_MIX_RADIO_GAIN_DB, SB_MIX_RADIO_DUCK_DB}, .transit_time = SB_MIX_TRANSIT_MS},
        {.samplerate = SB_MIX_SR, .channel = 2, .bits_num = 16,
         .gain = {SB_MIX_CLIP_MUTE_DB, SB_MIX_CLIP_GAIN_DB}, .transit_time = SB_MIX_TRANSIT_MS},
    };
    source_info_init(s_downmix, src);
    s_mute_radio = rb_create(256, 4);
    s_mute_clip = rb_create(256, 4);
    mix_mute_slot(SB_SLOT_RADIO);
    mix_mute_slot(SB_SLOT_CLIP);

    s_tap = tap_init();

    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.use_alc = true;
    i2s_cfg.volume = SB_ALC_MIN_DB;
    i2s_cfg.chan_cfg.auto_clear = true;
    i2s_cfg.uninstall_drv = true;
    i2s_cfg.stack_in_ext = false;
    i2s_cfg.std_cfg.clk_cfg.sample_rate_hz = SB_MIX_SR;
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_RIGHT_LEFT);
    s_i2s = i2s_stream_init(&i2s_cfg);
    i2s_alc_gate();

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_mix_pipe = audio_pipeline_init(&pipeline_cfg);
    if (!s_mix_pipe || !s_downmix || !s_tap || !s_i2s) {
        ESP_LOGE(TAG, "mix init failed");
        teardown_mix();
        return ESP_FAIL;
    }
    audio_pipeline_register(s_mix_pipe, s_downmix, "mix");
    audio_pipeline_register(s_mix_pipe, s_tap, "tap");
    audio_pipeline_register(s_mix_pipe, s_i2s, "i2s");
    const char *link[3] = {"mix", "tap", "i2s"};
    audio_pipeline_link(s_mix_pipe, &link[0], 3);
    audio_pipeline_set_listener(s_mix_pipe, s_mix_evt);
    if (audio_pipeline_run(s_mix_pipe) != ESP_OK) {
        ESP_LOGE(TAG, "mix pipeline_run failed");
        teardown_mix();
        return ESP_FAIL;
    }
    i2s_set_clk_gated();
    beep_prepare();
    s_have_out = true;
    radio_player_pcm_arm();
    ESP_LOGI(TAG, "mix+I2S up (44100 stereo) alc=%d dB gated", SB_ALC_MIN_DB);
    return ESP_OK;
}

static esp_err_t ensure_radio(const char *url, int volume, bool hold)
{
    if (s_radio_pipe) {
        return ESP_OK;
    }
    if (ensure_mix(volume) != ESP_OK) {
        return ESP_FAIL;
    }
    mix_restart_if_needed();
    store_url_volume(url, volume);

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = false;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.event_handle = http_stream_event;
    http_cfg.user_agent = stream_user_agent();
    ESP_LOGI(TAG, "stream UA %s", http_cfg.user_agent);
    http_cfg.out_rb_size = SB_HTTP_RB_SIZE;
    http_cfg.task_stack = 5 * 1024;
    http_cfg.task_core = 1;
    /* Decode path: PSRAM stack. Mix/I2S/tap stay internal (cache-off DMA). */
    http_cfg.stack_in_ext = true;
    s_http = http_stream_init(&http_cfg);

    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
    aac_cfg.plus_enable = true;
    aac_cfg.out_rb_size = 16 * 1024;
    aac_cfg.task_stack = 8 * 1024;
    aac_cfg.task_core = 1;
    aac_cfg.task_prio = 6;
    aac_cfg.stack_in_ext = true;
    s_aac = aac_decoder_init(&aac_cfg);

    s_radio_m2s = pcm_upmix_init_core("rm2s", 1);
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 32 * 1024;
    s_radio_raw = raw_stream_init(&raw_cfg);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_radio_pipe = audio_pipeline_init(&pipeline_cfg);
    if (!s_radio_pipe || !s_http || !s_aac || !s_radio_m2s || !s_radio_raw) {
        ESP_LOGE(TAG, "radio init failed");
        teardown_radio();
        return ESP_FAIL;
    }
    audio_pipeline_register(s_radio_pipe, s_http, "http");
    audio_pipeline_register(s_radio_pipe, s_aac, "aac");
    audio_pipeline_register(s_radio_pipe, s_radio_m2s, "rm2s");
    audio_pipeline_register(s_radio_pipe, s_radio_raw, "rraw");
    const char *link[4] = {"http", "aac", "rm2s", "rraw"};
    audio_pipeline_link(s_radio_pipe, &link[0], 4);
    /* Prefer plain HTTP (no live TLS). crt_bundle stays attached so a
     * failed HTTP open can set the https:// URI and restart. */
    const char *play = apply_stream_uri();
    ESP_LOGI(TAG, "stream uri %s", play);
    s_radio_open_ms = esp_timer_get_time() / 1000;
    /* channels=0: upmix holds until decoder music_info (do not assume mono). */
    audio_element_set_music_info(s_radio_m2s, SB_MIX_SR, 0, 16);
    s_got_music_info = false;
    s_radio_pcm = audio_element_get_input_ringbuf(s_radio_raw);
    if (!s_radio_evt) {
        s_radio_evt = make_evt();
    }
    audio_pipeline_set_listener(s_radio_pipe, s_radio_evt);
    if (audio_pipeline_run(s_radio_pipe) != ESP_OK) {
        ESP_LOGE(TAG, "radio pipeline_run failed");
        teardown_radio();
        return ESP_FAIL;
    }
    if (hold) {
        s_prefetching = true;
        s_running = false;
        ESP_LOGI(TAG, "radio prefetch (mixer not reading yet)");
    } else {
        radio_mark_started();
        /* Saved knob before the mixer reads radio — no 0 dB / leftover burst. */
        amp_apply_saved(" (stream)");
        ESP_LOGI(TAG, "radio live (audible after HTTP rb>=%d)", SB_HTTP_START_BYTES);
    }
    mix_route_clip_and_radio();
    return ESP_OK;
}

static void mix_set_idle_mode(void)
{
    if (!s_downmix) {
        return;
    }
    /* Every SWITCH_ON exit goes SWITCH_OFF and waits transit_time.
     * JUMP SWITCH_ON→BYPASS (including clip-only → mute rb) left the
     * overnight tap at peak=1 while pcmrb still drained. mix_restart()
     * is not this. */
    if (s_mix_clip_mode) {
        s_mix_clip_mode = false;
        s_mix_off_until_ms = (esp_timer_get_time() / 1000) + SB_MIX_TRANSIT_MS;
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_SWITCH_OFF);
        return;
    }
    if (s_mix_off_until_ms) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now < s_mix_off_until_ms) {
            return;
        }
        s_mix_off_until_ms = 0;
    }
    downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_BYPASS);
}

static void mix_route_clip_and_radio(void)
{
    if (!s_downmix) {
        return;
    }
    /* Prefetch fills the radio pipe but must not feed the mixer — otherwise
     * go_live finds an empty PCM rb and the station underruns. */
    /* Hold / prebuffer pause the upmix so HTTP can fill. Slot 0 stays on
     * s_radio_pcm after prefetch (live mute→radio rebind does not drain). */
    bool radio = s_running && s_radio_pcm && s_got_music_info && !s_hold_radio
        && !s_prebuffering;
    bool clip = s_clip_active && s_clip_pcm;
    bool slot0_radio = s_radio_pcm && !s_prefetching;
    int slot0_to = radio ? SB_MIX_RADIO_TIMEOUT : SB_MIX_MUTE_TIMEOUT;
    if (clip && radio) {
        s_mix_clip_mode = true;
        s_mix_off_until_ms = 0;
        mix_use_rb(SB_SLOT_RADIO, s_radio_pcm, SB_MIX_MUTE_TIMEOUT);
        mix_use_rb(SB_SLOT_CLIP, s_clip_pcm, SB_MIX_CLIP_TIMEOUT);
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_SWITCH_ON);
        return;
    }
    if (clip) {
        /* BYPASS dies on any non-TIMEOUT on slot 0. Clip EOS/ABORT would
         * finish mix+I2S, so clip-only always uses SWITCH_ON: slot 0
         * timeout 1 plus clip on slot 1. Keep s_radio_pcm on slot 0 when
         * it is already bound (empty while upmix is paused). */
        s_mix_clip_mode = true;
        s_mix_off_until_ms = 0;
        if (slot0_radio) {
            mix_use_rb(SB_SLOT_RADIO, s_radio_pcm, SB_MIX_MUTE_TIMEOUT);
        } else {
            mix_mute_slot(SB_SLOT_RADIO);
        }
        mix_use_rb(SB_SLOT_CLIP, s_clip_pcm, SB_MIX_CLIP_TIMEOUT);
        downmix_set_work_mode(s_downmix, ESP_DOWNMIX_WORK_MODE_SWITCH_ON);
        return;
    }
    mix_mute_slot(SB_SLOT_CLIP);
    if (slot0_radio) {
        mix_use_rb(SB_SLOT_RADIO, s_radio_pcm, slot0_to);
    } else {
        mix_mute_slot(SB_SLOT_RADIO);
    }
    mix_set_idle_mode();
}

esp_err_t radio_player_attach_clip_pcm(ringbuf_handle_t rb)
{
    if (ensure_mix(s_volume) != ESP_OK) {
        return ESP_FAIL;
    }
    mix_restart_if_needed();
    s_clip_pcm = rb;
    mix_route_clip_and_radio();
    return ESP_OK;
}

void radio_player_set_clip_active(bool on)
{
    s_clip_active = on;
    mix_route_clip_and_radio();
}

esp_err_t radio_player_start_idle(int volume)
{
    ESP_LOGI(TAG, "Start idle audio out (clips)");
    if (ensure_mix(volume) != ESP_OK) {
        return ESP_FAIL;
    }
    mix_restart_if_needed();
    return ESP_OK;
}

esp_err_t radio_player_prefetch(const char *url, int volume)
{
    if (s_running || s_prefetching) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Prefetch stream %s", url);
    return ensure_radio(url, volume, true);
}

bool radio_player_is_prefetching(void)
{
    return s_prefetching;
}

bool radio_player_is_prebuffering(void)
{
    return s_prebuffering;
}

esp_err_t radio_player_go_live(void)
{
    if (!s_have_out) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_radio_pipe) {
        if (!s_url[0]) {
            return ESP_ERR_INVALID_STATE;
        }
        return ensure_radio(s_url, s_volume, false);
    }
    ESP_LOGI(TAG, "Go live — mixer reads radio after HTTP rb>=%d",
             SB_HTTP_START_BYTES);
    /* Always rewind mix here. After the ident clip the mixer may still
     * look RUNNING while wedged on an aborted clip rb; if-needed then
     * leaves the station silent. Bind s_radio_pcm while stopped, then
     * run. Stream stays gated through set_clk until the saved knob is
     * on I2S. */
    amp_gate();
    s_prefetching = false;
    mix_restart();
    drain_mix_evt();
    if (!s_running) {
        radio_mark_started();
    } else if (!s_prebuffering) {
        /* Prefetch already marked running? Still wait for almost-full. */
        radio_prebuffer_begin("go-live");
    }
    amp_apply_saved(" (stream)");
    if (!radio_prebuffer_release_if_ready()) {
        mix_route_clip_and_radio();
    }
    drain_mix_evt();
    s_mix_hold_restart_until_ms = (esp_timer_get_time() / 1000) + 1000;
    return ESP_OK;
}

esp_err_t radio_player_start(const char *url, int volume)
{
    if (s_prefetching && s_radio_pipe) {
        store_url_volume(url, volume);
        return radio_player_go_live();
    }
    if (s_running) {
        teardown_radio();
    }
    ESP_LOGI(TAG, "Start stream %s", url);
    return ensure_radio(url, volume, false);
}

bool radio_player_has_output(void)
{
    return s_have_out;
}

bool radio_player_is_running(void)
{
    return s_running;
}

void radio_player_request_stop(void)
{
    if (!s_running) {
        return;
    }
    s_want_stop = true;
}

void radio_player_resume(void)
{
    if (s_running || !s_url[0]) {
        return;
    }
    ESP_LOGI(TAG, "Resuming stream after pause");
    radio_player_start(s_url, s_volume);
}

void radio_player_stop(void)
{
    s_want_stop = false;
    teardown_radio();
}

static void radio_soft_restart(const char *reason)
{
    if (!s_radio_pipe) {
        return;
    }
    stream_snap(reason);
    ESP_LOGW(TAG, "%s — soft restart in %d ms (strike %d)", reason, s_restart_backoff_ms,
             s_stall_strikes + 1);
    audio_pipeline_stop(s_radio_pipe);
    audio_pipeline_wait_for_stop(s_radio_pipe);
    vTaskDelay(pdMS_TO_TICKS(s_restart_backoff_ms));
    if (s_restart_backoff_ms < 8000) {
        s_restart_backoff_ms *= 2;
    }
    audio_pipeline_reset_ringbuffer(s_radio_pipe);
    audio_pipeline_reset_elements(s_radio_pipe);
    audio_pipeline_reset_items_state(s_radio_pipe);
    if (s_radio_m2s) {
        audio_element_set_music_info(s_radio_m2s, SB_MIX_SR, 0, 16);
    }
    audio_pipeline_run(s_radio_pipe);
    int64_t now = esp_timer_get_time() / 1000;
    s_last_pcm_ms = now;
    s_stall_grace_until_ms = now + SB_STREAM_STALL_GRACE_MS;
    s_radio_open_ms = now;
    s_got_music_info = false;
    radio_prebuffer_begin("soft-restart");
    mix_route_clip_and_radio();
}

static void radio_hard_restart(const char *reason)
{
    if (!s_url[0]) {
        return;
    }
    stream_snap(reason);
    ESP_LOGW(TAG, "%s — hard restart of radio", reason);
    int vol = s_volume;
    char url[sizeof(s_url)];
    memcpy(url, s_url, sizeof(url));
    teardown_radio();
    vTaskDelay(pdMS_TO_TICKS(500));
    radio_player_start(url, vol);
}

static void radio_fallback_https(const char *reason)
{
    if (!s_using_http || !s_url[0]) {
        return;
    }
    s_force_https = true;
    const char *play = apply_stream_uri();
    ESP_LOGW(TAG, "HTTP failed (%s) — HTTPS fallback %s", reason, play);
    radio_soft_restart("HTTPS fallback");
}

static void check_stream_stall(void)
{
    if (!s_running || !s_got_music_info || s_clip_active || s_prebuffering) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_stall_grace_until_ms) {
        return;
    }
    int64_t idle = now - s_last_pcm_ms;
    if (idle < SB_STREAM_STALL_MS) {
        return;
    }
    s_stall_strikes++;
    ESP_LOGW(TAG, "stream stall: no PCM for %lld ms strike=%d", (long long)idle,
             s_stall_strikes);
    stream_snap("pcm-stall");
    if (s_stall_strikes >= SB_STREAM_HARD_RESTART_AFTER && s_url[0]) {
        radio_hard_restart("stream stall");
        return;
    }
    radio_soft_restart("stream stall");
}

/* s_running && !s_got_music_info after HTTPS fallback / DNS fail.
 * Periodic soft then hard restart so a dead HTTP element does not stay
 * silent until power-cycle. */
static bool check_stream_join(void)
{
    if (!s_running || s_got_music_info || s_clip_active || s_hold_radio) {
        return false;
    }
    if (!s_radio_pipe || !s_radio_open_ms || s_using_http) {
        return false;
    }
    if (!sta_associated()) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_radio_open_ms < SB_JOIN_RETRY_MS) {
        return false;
    }
    s_stall_strikes++;
    if (s_stall_strikes >= SB_STREAM_HARD_RESTART_AFTER && s_url[0]) {
        /* Rebuild and try HTTP Icecast first, then HTTPS fallback again. */
        s_force_https = false;
        radio_hard_restart("join fail");
        return true;
    }
    radio_soft_restart("join fail");
    return true;
}

static void drain_mix_evt(void)
{
    if (!s_mix_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    while (audio_event_iface_listen(s_mix_evt, &msg, 0) == ESP_OK) {
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && msg.source == (void *)s_downmix) {
            int st = (int)msg.data;
            if (st == AEL_STATUS_STATE_FINISHED || st == AEL_STATUS_STATE_STOPPED
                || st == AEL_STATUS_ERROR_PROCESS || st == AEL_STATUS_ERROR_INPUT) {
                ESP_LOGW(TAG, "mix status=%d", st);
            }
        }
    }
}

void radio_player_arm_rssi_threshold(void)
{
    esp_err_t err = esp_wifi_set_rssi_threshold(SB_WIFI_RSSI_WEAK_DBM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rssi threshold %d dBm: %s", SB_WIFI_RSSI_WEAK_DBM,
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "rssi threshold %d dBm", SB_WIFI_RSSI_WEAK_DBM);
}

static int http_rb_filled(void)
{
    if (!s_http) {
        return -1;
    }
    ringbuf_handle_t rb = audio_element_get_output_ringbuf(s_http);
    return rb ? rb_bytes_filled(rb) : -1;
}

int radio_player_http_buffered(void)
{
    if (!s_running && !s_prefetching && !s_prebuffering) {
        return -1;
    }
    return http_rb_filled();
}

static void stream_snap(const char *why)
{
    int filled = http_rb_filled();
    int pcm = s_radio_pcm ? (int)rb_bytes_filled(s_radio_pcm) : -1;
    int pcm_cap = s_radio_pcm ? PCM_UPMIX_OUT_RB_SIZE : 0;
    int peak = s_pcm_peak_now;
    wifi_ap_record_t ap = {0};
    bool assoc = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    int64_t now = esp_timer_get_time() / 1000;
    int pcm_idle = s_last_pcm_ms ? (int)(now - s_last_pcm_ms) : -1;
    int hold_ms = s_hold_started_ms ? (int)(now - s_hold_started_ms) : 0;
    int need = radio_buf_play_need((int)s_wifi_weak_latched);
    /* W so log_shipper flushes. One line: rb vs PCM vs tap energy vs Wi-Fi. */
    ESP_LOGW(TAG,
             "stall %s rb=%d/%d need=%d pcmrb=%d/%d peak=%d music=%d hold=%d prebuf=%d weak=%d "
             "hold_ms=%d recon=%d rssi=%d assoc=%d pcm_idle=%d http_el=%d aac_el=%d "
             "tls=%d clip=%d run=%d slot0pcm=%d paused=%d",
             why ? why : "-", filled, SB_HTTP_RB_SIZE, need, pcm, pcm_cap, peak,
             (int)s_got_music_info, (int)s_hold_radio, (int)s_prebuffering,
             (int)s_wifi_weak_latched,
             hold_ms, s_hold_reconnects, assoc ? ap.rssi : 0, (int)assoc, pcm_idle,
             s_http ? (int)audio_element_get_state(s_http) : -1,
             s_aac ? (int)audio_element_get_state(s_aac) : -1,
             s_using_http ? 0 : 1, (int)s_clip_active, (int)s_running,
             (int)(s_radio_pcm && !s_prefetching), (int)s_radio_pcm_paused);
}

void radio_player_log_health(const char *why)
{
    stream_snap(why);
}

static bool sta_associated(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static bool sta_rssi_is_weak(void)
{
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    return ap.rssi <= SB_WIFI_RSSI_WEAK_DBM;
}

static bool http_buf_hungry(void)
{
    int filled = http_rb_filled();
    return filled < SB_WIFI_HTTP_LOW_BYTES;
}

static bool http_buf_critical(void)
{
    int filled = http_rb_filled();
    return filled >= 0 && filled < SB_HTTP_SLOW_LOW_BYTES;
}

void radio_player_on_rssi_low(int rssi_dbm)
{
    s_wifi_weak_latched = true;
    ESP_LOGW(TAG, "RSSI low (%d dBm) HTTP rb=%d/%d", rssi_dbm, http_rb_filled(),
             SB_HTTP_RB_SIZE);
}

void radio_player_on_sta_lost(void)
{
    /* sys_evt stack is tiny — only flags here. Snap/mix/clip in radio_player_loop. */
    s_wifi_weak_latched = false;
    if (s_running || s_hold_radio) {
        s_need_hard_on_wifi = true;
        s_stop_clip = true;
        s_sta_lost_pending = true;
        if (!s_hold_radio) {
            s_hold_radio = true;
            s_hold_started_ms = esp_timer_get_time() / 1000;
            s_hold_last_filled = http_rb_filled();
            s_hold_reconnects = 0;
        }
    }
}

void radio_player_on_sta_got_ip(void)
{
    if (s_need_hard_on_wifi) {
        s_wifi_rejoin = true;
    }
}

bool radio_player_take_stop_clip(void)
{
    bool stop = s_stop_clip;
    s_stop_clip = false;
    return stop;
}

void radio_player_hold_stream(bool on)
{
    if (s_hold_radio == on) {
        return;
    }
    s_hold_radio = on;
    if (on) {
        /* Prompt owns silence; do not keep a parallel prebuffer mute. */
        s_prebuffering = false;
        s_prebuffer_since_ms = 0;
        s_hold_started_ms = esp_timer_get_time() / 1000;
        s_hold_last_filled = http_rb_filled();
        s_hold_reconnects = 0;
        s_hold_snap_ms = 0;
        stream_snap("hold");
        radio_pcm_pause();
    } else {
        stream_snap("resume");
        radio_flush_pcm();
        s_last_pcm_ms = esp_timer_get_time() / 1000;
        s_hold_started_ms = 0;
        s_wifi_weak_latched = false;
        radio_player_arm_rssi_threshold();
        radio_pcm_resume();
    }
    mix_route_clip_and_radio();
}

bool radio_player_wifi_weak_holding(void)
{
    return s_hold_radio;
}

bool radio_player_wifi_weak_resume_ready(void)
{
    if (!s_hold_radio) {
        return false;
    }
    if (!s_got_music_info) {
        return false;
    }
    int filled = http_rb_filled();
    int need = radio_buf_play_need((int)s_wifi_weak_latched);
    if (filled >= need) {
        stream_snap("refill-ready");
        return true;
    }
    return false;
}

bool radio_player_wifi_weak_should_speak(void)
{
    if (s_hold_radio || s_clip_active || s_prefetching || s_prebuffering) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now < s_stall_grace_until_ms) {
        return false;
    }
    if (!http_buf_hungry()) {
        return false;
    }
    if (!s_wifi_weak_latched) {
        /* Event can miss if RSSI was already below the threshold. Confirm
         * only when the HTTP rb is actually hurting. */
        if (!sta_rssi_is_weak()) {
            return false;
        }
        s_wifi_weak_latched = true;
    }
    if (s_wifi_weak_prompt_ms
        && (now - s_wifi_weak_prompt_ms) < SB_BUFFER_PROMPT_COOLDOWN_MS) {
        return false;
    }
    s_wifi_weak_prompt_ms = now;
    stream_snap("wifi-weak");
    return true;
}

bool radio_player_http_slow_should_speak(void)
{
    if (s_hold_radio || s_clip_active || s_prefetching) {
        return false;
    }
    if (!s_running || !s_got_music_info) {
        return false;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_wifi_weak_latched || sta_rssi_is_weak() || !sta_associated()) {
        s_http_slow_low_since = 0;
        return false;
    }
    int filled = http_rb_filled();
    bool critical = filled >= 0 && filled < SB_HTTP_SLOW_LOW_BYTES;
    /* last_filled is the rb at prebuffer_begin; the 25 s reconnect loop
     * does not touch it until then. Healthy 8 KB/s grows ~96 KB in 12 s. */
    bool stuck = s_prebuffering && s_prebuffer_since_ms
        && (now - s_prebuffer_since_ms) >= SB_HTTP_SLOW_PREBUF_SPEAK_MS
        && filled >= 0 && filled < SB_HTTP_RECOVER_BYTES
        && (filled - s_prebuffer_last_filled) < SB_HTTP_SLOW_GROW_BYTES;
    if (!critical && !stuck) {
        s_http_slow_low_since = 0;
        return false;
    }
    /* Stall grace is for post-start jitter. Growth-based stuck already
     * waited ~12 s with no fill; do not let 35 s of grace hide it. */
    if (!stuck && now < s_stall_grace_until_ms) {
        s_http_slow_low_since = 0;
        return false;
    }
    if (critical) {
        if (!s_http_slow_low_since) {
            s_http_slow_low_since = now;
            return false;
        }
        if ((now - s_http_slow_low_since) < SB_HTTP_SLOW_DEBOUNCE_MS) {
            return false;
        }
    }
    if (s_http_slow_prompt_ms
        && (now - s_http_slow_prompt_ms) < SB_BUFFER_PROMPT_COOLDOWN_MS) {
        return false;
    }
    s_http_slow_prompt_ms = now;
    s_http_slow_low_since = 0;
    stream_snap("http-slow");
    return true;
}

void radio_player_loop(void)
{
    listen_stats_poll();
    if (s_want_stop) {
        radio_player_stop();
        return;
    }
    if (!s_have_out) {
        return;
    }
    if (s_sta_lost_pending) {
        s_sta_lost_pending = false;
        stream_snap("sta-lost");
        mix_route_clip_and_radio();
    }
    if (s_wifi_rejoin) {
        s_wifi_rejoin = false;
        s_need_hard_on_wifi = false;
        stream_snap("sta-got-ip");
        if (s_url[0]) {
            radio_hard_restart("wifi back");
            return;
        }
    }
    if (s_radio_pipe && s_using_http && !s_got_music_info && s_radio_open_ms) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_radio_open_ms >= SB_HTTP_FALLBACK_MS) {
            if (!sta_associated()) {
                static int64_t s_fb_skip_log_ms;
                if (now - s_fb_skip_log_ms >= 30000) {
                    s_fb_skip_log_ms = now;
                    ESP_LOGW(TAG, "HTTPS fallback skipped — no wifi");
                }
            } else {
                radio_fallback_https("no music_info");
            }
        }
    }
    drain_mix_evt();
    mix_restart_if_needed();
    {
        int filled = http_rb_filled();
        int64_t now = esp_timer_get_time() / 1000;
        if (s_prebuffering) {
            if (radio_prebuffer_release_if_ready()) {
                s_rb_drop_band = -1;
            } else if (s_prebuffer_snap_ms == 0 || (now - s_prebuffer_snap_ms) >= 2000) {
                s_prebuffer_snap_ms = now;
                stream_snap("prebuffer-tick");
            }
        } else if (s_hold_radio) {
            int hold_ms = s_hold_started_ms ? (int)(now - s_hold_started_ms) : 0;
            int period = (!sta_associated() || hold_ms > 30000) ? 30000 : 5000;
            if (s_hold_snap_ms == 0 || (now - s_hold_snap_ms) >= period) {
                s_hold_snap_ms = now;
                stream_snap("hold-tick");
            }
        } else if (s_running && filled >= 0 && s_got_music_info && !s_clip_active
                   && radio_buf_underrun(filled)
                   && !s_wifi_weak_latched && !sta_rssi_is_weak()) {
            /* Good Wi-Fi but fill dipped under 64 KB: mute and refill to 224 KB.
             * A real slow server can still speak internet lenta while muted. */
            radio_prebuffer_begin("rb-drop");
            mix_route_clip_and_radio();
            s_rb_drop_band = filled / (32 * 1024);
        } else if (s_running && filled >= 0) {
            if (filled < SB_WIFI_HTTP_LOW_BYTES) {
                int band = filled / (32 * 1024);
                if (band != s_rb_drop_band) {
                    s_rb_drop_band = band;
                    stream_snap("rb-drop");
                }
            } else {
                s_rb_drop_band = -1;
            }
        }
        /* Re-assert timeouts while live and clip-off. Slot 0 stays on
         * s_radio_pcm; this is not a mute↔radio swap and not mix_restart. */
        if (s_running && s_radio_pcm && s_got_music_info && !s_hold_radio
            && !s_prebuffering && !s_clip_active) {
            mix_route_clip_and_radio();
        }
    }
    if (s_prebuffering && !s_hold_radio && s_running && s_http
        && s_prebuffer_since_ms > 0 && sta_associated() && !s_need_hard_on_wifi) {
        int64_t now = esp_timer_get_time() / 1000;
        int filled = http_rb_filled();
        if (now - s_prebuffer_since_ms >= SB_HTTP_SLOW_RECONNECT_MS
            && filled >= 0 && filled < SB_HTTP_RECOVER_BYTES) {
            int grew = filled - s_prebuffer_last_filled;
            s_prebuffer_last_filled = filled;
            if (grew >= SB_HTTP_SLOW_GROW_BYTES) {
                s_prebuffer_since_ms = now;
                ESP_LOGW(TAG, "prebuffer filling rb=%d grew=%d — wait",
                         filled, grew);
            } else if (s_prebuffer_reconnects + 1 >= SB_HTTP_SLOW_RECONNECT_MAX) {
                radio_hard_restart("prebuffer — still empty");
                return;
            } else {
                s_prebuffer_reconnects++;
                radio_soft_restart("prebuffer — reconnect");
                return;
            }
        }
    }
    if (s_hold_radio && !s_wifi_weak_latched && s_running && s_http
        && s_hold_started_ms > 0 && sta_associated() && !s_need_hard_on_wifi) {
        int64_t now = esp_timer_get_time() / 1000;
        int filled = http_rb_filled();
        if (now - s_hold_started_ms >= SB_HTTP_SLOW_RECONNECT_MS
            && filled >= 0 && filled < SB_HTTP_SLOW_RESUME_BYTES) {
            int grew = filled - s_hold_last_filled;
            s_hold_last_filled = filled;
            if (grew >= SB_HTTP_SLOW_GROW_BYTES) {
                s_hold_started_ms = now;
                ESP_LOGW(TAG, "HTTP slow hold filling rb=%d grew=%d — wait",
                         filled, grew);
            } else if (s_hold_reconnects + 1 >= SB_HTTP_SLOW_RECONNECT_MAX) {
                radio_hard_restart("HTTP slow hold — still empty");
                return;
            } else {
                s_hold_reconnects++;
                radio_soft_restart("HTTP slow hold — reconnect");
                s_hold_started_ms = now;
                s_hold_last_filled = 0;
            }
        }
    }
    if (s_running && !s_clip_active && !s_hold_radio && !s_wifi_weak_latched) {
        check_stream_stall();
    }
    if (check_stream_join()) {
        return;
    }
    if (!s_radio_evt) {
        return;
    }
    audio_event_iface_msg_t msg;
    esp_err_t ret = audio_event_iface_listen(s_radio_evt, &msg, 0);
    if (ret != ESP_OK) {
        return;
    }
    /* Fall through to handle this event (loop may be called often). */
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)s_aac
        && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t music_info = {0};
        audio_element_getinfo(s_aac, &music_info);
        ESP_LOGI(TAG, "stream music info rate=%d bits=%d ch=%d (mix locked 44100 stereo)",
                 music_info.sample_rates, music_info.bits, music_info.channels);
        int ch = music_info.channels > 0 ? music_info.channels : 2;
        int rate = music_info.sample_rates > 0 ? music_info.sample_rates : SB_MIX_SR;
        if (s_radio_m2s) {
            audio_element_set_music_info(s_radio_m2s, rate, ch, 16);
        }
        /* Flush only while prefetching. Doing it after go-live dumps the
         * PCM that was already playing (clean first slice, then underrun). */
        if (!s_running) {
            radio_flush_pcm();
        }
        if (rate >= 16000) {
            s_got_music_info = true;
        }
        s_restart_backoff_ms = 500;
        s_stall_strikes = 0;
        s_last_pcm_ms = esp_timer_get_time() / 1000;
        if (!radio_prebuffer_release_if_ready()) {
            mix_route_clip_and_radio();
        }
        if (s_hold_radio || s_prebuffering) {
            stream_snap("music-info");
        }
        ESP_LOGI(TAG, "heap after music: free=%u spiram=%u internal=%u tls=%d",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 s_using_http ? 0 : 1);
        return;
    }
    if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
        && (msg.source == (void *)s_http || msg.source == (void *)s_aac)) {
        int st = (int)msg.data;
        int *prev = (msg.source == (void *)s_http) ? &s_http_evt_st : &s_aac_evt_st;
        if (st != *prev) {
            *prev = st;
            ESP_LOGW(TAG, "stream evt %s status=%d",
                     msg.source == (void *)s_http ? "http" : "aac", st);
            if (st <= AEL_STATUS_ERROR_UNKNOWN || st == AEL_STATUS_STATE_FINISHED
                || st == AEL_STATUS_STATE_STOPPED || st == AEL_STATUS_INPUT_DONE) {
                stream_snap("el-evt");
            }
        }
    }
    bool bad = msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
        && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
        && ((int)msg.data == AEL_STATUS_ERROR_OPEN || (int)msg.data == AEL_STATUS_ERROR_INPUT
            || (int)msg.data == AEL_STATUS_ERROR_PROCESS)
        && (msg.source == (void *)s_http || msg.source == (void *)s_aac);
    if (!bad) {
        return;
    }
    /* Empty HTTP rb is normal before the first music_info. Ignoring
     * ERROR_OPEN then left HTTPS-fallback / DNS fail silent forever. */
    if (s_clip_active || s_hold_radio || s_wifi_weak_latched
        || (s_got_music_info && http_buf_critical())) {
        ESP_LOGW(TAG, "stream err ignored hold=%d clip=%d weak=%d rb=%d st=%d",
                 (int)s_hold_radio, (int)s_clip_active, (int)s_wifi_weak_latched,
                 http_rb_filled(), (int)msg.data);
        return;
    }
    if (s_using_http && !s_got_music_info) {
        radio_fallback_https("stream/decoder error");
        return;
    }
    s_stall_strikes++;
    if (s_stall_strikes >= SB_STREAM_HARD_RESTART_AFTER && s_url[0]) {
        ESP_LOGW(TAG, "stream/decoder error — hard restart");
        if (!s_got_music_info) {
            s_force_https = false;
        }
        int vol = s_volume;
        char url[sizeof(s_url)];
        memcpy(url, s_url, sizeof(url));
        teardown_radio();
        vTaskDelay(pdMS_TO_TICKS(s_restart_backoff_ms));
        if (s_restart_backoff_ms < 8000) {
            s_restart_backoff_ms *= 2;
        }
        radio_player_start(url, vol);
        return;
    }
    radio_soft_restart("stream/decoder error");
}

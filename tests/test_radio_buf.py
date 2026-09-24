#!/usr/bin/env python3
"""Host spec for radio HTTP ringbuffer start / recover watermarks.

Live 0.5.25 on Lucas QA Zero (d0:cf:13:07:de:fc):
  stall rb-drop rb≈98–101kB / 262144 need=131072 every ~200–300 ms.

Root cause encoded here:
  mixer consume ≈ Icecast produce, so fill hovers where play started.
  Start/resume at ~100 KB with need=128 KB sits on the 96 KB log-band edge.

Firmware policy lives in firmware/main/radio_buf.h (compiled below).
One 224 KB fill for start / recover / wifi-weak / http-slow. Leave PLAY
at 64 KB. Silence from empty is ~28 s at ~8 KB/s (~20 s from underrun).
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware" / "main" / "radio_buf.h"

# Historical (0.5.25 / PR #3-era) numbers the live box is hitting.
OLD_START_HOVER = 100 * 1024  # post-ident prefetch, then go_live immediately
OLD_NEED = 128 * 1024  # SB_HTTP_SLOW_RESUME_BYTES — need=131072 in logs
OLD_RB_DROP_LINE = 128 * 1024
CAPACITY = 256 * 1024
# Policy this PR checks (must match radio_buf.h).
FILL = 224 * 1024
START = FILL
RECOVER = FILL
UNDERRUN = 64 * 1024
WIFI_RESUME = FILL
SLOW_RESUME = FILL


def parse_c_int_defines(path: Path) -> dict[str, int]:
    text = path.read_text()
    defs: dict[str, int] = {}
    # Resolve one-level aliases after collecting literals.
    lit = re.compile(
        r"^#define\s+(SB_[A-Z0-9_]+|RADIO_BUF_[A-Z0-9_]+)\s+"
        r"(?:\((\d+)\s*\*\s*(\d+)\)|(\d+)|([A-Z0-9_]+))\s*$",
        re.M,
    )
    pending: list[tuple[str, str]] = []
    for m in lit.finditer(text):
        name, a, b, n, alias = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if a and b:
            defs[name] = int(a) * int(b)
        elif n:
            defs[name] = int(n)
        elif alias:
            pending.append((name, alias))
    for name, alias in pending:
        assert alias in defs, f"{name} aliases unknown {alias}"
        defs[name] = defs[alias]
    return defs


def test_header_numbers() -> dict[str, int]:
    d = parse_c_int_defines(HEADER)
    assert d["SB_HTTP_RB_SIZE"] == CAPACITY
    assert d["SB_HTTP_START_BYTES"] == START
    assert d["SB_HTTP_RECOVER_BYTES"] == RECOVER
    assert d["SB_HTTP_UNDERRUN_BYTES"] == UNDERRUN
    assert d["SB_HTTP_SLOW_LOW_BYTES"] == 64 * 1024
    assert d["SB_HTTP_SLOW_RESUME_BYTES"] == SLOW_RESUME
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == WIFI_RESUME
    assert d["SB_WIFI_HTTP_LOW_BYTES"] == d["SB_HTTP_UNDERRUN_BYTES"]
    assert d["SB_HTTP_START_BYTES"] < d["SB_HTTP_RB_SIZE"]
    # One fill height for every resume path.
    assert d["SB_HTTP_START_BYTES"] == FILL == 229376
    assert d["SB_HTTP_RECOVER_BYTES"] == FILL
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == FILL
    assert d["SB_HTTP_SLOW_RESUME_BYTES"] == FILL
    # Above the live hover so go_live does not start on the 96 KB band edge.
    assert d["SB_HTTP_START_BYTES"] > OLD_START_HOVER
    # Leave PLAY at 64 KB, not 96 KB. Long quiet up to 224 KB is the policy.
    assert d["SB_HTTP_UNDERRUN_BYTES"] == 64 * 1024
    assert d["SB_HTTP_UNDERRUN_BYTES"] != 96 * 1024
    assert d["SB_HTTP_UNDERRUN_BYTES"] < d["SB_HTTP_RECOVER_BYTES"]
    return d


def test_python_hysteresis(d: dict[str, int]) -> None:
    start = d["SB_HTTP_START_BYTES"]
    recover = d["SB_HTTP_RECOVER_BYTES"]
    underrun = d["SB_HTTP_UNDERRUN_BYTES"]

    assert OLD_START_HOVER < start
    assert recover == start == FILL
    assert underrun == 64 * 1024

    def gate(now: int, filled: int) -> int:
        if now == d["RADIO_BUF_PLAY"]:
            return d["RADIO_BUF_REFILL"] if 0 <= filled < underrun else d["RADIO_BUF_PLAY"]
        if now == d["RADIO_BUF_REFILL"]:
            return d["RADIO_BUF_PLAY"] if filled >= recover else d["RADIO_BUF_REFILL"]
        return d["RADIO_BUF_PLAY"] if filled >= start else d["RADIO_BUF_WAIT"]

    # Boot: 100 KB after ident stays silent; 224 KB goes live.
    st = d["RADIO_BUF_WAIT"]
    for filled in (0, 64 * 1024, OLD_START_HOVER, 112 * 1024, 200 * 1024, start - 1):
        st = gate(st, filled)
        assert st == d["RADIO_BUF_WAIT"], f"started too early at {filled}"
    st = gate(st, start)
    assert st == d["RADIO_BUF_PLAY"]

    # PLAY stays PLAY until fill drops under 64 KB (96 KB is too high).
    st = gate(d["RADIO_BUF_PLAY"], OLD_START_HOVER)
    assert st == d["RADIO_BUF_PLAY"], "100 KB is still above the 64 KB leave-PLAY line"
    st = gate(d["RADIO_BUF_PLAY"], 96 * 1024)
    assert st == d["RADIO_BUF_PLAY"], "96 KB must not start a refill"
    st = gate(d["RADIO_BUF_PLAY"], 98 * 1024)
    assert st == d["RADIO_BUF_PLAY"]
    st = gate(d["RADIO_BUF_PLAY"], 101 * 1024)
    assert st == d["RADIO_BUF_PLAY"]
    st = gate(d["RADIO_BUF_PLAY"], underrun)
    assert st == d["RADIO_BUF_PLAY"], "exactly 64 KB is still PLAY"

    # Dip under 64 KB: mute and refill all the way to 224 KB.
    st = gate(d["RADIO_BUF_PLAY"], underrun - 1)
    assert st == d["RADIO_BUF_REFILL"]
    st = gate(st, OLD_START_HOVER)
    assert st == d["RADIO_BUF_REFILL"], "100 KB is still below 224 KB recover"
    st = gate(st, OLD_NEED)
    assert st == d["RADIO_BUF_REFILL"], "128 KB is still below 224 KB recover"
    st = gate(st, 200 * 1024)
    assert st == d["RADIO_BUF_REFILL"], "200 KB is still below 224 KB recover"
    st = gate(d["RADIO_BUF_REFILL"], recover)
    assert st == d["RADIO_BUF_PLAY"]

    # Healthy hover at start/recover stays PLAY. 64 KB is the floor, not a mute.
    for filled in (recover, underrun, 200 * 1024):
        st = gate(d["RADIO_BUF_PLAY"], filled)
        assert st == d["RADIO_BUF_PLAY"], f"false underrun at {filled}"


def test_c_gate_matches(d: dict[str, int]) -> None:
    harness = r"""
#include <stdio.h>
#include "radio_buf.h"
int main(void) {
    printf("cap=%d start=%d recover=%d underrun=%d need0=%d need1=%d\n",
           SB_HTTP_RB_SIZE, SB_HTTP_START_BYTES, SB_HTTP_RECOVER_BYTES,
           SB_HTTP_UNDERRUN_BYTES, radio_buf_play_need(0), radio_buf_play_need(1));
    int samples[] = {0, 65536, 98304, 102400, 114688, 131072, 204800, 229376, 262144};
    const char *names[] = {"wait", "play", "refill"};
    for (int n = 0; n < 3; n++) {
        for (int i = 0; i < (int)(sizeof(samples)/sizeof(samples[0])); i++) {
            printf("%s %d -> %d\n", names[n], samples[i],
                   radio_buf_gate(n, samples[i]));
        }
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        (tdir / "harness.c").write_text(harness)
        bin_path = tdir / "test_radio_buf"
        subprocess.check_call(
            [
                "gcc", "-O0", "-Wall", "-Werror",
                "-I", str(HEADER.parent),
                str(tdir / "harness.c"),
                "-o", str(bin_path),
            ]
        )
        out = subprocess.check_output([str(bin_path)], text=True)
    lines = out.strip().splitlines()
    hdr = lines[0]
    assert f"cap={CAPACITY}" in hdr
    assert f"start={d['SB_HTTP_START_BYTES']}" in hdr
    assert f"recover={d['SB_HTTP_RECOVER_BYTES']}" in hdr
    assert f"underrun={d['SB_HTTP_UNDERRUN_BYTES']}" in hdr
    assert f"need0={FILL}" in hdr
    assert f"need1={FILL}" in hdr
    assert d["SB_HTTP_RECOVER_BYTES"] == FILL
    assert d["SB_WIFI_HTTP_RESUME_BYTES"] == FILL

    name_to_now = {"wait": 0, "play": 1, "refill": 2}
    for line in lines[1:]:
        name, rest = line.split(" ", 1)
        filled_s, got_s = rest.split(" -> ")
        filled = int(filled_s)
        got = int(got_s)
        now = name_to_now[name]
        if now == 1:
            want = 2 if 0 <= filled < d["SB_HTTP_UNDERRUN_BYTES"] else 1
        elif now == 2:
            want = 1 if filled >= d["SB_HTTP_RECOVER_BYTES"] else 2
        else:
            want = 1 if filled >= d["SB_HTTP_START_BYTES"] else 0
        assert got == want, f"{line}: expected {want}"

    # Explicit live-bug / policy cases against the C gate.
    assert "wait 102400 -> 0" in out  # old ident hover: still wait
    assert "wait 114688 -> 0" in out  # 112 KB still wait
    assert "wait 204800 -> 0" in out  # 200 KB still wait
    assert "wait 229376 -> 1" in out  # 224 KB: play
    assert "play 102400 -> 1" in out  # 100 KB is above 64 KB: stay PLAY
    assert "play 98304 -> 1" in out   # 96 KB must not start a refill
    assert "play 65536 -> 1" in out   # exactly 64 KB: stay PLAY
    assert "play 0 -> 2" in out       # empty: refill
    assert "refill 102400 -> 2" in out  # 100 KB still shy of 224
    assert "refill 114688 -> 2" in out  # 112 KB still shy of 224
    assert "refill 204800 -> 2" in out  # 200 KB still shy of 224
    assert "refill 229376 -> 1" in out


def _radio_player_int(rp: str, name: str) -> int:
    m = re.search(rf"^#define {name} (?:\((\d+)\s*\*\s*(\d+)\)|(\d+))\s*$", rp, re.M)
    assert m, f"{name} missing"
    if m.group(1) and m.group(2):
        return int(m.group(1)) * int(m.group(2))
    return int(m.group(3))


def test_prebuf_speak_is_growth_based() -> int:
    """Speak on a stuck prebuffer before the 25 s reconnect resets the clock.

    A timer ≥ 25 s never fires: radio_player_loop soft-restarts and resets
    s_prebuffer_since_ms. Healthy 8 KB/s grows ~96 KB in 12 s, so a
    growth check stays silent on a climb toward 224 KB.
    """
    rp = (ROOT / "firmware" / "main" / "radio_player.c").read_text()
    speak_ms = _radio_player_int(rp, "SB_HTTP_SLOW_PREBUF_SPEAK_MS")
    reconnect_ms = _radio_player_int(rp, "SB_HTTP_SLOW_RECONNECT_MS")
    grow = _radio_player_int(rp, "SB_HTTP_SLOW_GROW_BYTES")
    assert reconnect_ms == 25000
    assert grow == 16 * 1024
    assert speak_ms < reconnect_ms, (
        f"prebuf speak {speak_ms} ms must be below reconnect {reconnect_ms} ms"
    )
    assert 10000 <= speak_ms <= 20000, f"growth window should be ~12 s, got {speak_ms}"
    speak = rp[rp.index("bool radio_player_http_slow_should_speak(void)"):]
    speak = speak.split("\nvoid ", 1)[0]
    assert "SB_HTTP_SLOW_PREBUF_SPEAK_MS" in speak
    assert "s_prebuffer_last_filled" in speak
    assert "SB_HTTP_SLOW_GROW_BYTES" in speak
    assert "s_prebuffering" in speak
    assert "SB_HTTP_RECOVER_BYTES" in speak
    assert "SB_HTTP_SLOW_LOW_BYTES" in speak

    recover = FILL
    last = 0

    def stuck(now_ms: int, filled: int, last_filled: int) -> bool:
        return (
            now_ms >= speak_ms
            and filled >= 0
            and filled < recover
            and (filled - last_filled) < grow
        )

    # Healthy 8 KB/s from empty: 12 s → ~96 KB, not stuck; ready at ~28 s.
    healthy_12 = int(8 * 1024 * (speak_ms / 1000))
    assert healthy_12 > grow
    assert not stuck(speak_ms, healthy_12, last)
    assert not stuck(speak_ms, FILL - 1, last)  # still climbing
    # Stuck 0 B/s from empty, and stuck at the ~100 KB ident hover.
    assert stuck(speak_ms, 0, 0)
    assert stuck(speak_ms, OLD_START_HOVER, OLD_START_HOVER)
    # Trickle 1 KB/s: 12 KB < 16 KB grow floor → speak.
    assert stuck(speak_ms, int(1 * 1024 * (speak_ms / 1000)), 0)
    return speak_ms


def _fn_until(rp: str, start_token: str, end_token: str) -> str:
    start = rp.index(start_token)
    end = rp.index(end_token, start + 10)
    return rp[start:end]


def test_prebuf_release_binds_radio_under_clip() -> None:
    """QA Zero 605bc23: slot0pcm=1, mix restart, pcmrb full, tap peak=1.

    Clips are audible through SWITCH_ON. Station BYPASS left those samples
    at tap peak=1. Radio-only uses SWITCH_ON plus a mute clip slot.
    Stall decpeak= is the upmix (rm2s) block peak, before downmix.
    """
    rp = (ROOT / "firmware/main/radio_player.c").read_text()
    sc = (ROOT / "firmware/main/serial_cmd.c").read_text()
    ls = (ROOT / "firmware/main/listen_stats.c").read_text()
    uh = (ROOT / "firmware/main/pcm_upmix.h").read_text()
    uc = (ROOT / "firmware/main/pcm_upmix.c").read_text()

    release = _fn_until(
        rp,
        "static bool radio_prebuffer_release_if_ready(void)",
        "static void radio_mark_started(void)",
    )
    assert "s_prebuffering = false" in release
    assert "mix_restart()" in release
    assert "radio_pcm_resume()" not in release
    assert "if (!s_got_music_info || s_hold_radio)" in release
    assert "|| s_clip_active" not in release
    assert "amp_apply_saved" in release
    assert release.index("s_prebuffering = false") < release.index("mix_restart()")

    begin = _fn_until(
        rp,
        "static void radio_prebuffer_begin(const char *why)",
        "static bool radio_prebuffer_release_if_ready(void)",
    )
    assert "radio_pcm_pause()" not in begin
    assert "mix_restart()" not in begin
    assert "analog_tune_sample" not in begin

    rst = _fn_until(rp, "static void mix_restart(void)\n{",
                    "static void mix_restart_if_needed(void)")
    assert rst.index("mix_route_clip_and_radio()") < rst.index("audio_pipeline_run")
    assert rst.index("mix_apply_gains(") < rst.index("audio_pipeline_run")
    assert "mix_mute_slot(SB_SLOT_RADIO)" not in rst
    assert "duck ? SB_MIX_CLIP_GAIN_DB : SB_MIX_CLIP_MUTE_DB" not in rp

    gains = _fn_until(rp, "static void mix_apply_gains(bool duck)",
                      "static void mix_restart(void)\n{")
    assert "SB_MIX_CLIP_GAIN_DB" in gains
    assert "downmix_set_gain_info" in gains
    assert "source_info_init" in gains
    assert "{SB_MIX_CLIP_MUTE_DB, SB_MIX_CLIP_GAIN_DB}" in gains
    assert "SB_MIX_CLIP_MUTE_DB, SB_MIX_CLIP_MUTE_DB" not in gains
    assert "duck ? SB_MIX_RADIO_DUCK_DB : SB_MIX_RADIO_GAIN_DB" in gains

    gl = _fn_until(rp, "esp_err_t radio_player_go_live(void)",
                   "esp_err_t radio_player_start(const char *url, int volume)")
    assert gl.index("s_prefetching = false") < gl.index("mix_restart()")
    assert gl.index("radio_mark_started()") < gl.index("mix_restart()")

    ens = _fn_until(rp, "static esp_err_t ensure_radio(const char *url, int volume, bool hold)",
                    "static void mix_set_idle_mode(void)")
    after_pcm = ens.split("s_radio_pcm = audio_element_get_input_ringbuf(s_radio_raw)", 1)[1]
    # Prefetch keeps slot 0 mute. Non-prefetch / hard restart: fill first
    # so the stopped bind is mute, not radio.
    assert "s_prefetching = true" in after_pcm
    assert "mix_restart()" in after_pcm
    assert after_pcm.index("if (hold)") < after_pcm.index("mix_restart()")
    assert after_pcm.index("radio_mark_started()") < after_pcm.index("mix_restart()")

    assert "s_aac = aac_decoder_init(&aac_cfg);" in ens
    assert "plus_enable = true" in ens
    assert "helix_lc" not in rp
    assert not (ROOT / "firmware/components/helix_lc").exists()

    assert "analog_tune_sample" not in rp
    assert "mix_tune_static_s16le" not in rp
    assert "s_tune_static" not in rp

    route = _fn_until(rp, "static void mix_route_clip_and_radio(void)\n{",
                      "esp_err_t radio_player_attach_clip_pcm")
    both = route.split("if (clip && radio)")[1].split("if (clip)")[0]
    assert "SB_MIX_MUTE_TIMEOUT" in both
    assert "SB_MIX_RADIO_TIMEOUT" not in both
    assert "mix_apply_gains(true)" in both
    assert "slot0_radio" not in route
    assert "s_radio_pcm && !s_prefetching" not in route
    assert "!s_prebuffering" in route
    radio_only = route.split("mix_mute_slot(SB_SLOT_CLIP)")[1]
    assert "SB_MIX_RADIO_TIMEOUT" in radio_only
    assert "ESP_DOWNMIX_WORK_MODE_SWITCH_ON" in radio_only
    assert "mix_apply_gains(false)" in radio_only
    clip_only = route.split("if (clip)")[1].split("mix_mute_slot(SB_SLOT_CLIP)")[0]
    assert "mix_apply_gains(false)" in clip_only
    idle = _fn_until(rp, "static void mix_set_idle_mode(void)",
                     "static void mix_route_clip_and_radio(void)\n{")
    assert "ESP_DOWNMIX_WORK_MODE_SWITCH_OFF" in idle
    assert "s_mix_clip_mode" in idle
    assert "s_mix_off_until_ms" in idle
    assert "SB_MIX_TRANSIT_MS" in idle
    assert "ESP_DOWNMIX_WORK_MODE_BYPASS" not in route
    assert "mix_set_idle_mode()" in route
    assert route.strip().endswith("mix_set_idle_mode();\n}")
    assert "#define SB_MIX_TRANSIT_MS 150" in rp
    assert "SB_STATIC_POP" not in rp
    assert "audio_element_pause" not in rp
    assert "audio_element_resume" not in rp
    assert "s_radio_pcm_paused" not in rp

    use_rb = _fn_until(rp, "static void mix_use_rb(int slot, ringbuf_handle_t rb, int timeout)",
                       "static void mix_mute_slot(int slot)")
    assert "downmix_set_input_rb(s_downmix, rb, slot)" in use_rb
    assert use_rb.index("downmix_set_input_rb(s_downmix, rb, slot)") < use_rb.index(
        "downmix_set_input_rb_timeout"
    )
    assert "downmix_set_input_rb_timeout(s_downmix, 0, slot)" not in use_rb

    loop = rp.split("void radio_player_loop(void)", 1)[1]
    drop = loop.split('radio_prebuffer_begin("rb-drop")', 1)[1][:350]
    assert "mix_route_clip_and_radio()" in drop
    assert "mix_restart()" not in drop.split("s_rb_drop_band", 1)[0]
    live = loop.split("Re-assert timeouts while live", 1)[1][:500]
    assert "mix_route_clip_and_radio()" in live
    assert "!s_clip_active" in live
    assert "mix_restart()" not in live

    soft = _fn_until(rp, "static void radio_soft_restart(const char *reason)\n{",
                     "static void radio_hard_restart(const char *reason)")
    assert "mix_route_clip_and_radio()" in soft
    assert "mix_restart()" not in soft
    assert "radio_pcm_resume()" not in soft
    assert "s_radio_pcm_paused" not in soft
    assert 'radio_prebuffer_begin("soft-restart")' in soft

    hold = _fn_until(rp, "void radio_player_hold_stream(bool on)",
                     "bool radio_player_wifi_weak_holding(void)")
    assert "mix_route_clip_and_radio()" in hold
    assert "mix_restart()" in hold
    assert hold.index("mix_route_clip_and_radio()") < hold.index("mix_restart()")
    assert "radio_pcm_pause()" not in hold
    assert "radio_pcm_resume()" not in hold
    assert "radio_flush_pcm()" not in hold

    snap = _fn_until(rp, "static void stream_snap(const char *why)",
                     "void radio_player_log_health(const char *why)")
    assert "pcmrb=%d/%d" in snap
    assert "peak=%d" in snap
    assert "decpeak=%d" in snap
    assert "pcm_upmix_radio_peak()" in snap
    assert "s_pcm_peak_now" in snap
    assert "PCM_UPMIX_OUT_RB_SIZE" in snap
    assert "slot0pcm=%d" in snap
    assert "paused=%d" not in snap
    assert "!s_prebuffering" in snap

    assert "#define PCM_UPMIX_OUT_RB_SIZE (16 * 1024)" in uh
    assert "PCM_UPMIX_OUT_RB_SIZE" in uc
    assert "pcm_upmix_radio_peak" in uh
    assert 'strcmp(tag, "rm2s")' in uc
    assert "decoded peak=" in uc
    assert "radio_player_pcm_tap_peak()" in sc
    assert "peak=%d" in sc[sc.index('if (strcasecmp(line, "http")'):]

    station = _fn_until(ls, "static bool station_pcm_now(void)", "void listen_stats_poll(void)")
    assert "radio_player_pcm_flowing()" in station
    assert "radio_player_pcm_has_energy()" in station
    assert "s_pcm_last_voice_us" in rp[rp.index("bool radio_player_pcm_has_energy(void)"):
                                      rp.index("bool radio_player_pcm_finished")]


def main() -> int:
    d = test_header_numbers()
    test_python_hysteresis(d)
    test_c_gate_matches(d)
    speak_ms = test_prebuf_speak_is_growth_based()
    test_prebuf_release_binds_radio_under_clip()
    print("test_radio_buf: ok")
    print(
        f"  start={d['SB_HTTP_START_BYTES']} recover={d['SB_HTTP_RECOVER_BYTES']} "
        f"underrun={d['SB_HTTP_UNDERRUN_BYTES']} cap={d['SB_HTTP_RB_SIZE']}"
    )
    print(
        f"  before: start~{OLD_START_HOVER} recover={OLD_NEED} "
        f"(need=131072) / {CAPACITY}"
    )
    print(
        f"  after:  start={d['SB_HTTP_START_BYTES']} "
        f"recover={d['SB_HTTP_RECOVER_BYTES']} "
        f"underrun={d['SB_HTTP_UNDERRUN_BYTES']} "
        f"wifi_resume={d['SB_WIFI_HTTP_RESUME_BYTES']} "
        f"slow_resume={d['SB_HTTP_SLOW_RESUME_BYTES']} "
        f"(need={FILL}) / {CAPACITY}"
    )
    print(f"  prebuf_speak_ms={speak_ms}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

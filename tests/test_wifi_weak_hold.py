#!/usr/bin/env python3
"""wifi-weak hold: reconnect only when HTTP has already finished.

Michael House 0.5.29 latched wifi-weak, held the stream, then HTTP finished
(status=15, http_el=6). Ring froze at 67258; resume needs 229376. recon stayed
0 and weak stayed 1 for hours while assoc=1. Hold-reconnect requires the weak
latch clear. A finished HTTP element cannot refill, so resume never fires.

Do not treat this as net_slow (#10) or rb-drop (#2). Do not reconnect a live
HTTP element during a weak hold.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RP = (ROOT / "firmware/main/radio_player.c").read_text()
BUF = (ROOT / "firmware/main/radio_buf.h").read_text()

AEL_STATE_FINISHED = 6  # http_el=6 in the stall snap
MICHAEL_RB = 67258
NEED = 224 * 1024
RECONNECT_MS = 25000


def _loop() -> str:
    return RP.split("void radio_player_loop(void)", 1)[1]


def _block_from(loop: str, token: str) -> str:
    start = loop.index(token)
    end = None
    for sep in ("\n    if (", "\n    /*"):
        i = loop.find(sep, start + len(token))
        if i > start and (end is None or i < end):
            end = i
    assert end is not None, f"no end after {token[:48]!r}"
    return loop[start:end]


def slow_hold_reconnect() -> str:
    return _block_from(
        _loop(),
        "if (s_hold_radio && !s_wifi_weak_latched && s_running && s_http",
    )


def weak_hold_http_ended() -> str:
    return _block_from(
        _loop(),
        "if (s_hold_radio && s_wifi_weak_latched && s_running && s_http",
    )


def resume_ready() -> str:
    start = RP.index("bool radio_player_wifi_weak_resume_ready(void)")
    end = RP.index("bool radio_player_wifi_weak_should_speak(void)")
    return RP[start:end]


def should_reconnect_weak_hold(
    *,
    hold: bool,
    weak: bool,
    running: bool,
    assoc: bool,
    need_hard: bool,
    hold_ms: int,
    filled: int,
    need: int,
    http_st: int,
) -> bool:
    if not (hold and weak and running and assoc and not need_hard):
        return False
    if hold_ms < RECONNECT_MS:
        return False
    if filled < 0 or filled >= need:
        return False
    return http_st == AEL_STATE_FINISHED


def test_slow_hold_still_requires_weak_clear() -> None:
    block = slow_hold_reconnect()
    assert "!s_wifi_weak_latched" in block
    assert 'radio_soft_restart("HTTP slow hold — reconnect")' in block
    assert "AEL_STATE_FINISHED" not in block
    assert "wifi-weak hold" not in block


def test_weak_hold_reconnects_finished_http_below_resume() -> None:
    block = weak_hold_http_ended()
    assert "s_wifi_weak_latched" in block
    assert "!s_wifi_weak_latched" not in block
    assert "AEL_STATE_FINISHED" in block
    assert "radio_buf_play_need(1)" in block
    assert "SB_HTTP_SLOW_RECONNECT_MS" in block
    assert 'radio_soft_restart("wifi-weak hold — HTTP ended")' in block
    assert 'radio_hard_restart("wifi-weak hold — HTTP ended")' in block
    assert "SB_HTTP_SLOW_RECONNECT_MAX" in block
    assert "sta_associated()" in block
    # Live HTTP during a weak hold must keep filling; do not steal that path.
    assert "AEL_STATE_RUNNING" not in block
    assert "HTTP slow hold" not in block
    assert "net_slow" not in block
    assert "rb-drop" not in block


def test_michael_house_state_reconnects() -> None:
    assert NEED == 229376
    assert "SB_HTTP_START_BYTES (224 * 1024)" in BUF
    # Frozen ring, finished HTTP, weak latch, still associated.
    assert should_reconnect_weak_hold(
        hold=True,
        weak=True,
        running=True,
        assoc=True,
        need_hard=False,
        hold_ms=51218,
        filled=MICHAEL_RB,
        need=NEED,
        http_st=AEL_STATE_FINISHED,
    )
    # Same box 4 h later: still reconnect, not a one-shot event.
    assert should_reconnect_weak_hold(
        hold=True,
        weak=True,
        running=True,
        assoc=True,
        need_hard=False,
        hold_ms=14489386,
        filled=MICHAEL_RB,
        need=NEED,
        http_st=AEL_STATE_FINISHED,
    )


def test_does_not_reconnect_live_or_full_http() -> None:
    live = dict(
        hold=True,
        weak=True,
        running=True,
        assoc=True,
        need_hard=False,
        hold_ms=51218,
        filled=MICHAEL_RB,
        need=NEED,
        http_st=3,  # AEL_STATE_RUNNING
    )
    assert not should_reconnect_weak_hold(**live)
    full = dict(live, http_st=AEL_STATE_FINISHED, filled=NEED)
    assert not should_reconnect_weak_hold(**full)
    too_soon = dict(live, http_st=AEL_STATE_FINISHED, hold_ms=5000)
    assert not should_reconnect_weak_hold(**too_soon)
    not_weak = dict(live, weak=False, http_st=AEL_STATE_FINISHED)
    assert not should_reconnect_weak_hold(**not_weak)
    no_assoc = dict(live, assoc=False, http_st=AEL_STATE_FINISHED)
    assert not should_reconnect_weak_hold(**no_assoc)


def test_resume_still_needs_224k() -> None:
    fn = resume_ready()
    assert "radio_buf_play_need((int)s_wifi_weak_latched)" in fn
    assert "filled >= need" in fn
    assert "refill-ready" in fn
    # Finished HTTP is not a resume; rb=67258 stays below 229376.
    assert MICHAEL_RB < NEED


def test_does_not_broaden_stall_or_err_ignore() -> None:
    loop = _loop()
    stall_gate = loop.split("check_stream_stall();", 1)[0]
    assert stall_gate.rstrip().endswith(
        "if (s_running && !s_clip_active && !s_hold_radio && !s_wifi_weak_latched) {"
    )
    assert "s_clip_active || s_hold_radio || s_wifi_weak_latched" in loop
    assert "stream err ignored" in loop
    drop = loop.split('radio_prebuffer_begin("rb-drop")', 1)[0]
    assert "!s_wifi_weak_latched && !sta_rssi_is_weak()" in drop


def test_slow_and_weak_paths_stay_separate() -> None:
    slow = slow_hold_reconnect()
    weak = weak_hold_http_ended()
    assert slow.index("!s_wifi_weak_latched") < slow.index("HTTP slow hold")
    assert weak.index("s_wifi_weak_latched") < weak.index("AEL_STATE_FINISHED")
    # Insertion order: slow hold (weak clear) then finished-HTTP weak hold.
    loop = _loop()
    assert loop.index("!s_wifi_weak_latched && s_running && s_http") < loop.index(
        "s_wifi_weak_latched && s_running && s_http"
    )


if __name__ == "__main__":
    test_slow_hold_still_requires_weak_clear()
    test_weak_hold_reconnects_finished_http_below_resume()
    test_michael_house_state_reconnects()
    test_does_not_reconnect_live_or_full_http()
    test_resume_still_needs_224k()
    test_does_not_broaden_stall_or_err_ignore()
    test_slow_and_weak_paths_stay_separate()
    print("ok")

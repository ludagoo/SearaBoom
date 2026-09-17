#!/usr/bin/env python3
"""Join never got music_info: retry after HTTPS fallback / DNS fail.

Stall check still needs music_info. HTTP fallback still needs s_using_http.
Empty HTTP rb must not ignore ERROR_OPEN before the first music_info.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RP = (ROOT / "firmware/main/radio_player.c").read_text()


def _fn(name: str) -> str:
    key = f"static bool {name}(void)" if name == "check_stream_join" else f"static void {name}(void)"
    if name == "check_stream_join":
        start = RP.index("static bool check_stream_join(void)")
    else:
        start = RP.index(key)
    nxt = RP.find("\nstatic ", start + 10)
    loop = RP.find("\nvoid radio_player_loop(void)", start + 10)
    end = min(x for x in (nxt, loop) if x > start)
    return RP[start:end]


def test_join_retry_timer_exists() -> None:
    assert "#define SB_JOIN_RETRY_MS 20000" in RP
    assert "#define SB_HTTP_FALLBACK_MS 8000" in RP
    assert "#define SB_STREAM_HARD_RESTART_AFTER 3" in RP


def test_stall_still_requires_music_info() -> None:
    stall = _fn("check_stream_stall")
    assert "!s_got_music_info" in stall or "s_got_music_info" in stall
    assert "if (!s_running || !s_got_music_info || s_clip_active)" in stall
    assert "join fail" not in stall


def test_http_fallback_still_http_only() -> None:
    loop = RP.split("void radio_player_loop(void)", 1)[1]
    assert "s_using_http && !s_got_music_info" in loop
    assert 'radio_fallback_https("no music_info")' in loop


def test_join_retry_after_https_no_music() -> None:
    join = _fn("check_stream_join")
    assert "!s_running || s_got_music_info" in join
    assert "s_using_http" in join
    assert "SB_JOIN_RETRY_MS" in join
    assert 'radio_soft_restart("join fail")' in join
    assert 'radio_hard_restart("join fail")' in join
    assert "s_force_https = false" in join
    loop = RP.split("void radio_player_loop(void)", 1)[1]
    assert "check_stream_join()" in loop.split("if (!s_radio_evt)", 1)[0]


def test_empty_rb_does_not_ignore_join_open_error() -> None:
    loop = RP.split("void radio_player_loop(void)", 1)[1]
    assert "s_got_music_info && http_buf_critical()" in loop
    ignored = loop.split("stream err ignored", 1)[0]
    # The ignore gate immediately before the log must require music_info
    # for the empty-rb starve path — not bare http_buf_critical().
    tail = ignored[-400:]
    assert "http_buf_critical()" in tail
    assert "s_got_music_info && http_buf_critical()" in tail
    assert "|| http_buf_critical()" not in tail


def test_does_not_reimplement_net_slow_or_welcome() -> None:
    join = _fn("check_stream_join")
    assert "net_slow" not in join
    assert "welcome" not in join
    assert "SB_MIX_RADIO_TIMEOUT" not in join
    assert "sb_wifi_should_play_welcome" not in RP


if __name__ == "__main__":
    test_join_retry_timer_exists()
    test_stall_still_requires_music_info()
    test_http_fallback_still_http_only()
    test_join_retry_after_https_no_music()
    test_empty_rb_does_not_ignore_join_open_error()
    test_does_not_reimplement_net_slow_or_welcome()
    print("ok")

#!/usr/bin/env python3
"""Host tests for SoftAP / welcome boot policy (wifi_boot.h).

First-setup (empty NVS) still gets setup AP + ap_welcome.
A saved SSID that will not join, or a named box with an empty ssid, must not.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIFI_BOOT_H = ROOT / "firmware" / "main" / "wifi_boot.h"
MAIN_C = ROOT / "firmware" / "main" / "main.c"
PORTAL_C = ROOT / "firmware" / "main" / "captive_portal.c"

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "wifi_boot.h"

static int fail(const char *msg)
{
    fprintf(stderr, "fail: %s\n", msg);
    return 1;
}

int main(void)
{
    /* Empty NVS / factory: first-setup. */
    if (!sb_wifi_is_first_setup("", "", "", "URL1")) {
        return fail("blank cfg should be first-setup");
    }
    if (!sb_wifi_should_start_portal("", "", "", "URL1", false)) {
        return fail("blank cfg should start portal");
    }
    if (!sb_wifi_should_play_welcome("", "", "", "URL1", false)) {
        return fail("blank cfg should play welcome");
    }

    /* QA: saved SSID join-fail. */
    if (sb_wifi_is_first_setup("GOOSE!", "", "", "URL2")) {
        return fail("saved ssid is not first-setup");
    }
    if (sb_wifi_should_start_portal("GOOSE!", "Lucas QA Zero", "", "URL2", false)) {
        return fail("join-fail must not start SoftAP");
    }
    if (sb_wifi_should_play_welcome("GOOSE!", "Lucas QA Zero", "", "URL2", false)) {
        return fail("join-fail must not play welcome");
    }

    /* Elwyn: empty ssid, identity still set. */
    if (sb_wifi_is_first_setup("", "Elwyn - Silver", "Hesston", "URL2")) {
        return fail("named empty-ssid is not first-setup");
    }
    if (sb_wifi_should_start_portal("", "Elwyn - Silver", "Hesston", "URL2", false)) {
        return fail("named empty-ssid must not start SoftAP");
    }
    if (sb_wifi_should_play_welcome("", "Elwyn - Silver", "Hesston", "URL2", false)) {
        return fail("named empty-ssid must not play welcome");
    }

    /* URL2 alone (user chose a station) is not first-setup. */
    if (sb_wifi_is_first_setup("", "", "", "URL2")) {
        return fail("URL2 without ssid is not first-setup");
    }
    if (sb_wifi_should_start_portal("", "", "", "URL2", false)) {
        return fail("URL2 without ssid must not start SoftAP");
    }

    /* Explicit wipe: SoftAP for reconfig, no welcome if identity remains. */
    if (!sb_wifi_should_start_portal("", "Elwyn - Silver", "Hesston", "URL2", true)) {
        return fail("force_ap should start portal");
    }
    if (sb_wifi_should_play_welcome("", "Elwyn - Silver", "Hesston", "URL2", true)) {
        return fail("force_ap with identity must not play welcome");
    }

    /* Wipe of a never-named box is still first-setup welcome. */
    if (!sb_wifi_should_play_welcome("", "", "", "URL1", true)) {
        return fail("force_ap on blank box should still welcome");
    }
    return 0;
}
"""


def test_policy_matrix() -> None:
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        src = tdir / "harness.c"
        src.write_text(HARNESS)
        bin_path = tdir / "test_wifi_boot"
        subprocess.check_call(
            [
                "gcc", "-O0", "-Wall", "-Werror",
                "-I", str(WIFI_BOOT_H.parent),
                str(src),
                "-o", str(bin_path),
            ]
        )
        subprocess.check_call([str(bin_path)])


def test_firmware_does_not_treat_join_fail_as_setup() -> None:
    main = MAIN_C.read_text()
    portal = PORTAL_C.read_text()
    assert "wifi_connect_or_setup(&play_welcome)" in main
    assert "SB_WIFI_NEED_SETUP" in main
    assert "SB_WIFI_NO_IP" in main
    assert "quiet STA retry, no welcome" in main
    assert "Restored ssid=" in main
    assert "captive_portal_run(play_welcome)" in main
    assert "if (!wifi_connect_or_setup())" not in main
    assert "clip_player_loop(SB_CLIP_AP_WELCOME)" not in main
    assert "if (play_welcome)" in portal
    assert "skip welcome clip (not first-setup)" in portal


def main() -> int:
    test_policy_matrix()
    test_firmware_does_not_treat_join_fail_as_setup()
    print("test_wifi_boot: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

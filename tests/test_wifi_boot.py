#!/usr/bin/env python3
"""Host tests for SoftAP / welcome boot policy (wifi_boot.h).

Welcome + SoftAP when there is no home network to use (first-setup, empty
ssid, wipe, or saved SSID not on the air). A saved SSID that is seen or
was associated, but misses DHCP/auth in the boot window, stays STA.
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
    if (!sb_wifi_should_start_portal("", false, false)) {
        return fail("blank cfg should start portal");
    }
    if (!sb_wifi_should_play_welcome("", false, false)) {
        return fail("blank cfg should play welcome");
    }

    /* QA: saved SSID seen, DHCP/auth miss — stay STA, no welcome. */
    if (sb_wifi_is_first_setup("GOOSE!", "", "", "URL2")) {
        return fail("saved ssid is not first-setup");
    }
    if (sb_wifi_should_start_portal("GOOSE!", false, true)) {
        return fail("on-air join-fail must not start SoftAP");
    }
    if (sb_wifi_should_play_welcome("GOOSE!", false, true)) {
        return fail("on-air join-fail must not play welcome");
    }

    /* Saved SSID not on the air — SoftAP + welcome so the user can pick. */
    if (!sb_wifi_should_start_portal("GOOSE!", false, false)) {
        return fail("ssid not on air should start portal");
    }
    if (!sb_wifi_should_play_welcome("GOOSE!", false, false)) {
        return fail("ssid not on air should play welcome");
    }

    /* Empty ssid on a named box: same class as a scan miss. */
    if (sb_wifi_is_first_setup("", "Elwyn - Silver", "Hesston", "URL2")) {
        return fail("named empty-ssid is not first-setup");
    }
    if (!sb_wifi_should_start_portal("", false, false)) {
        return fail("named empty-ssid should start portal");
    }
    if (!sb_wifi_should_play_welcome("", false, false)) {
        return fail("named empty-ssid should play welcome");
    }

    /* Explicit wipe: SoftAP + welcome (pick a new AP). */
    if (!sb_wifi_should_start_portal("", true, false)) {
        return fail("force_ap should start portal");
    }
    if (!sb_wifi_should_play_welcome("", true, false)) {
        return fail("force_ap should play welcome");
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


def test_firmware_join_fail_vs_ssid_missing() -> None:
    main = MAIN_C.read_text()
    portal = PORTAL_C.read_text()
    assert "wifi_connect_or_setup(&play_welcome)" in main
    assert "SB_WIFI_NEED_SETUP" in main
    assert "SB_WIFI_NO_IP" in main
    assert "quiet STA retry, no welcome" in main
    assert "saved ssid not on air -> setup AP" in main
    assert "wifi_saved_network_on_air" in main
    assert "scan saw saved ssid=" in main
    assert "captive_portal_run(play_welcome)" in main
    assert "if (!wifi_connect_or_setup())" not in main
    assert "clip_player_loop(SB_CLIP_AP_WELCOME)" not in main
    assert "if (play_welcome)" in portal
    assert "SSID empty but configured" not in main


def main() -> int:
    test_policy_matrix()
    test_firmware_join_fail_vs_ssid_missing()
    print("test_wifi_boot: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

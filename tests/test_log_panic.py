#!/usr/bin/env python3
"""Host tests for panic snapshot text that must survive reboot.

Nathan 0.5.25 (d0:cf:13:07:ca:c0) rebooted reset=PANIC around 19:20 UTC
26 Sep 2026. The 8 KB PSRAM ring kept only the last seconds; no boot
line, Guru, or Backtrace was posted. The next panic has to keep reason
+ backtrace in RTC and ship that chunk first after reboot.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "firmware" / "main" / "log_panic.h"
SHIPPER = ROOT / "firmware" / "main" / "log_shipper.c"
CMAKE = ROOT / "firmware" / "CMakeLists.txt"
MAIN_CMAKE = ROOT / "firmware" / "main" / "CMakeLists.txt"

HARNESS = r"""
#include <stdio.h>
#include <string.h>
#include "log_panic.h"

static int fail(const char *msg)
{
    fprintf(stderr, "fail: %s\n", msg);
    return 1;
}

int main(void)
{
    char buf[1536];
    log_panic_dump_t dump;
    memset(&dump, 0, sizeof(dump));

    size_t n = log_panic_format(buf, sizeof(buf), 1200, "0.5.25", "PANIC",
                                1, 4186, &dump);
    if (n == 0) {
        return fail("crash boot line empty");
    }
    if (!strstr(buf, "boot fw=0.5.25 reset=PANIC crashes=1 seq=4186")) {
        return fail("crash boot line missing");
    }
    if (strstr(buf, "Backtrace:")) {
        return fail("invalid dump should not print Backtrace");
    }
    if (strstr(buf, "panic reason=")) {
        return fail("invalid dump should not print panic reason");
    }

    dump.magic = LOG_PANIC_MAGIC;
    dump.pc = 0x40376abc;
    dump.core = 0;
    dump.nframes = 3;
    dump.frames[0] = 0x40376abc;
    dump.frames[1] = 0x42001234;
    dump.frames[2] = 0x42005678;
    dump.sps[0] = 0x3fc89010;
    dump.sps[1] = 0x3fc89040;
    dump.sps[2] = 0x3fc89070;
    strncpy(dump.reason, "LoadProhibited", sizeof(dump.reason) - 1);

    n = log_panic_format(buf, sizeof(buf), 612, "0.5.25", "PANIC",
                         1, 4186, &dump);
    if (n == 0 || n >= sizeof(buf)) {
        return fail("panic dump format failed");
    }
    if (!strstr(buf, "boot fw=0.5.25 reset=PANIC")) {
        return fail("dump chunk missing boot line");
    }
    if (!strstr(buf, "panic reason=LoadProhibited core=0 pc=0x40376abc")) {
        return fail("dump chunk missing reason/pc");
    }
    if (!strstr(buf, "Backtrace: 0x40376abc:0x3fc89010 0x42001234:0x3fc89040 0x42005678:0x3fc89070")) {
        return fail("dump chunk missing backtrace");
    }
    if (buf[n - 1] != '\n') {
        return fail("dump chunk must end in newline");
    }

    dump.nframes = LOG_PANIC_FRAMES;
    for (int i = 0; i < LOG_PANIC_FRAMES; i++) {
        dump.frames[i] = 0x42000000u + (unsigned)i * 4u;
        dump.sps[i] = 0x3fc88000u + (unsigned)i * 16u;
    }
    n = log_panic_format(buf, 1536, 1, "0.5.29", "TASK_WDT", 2, 9, &dump);
    if (n == 0 || n >= 1536) {
        return fail("16-frame dump overflowed POST chunk");
    }
    if (!strstr(buf, "0x4200003c:0x3fc880f0")) {
        return fail("last backtrace frame dropped");
    }

    n = log_panic_format(buf, sizeof(buf), 10, "0.5.29", "BROWNOUT",
                         1, 0, NULL);
    if (!strstr(buf, "reset=BROWNOUT") || strstr(buf, "Backtrace:")) {
        return fail("null dump should be boot line only");
    }

    memset(dump.reason, 'A', sizeof(dump.reason));
    dump.magic = LOG_PANIC_MAGIC;
    dump.nframes = 0;
    n = log_panic_format(buf, sizeof(buf), 1, "0.5.29", "PANIC", 1, 0, &dump);
    if (n == 0 || n >= sizeof(buf)) {
        return fail("unterminated reason overflowed");
    }
    if (!strstr(buf, "panic reason=")) {
        return fail("unterminated reason dropped the panic line");
    }
    return 0;
}
"""


def test_format() -> None:
    with tempfile.TemporaryDirectory() as td:
        tdir = Path(td)
        src = tdir / "harness.c"
        src.write_text(HARNESS)
        bin_path = tdir / "test_log_panic"
        subprocess.check_call(
            [
                "gcc", "-O0", "-Wall", "-Werror",
                "-I", str(HEADER.parent),
                str(src),
                "-o", str(bin_path),
            ]
        )
        subprocess.check_call([str(bin_path)])


def test_panic_chunk_ships_before_spill() -> None:
    shipper = SHIPPER.read_text()
    panic_at = shipper.find("if (take_panic_chunk())")
    spill_at = shipper.find("if (take_spill_chunk())")
    assert panic_at != -1 and spill_at != -1 and panic_at < spill_at
    assert "--wrap=esp_panic_handler" in MAIN_CMAKE.read_text()
    assert "--wrap=esp_panic_handler" not in CMAKE.read_text()


def main() -> int:
    test_format()
    test_panic_chunk_ships_before_spill()
    print("test_log_panic: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

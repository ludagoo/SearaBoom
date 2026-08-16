# Potential improvements

- Confirm exact amp wiring / button polarity on the production PCB and bake into `board_pins_config.c`.
- Capacitve touch (original T6/T7) in addition to GPIO buttons.
- Stream is mono 22050 Hz from brasilstream; consider server-side resample/upmix or higher default volume.
- Direct ADTS decode without stream proxy (ADF treats `Content-Type: audio/aac` as RAW+ASC).
- Persist last volume across reboots (partially done) and restore station selection in the captive UI select.
- Proper URL-decode for WiFi passwords with special characters in captive portal POST.
- Signed OTA (ECDSA) + admin auth beyond shared token.
- Offline tone / “updating…” voice prompt during OTA.
- Station list served from the management server instead of hard-coded URLs.
- Watchdog + automatic stream reconnect with jittered backoff (basic backoff exists).
- PSRAM enable on boards that have it for larger HTTP ringbuffers.
- Factory reset: hold both volume buttons 5s → wipe NVS → captive portal.

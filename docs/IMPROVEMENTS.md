# Potential improvements

- Factory reset: hold both volume pads ~5s → wipe NVS → captive portal.
- Signed OTA (ECDSA) + admin auth beyond shared token.
- Station list served from the management server instead of hard-coded URLs.
- Direct ADTS decode without stream proxy (ADF treats `Content-Type: audio/aac` as RAW+ASC).
- Batch remote logs onto boot/serial OTA TLS windows to reduce concurrent HTTPS pressure.
- Confirm production PCB amp wiring / pad layout and bake into `board_pins_config.c`.

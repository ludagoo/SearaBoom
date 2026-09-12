# Potential improvements

- Factory reset: hold both volume pads ~5s → wipe NVS → captive portal.
- Signed OTA (ECDSA) + admin auth beyond shared token.
  **Done for OTA (IDF RSA-3072 / Secure Boot V2 signature, no eFuse).** See
  [`docs/SIGNED_FIRMWARE.md`](SIGNED_FIRMWARE.md). Hardware Secure Boot + admin
  auth beyond the shared token still open.
- Station list served from the management server instead of hard-coded URLs.
- Drop the unused live-AAC proxy if boxes stay on direct Brasilstream. Firmware
  now opens `8396`/`8404.brasilstream.com.br/stream` (ADTS, `Icy-MetaData: 0`,
  ADF codec forced UNKNOWN). `GET /stream/102` and `/stream/104` in
  `server/app.py` (`STREAMS`, `stream_proxy`) plus any tunnel/nginx location
  for `/stream/` can go. Re-add a proxy only if ADF regresses on
  `Content-Type: audio/aac`.
- Batch remote logs onto boot/serial OTA TLS windows to reduce concurrent HTTPS pressure.
- Confirm production PCB amp wiring / pad layout and bake into `board_pins_config.c`.

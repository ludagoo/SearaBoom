## Path

- [ ] Targets `main`
- [ ] Did **not** publish OTA / USB factory
- [ ] Did **not** flash dedicated QA boxes (`/dev/searaboom-qa-*`)

Firmware/device change: push this PR and wait for **USB box (s3-zero)** and **USB box (s3-supermini)**.

- Rerun quiet QA: comment `/hw-test`
- Soak: `/hw-test soak`
- Loud / mic: `/hw-test listen` (only when Lucas says the room is OK)
- Pads / human steps: `/hw-test hands`

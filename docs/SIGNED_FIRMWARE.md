# Signed firmware (OTA + recovery)

**No signed USB flash and no OTA/factory publish until Lucas says the key is stored.**

Build/compile may use `~/.config/searaboom/secure_boot_signing_key.pem` if it
already exists. A build does **not** mint that key. That is not permission to
flash or publish.

After the key is copied offline, **Lucas** (not an agent) creates this marker:

```bash
touch ~/.config/searaboom/signing_key_backed_up
```

Until that file exists as a regular file:

- `scripts/dev_ota.sh`, `publish_firmware.sh`, and `snapshot_factory.sh` exit
- `scripts/hw_flash.sh` and `idf.py flash` (signed images) fail
- factory USB publish of signed images fails
- agents must never create the marker (no `--force`; policy forbids env tricks)

See the gate in `scripts/signing_key_backup.py`.

ESP32-S3 + ESP-IDF 5.3.2. This tree uses Espressif **signed app verification
without hardware Secure Boot**: `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`.
Boxes reject OTA images that are unsigned or signed with the wrong RSA-3072
key. USB download stays open so a bricked unit can be recovered.

Hardware Secure Boot V2 (eFuse digest) is **not** enabled in
`sdkconfig.defaults`. Burning those fuses is irreversible and would lock QA
boxes to whatever key signed that build.

## What this PR turns on

| Layer | What happens | eFuses |
|-------|----------------|--------|
| OTA (`esp_https_ota`) | RSA-PSS check against the key in the **running** app | none |
| App boot | Aborts if this image has no signature (so the next OTA can be checked) | none |
| USB web flash / `esptool` | Still writes any image (ROM is unlocked) | none |
| Hardware Secure Boot V2 | Opt-in later via `firmware/sdkconfig.defaults.secureboot` | `SECURE_BOOT_EN` + key digest |

Same signature format as Secure Boot V2 (4 KB sector, RSA-3072, PSS salt 32).
IDF documents this as [Signed App Verification Without Hardware Secure Boot](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/security/secure-boot-v2.html#signed-app-verification-without-hardware-secure-boot).

## Keys (Lucas)

The private key is **not** in git.

1. Preferred path: `~/.config/searaboom/secure_boot_signing_key.pem`
2. `SEARABOOM_SIGNING_KEY=/path/to/key.pem` is only for pointing the **firmware
   symlink** at an existing file. If the config private key already exists and
   differs from that path, `ensure_signing_key.sh` **exits**. It will not copy
   the env file (or a stray `firmware/*.pem`) over the config key.
3. Firmware build expects `firmware/secure_boot_signing_key.pem` (gitignored
   symlink/copy). `hw_flash.sh` and `firmware/CMakeLists.txt` call
   `scripts/ensure_signing_key.sh` **without** `--generate` so a fresh worktree
   gets the symlink. That does **not** mint a key; missing config key → exit 1.

`scripts/ensure_signing_key.sh --generate` mints an RSA-3072 key in
`~/.config/searaboom/` **once**, when you intend to keep it. USB QA and local
`idf.py build` then consume the firmware symlink. **Do not flash or publish**
that image until Lucas has stored the key and created
`~/.config/searaboom/signing_key_backed_up`. Agents must never create that
marker.

**Before the first signed flash or release from `main`:**

```bash
# Link firmware/secure_boot_signing_key.pem in this checkout (no mint).
# Needed on a fresh clone/worktree; the pem is gitignored.
./scripts/ensure_signing_key.sh

# Either keep the generated key and back it up offline:
cp ~/.config/searaboom/secure_boot_signing_key.pem /offline/searaboom-ota-key.pem

# Or replace it with a key made on an air-gapped machine (Lucas only, after
# backing up whatever is already at the config path):
espsecure.py generate_signing_key --version 2 --scheme rsa3072 /offline/searaboom-ota-key.pem
cp /offline/searaboom-ota-key.pem ~/.config/searaboom/secure_boot_signing_key.pem
./scripts/ensure_signing_key.sh

# Only Lucas, after the copy is stored:
touch ~/.config/searaboom/signing_key_backed_up
```

### Server pin (not optional on this host)

`ensure_signing_key.sh` writes `~/.config/searaboom/ota_signing_pubkey.pem`
whenever the config **private** key exists. That path is the server default
(`SEARABOOM_OTA_PUBKEY`, else `PUBKEY_DEFAULT`). After the next server restart,
uploads must match that pin.

This pin does **not** protect against a clobbered private key: if the private
key were replaced and the pubkey regenerated from it, the pin would follow.
That is why `ensure_signing_key.sh` must never overwrite the config private key.

To copy the pin offline (runnable; two arguments):

```bash
cp ~/.config/searaboom/ota_signing_pubkey.pem /offline/searaboom-ota-pubkey.pem
# optional override after restart:
# export SEARABOOM_OTA_PUBKEY=/offline/searaboom-ota-pubkey.pem
```

`./scripts/publish_firmware.sh` and `./scripts/snapshot_factory.sh` both run
`fw_signature.py --check` on the app image and refuse unsigned copies.
Do not publish from a PR. Do not bump `firmware/VERSION` except at release.

## Unbrick (today — signed OTA only)

Download mode is not disabled. Recovery is the same USB factory path:

1. Put the box in USB boot (USB-Serial-JTAG on these S3 modules).
2. Flash the **signed** factory image from https://searaboom.goossen.dev/ (Chrome/Edge).
3. Or `idf.py -p /dev/ttyACM0 flash` of a locally signed build on a **non-QA** box.

Unsigned images built **without** this config (current field firmware) still boot.
An image built **with** `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT` but
**no signature block** hits `check_signature_on_update_check()` → `abort()` and
boot-loops. USB can still overwrite flash. After a signed image is on a box,
**OTA** of unsigned or foreign-key images fails (`ESP_ERR_OTA_VALIDATE_FAILED`).

## Key rotation (not supported)

IDF trusts **block 0** of the running app only ("Only the first position of
signature blocks is used"). `server/fw_signature.py` also parses block 0 only.
A transitional dual-signed image would need the **old** key in block 0 for the
box to accept it, and a pinned server would still reject a new-key-only image.
Do not rotate the OTA key without a dedicated dual-signed step; this tree does
not implement that.

## Unbrick after hardware Secure Boot (later, production only)

Do this only on a throwaway or production box you intend to lock, **never** on
`/dev/searaboom-qa-*`.

1. Production signing key in place (same key that signed the images).
2. Merge `firmware/sdkconfig.defaults.secureboot` into the build config
   (`CONFIG_SECURE_BOOT=y`, UART download **left enabled** via
   `CONFIG_SECURE_INSECURE_ALLOW_DL_MODE`).
3. Rebuild. Sign bootloader + app (IDF does this when
   `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=y`).
4. USB-flash the signed bootloader, partition table, otadata, app, storage.
5. First boot burns `SECURE_BOOT_EN` and the key digest. Power must not drop.
   The bootloader also **revokes unused digest slots** unless
   `SECURE_BOOT_ALLOW_UNUSED_DIGEST_SLOTS` (under `SECURE_BOOT_INSECURE`) is set.
   After that there is **no key rotation on that unit**.
6. Confirm with `espefuse.py --port PORT summary` (`SECURE_BOOT_EN`, digest
   slots). Leave `DIS_DOWNLOAD_MODE` and `ENABLE_SECURITY_DOWNLOAD` **unburned**.

Recovery then: USB web flash or esptool of a **signed** factory image. Unsigned
images will not boot. If you burn `ENABLE_SECURITY_DOWNLOAD` or
`DIS_DOWNLOAD_MODE`, esptool / the public flash page can no longer recover the
unit.

Host workflow (if you prefer to burn fuses from the host instead of first boot):
[Enable Secure Boot V2 Externally](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/security/host-based-security-workflows.html#enable-secure-boot-v2-externally).

Flash encryption is **not** enabled: it needs a separate key escrow and makes
USB recovery harder. Add it only after signed OTA + Secure Boot are proven.

## Why not Secure Download Mode / UART lock now

ESP32-S3 can permanently switch UART ROM download to Secure Download Mode
(`CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`) or disable it
(`CONFIG_SECURE_DISABLE_ROM_DL_MODE`). Both fight the unbrick requirement.
This tree keeps full download mode so Lucas can always push a signed factory
image over USB.

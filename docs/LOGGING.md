# Remote logging plan

Contract for firmware + server. Implement against this. Do not add a log viewer to admin.html.

## Goals

- Do not drop boot, captive-portal, or OTA-path logs.
- Persist logs to SPIFFS while the box has no internet (setup AP, pre-join).
- Ship a heartbeat envelope (fw, reset, heap, rssi, station, seq) plus log text.
- Server records public IP + geo per device (for a later map / stats page).
- Logs survive server restart (JSONL on disk).
- Agent-friendly HTTP APIs only. Keep the existing SSE stream for `watch_logs.sh`.
- Old boxes (`{device_id, ts_ms, text}` only) must still ingest.

## Out of scope

- Admin HTML log UI (leave the existing live tail; do not polish it).
- Gzip, syslog, SQLite, Grafana.
- Publishing OTA / bumping `firmware/VERSION` / touching USB factory.
- Wiping NVS or running `tests/e2e_setup.py` (that erases Wi‑Fi).

---

## POST /api/logs body (firmware → server)

```json
{
  "device_id": "aa:bb:cc:dd:ee:ff",
  "fw": "0.3.0",
  "reset": "POWERON",
  "uptime_ms": 123456,
  "heap": 140000,
  "rssi": -62,
  "station": "URL1",
  "name": "Maria",
  "city": "Nova Russas",
  "seq": 17,
  "ts_ms": 123456,
  "text": "I (12345) radio: go live\\nW (12400) wifi: ..."
}
```

| Field | Required | Notes |
|-------|----------|--------|
| device_id | yes | STA MAC, lowercase, colon-separated |
| text | yes if shipping logs | Newline-separated ESP_LOG lines. Empty text with heartbeat fields is OK (heartbeat-only). |
| fw | no | `esp_app_get_description()->version` |
| reset | no | `esp_reset_reason()` as short token: POWERON, EXT, SW, PANIC, INT_WDT, TASK_WDT, WDT, DEEPSLEEP, BROWNOUT, SDIO, USB, JTAG, EFUSE, PWR_GLITCH, CPU_LOCKUP, UNKNOWN |
| uptime_ms | no | `esp_timer_get_time()/1000` |
| heap | no | `esp_get_free_heap_size()` |
| rssi | no | STA RSSI dBm, or 0 if unknown |
| station | no | `URL1` / `URL2` / empty |
| name | no | Owner name from captive portal (Nome). Prefer this over geo for identity. |
| city | no | Owner city from captive portal (Cidade). Prefer this over IP geo.city for the map. |
| seq | no | Monotonic uint, increment every successful POST. Server uses it to flag gaps. |
| ts_ms | no | Device uptime ms at POST (not wall clock) |

Server **ignores** any client-supplied IP/geo. Those come from the HTTP request.

Backward compatible: missing new keys is fine.

---

## Server ingest

1. Parse JSON. `device_id` default `unknown`, lowercased.
2. Client IP: first of `CF-Connecting-IP`, first hop in `X-Forwarded-For`, `X-Real-IP`, `request.remote_addr`. Strip ports. Ignore private/loopback for geo (still store as `ip` if that is all you have).
3. Split `text` on newlines. Skip empty lines.
4. Parse ESP-IDF `I (12345) TAG: msg` (also W/E/D/V) into `level`, `tag`, `device_ms`, `msg`. If no match, `level=null`, `msg=line`.
5. Each stored line:

```json
{
  "device_id": "...",
  "line": "<original>",
  "level": "I",
  "tag": "radio",
  "device_ms": 12345,
  "msg": "go live",
  "seq": 17,
  "ts": 1690000000.1,
  "server_ts": "2026-08-21T12:00:00+00:00"
}
```

6. Update device registry (see below) with heartbeat fields + ip. If public IP changed (or never geolocated), look up geo asynchronously so ingest stays fast. Cache geo by IP.
7. Append lines to in-memory deque (maxlen 2000) **and** `server/logs/<device_id_safe>.jsonl`.
8. Publish each line to SSE subscribers (existing `/api/logs/stream`).
9. If `seq` jumps by >1 vs last seq for that device, insert a synthetic line: `log_gap prev=N got=M`.
10. Return `{"ok": true, "accepted": <n>, "device_id": "..."}`.

Geo lookup: `http://ip-api.com/json/{ip}?fields=status,country,countryCode,region,regionName,city,lat,lon,query` (timeout 3s). On success store country, country_code, region, city, lat, lon. Rate-limit: at most one lookup per IP per 24h; never block the POST. If lookup fails, keep previous geo.

Device id filename: replace `:` with `-` (`aa-bb-cc-dd-ee-ff.jsonl`).

On process start, load existing JSONL tails into the deques (last 2000 lines each) and `devices.json`.

---

## Device registry `server/logs/devices.json`

```json
{
  "aa:bb:cc:dd:ee:ff": {
    "device_id": "aa:bb:cc:dd:ee:ff",
    "first_seen": "2026-08-21T12:00:00+00:00",
    "last_seen": "2026-08-21T12:05:00+00:00",
    "fw": "0.3.0",
    "reset": "POWERON",
    "uptime_ms": 123456,
    "heap": 140000,
    "rssi": -62,
    "station": "URL1",
    "name": "Maria",
    "city": "Nova Russas",
    "seq": 17,
    "ip": "203.0.113.10",
    "geo": {
      "country": "Brazil",
      "country_code": "BR",
      "region": "Parana",
      "city": "Curitiba",
      "lat": -25.43,
      "lon": -49.27
    },
    "lines": 420
  }
}
```

Rewrite this file on ingest (debounce 1s if you want). `lines` is total accepted ever or current jsonl count — use jsonl line count / deque length, whichever is easy; expose both `lines` (in-memory) and leave it stable for `/api/status`.

---

## HTTP APIs

Keep existing routes working. Add/extend:

### `GET /api/devices`

```json
{
  "ok": true,
  "devices": [ { "<registry object + online: bool>" } ]
}
```

`online` = `last_seen` within 120s.

### `GET /api/devices/<device_id>`

Registry object plus `"recent": [ last 50 log line objects ]`.

### `GET /api/logs`

Existing. Extra query args:

- `device` — filter (already)
- `limit` — already, max 2000 now
- `level` — optional `E`/`W`/`I`
- `since` — unix ts, exclusive

Response `{ok, lines}` with the richer line objects.

### `GET /api/logs/stream`

Unchanged SSE of line objects (now richer). `watch_logs.sh` still works (`line` + `device_id`).

### `GET /api/status`

Keep firmware/factory. `devices` entries must still have `device_id`, `last_seen`, `lines`, and should also include `fw`, `ip`, `geo`, `online`, `rssi`, `station`.

Do **not** require admin token on ingest (old boxes). Read APIs stay open too (local tunnel, agent use).

---

## Firmware

### Collect vs ship

- Install `esp_log_set_vprintf` at the **start** of `app_main` (after log level sets, before Wi‑Fi).
- Pause means **do not POST**. Keep writing the RAM ring. Do not skip the ring when `s_paused`.
- Pause only around OTA **firmware download**, not the version-check GET. After the check GET, call `log_shipper_flush()`.
- Do not dequeue a chunk until POST returns 2xx. On failure, keep the chunk (put back / cursor) and back off (existing 45s is fine).
- Line buffer: 256 bytes (up from 160). Mutex: wait a few ms, not 0.

### RAM ring + flash spill

- RAM ring stays ~8 KB.
- SPIFFS file `/spiffs/logq` (or `logq.0`/`logq.1` rotate), **cap ~48 KB**.
- Mount the `storage` SPIFFS partition at shipper init (boot). Captive portal already mounts it later — if already mounted, skip/re-use; do not double-register; do not format on shipper init (portal still has `format_if_mount_failed`).
- When the RAM ring would drop lines **and** we are not shipping (no Wi‑Fi / paused): append overflow to SPIFFS. Also periodically (~10s) spill if ring > half and `!wifi_sta_got_ip()`.
- Do **not** SPIFFS-write while the radio HTTP/AAC path is live unless the ring is about to wrap; prefer RAM. Setup AP / pre-Wi‑Fi is the main flash-writer.
- On Wi‑Fi up: drain SPIFFS oldest-first in `LOG_POST_MAX` chunks, then RAM. Unlink/truncate when drained.
- Never ship until `wifi_sta_got_ip()`. Keep the 25s post-connect settle before the **first** TLS POST (audio). Subsequent flushes: 20–30s, or immediately if ring high / an E or W line arrived / `log_shipper_flush()` called.

### Ship triggers

1. 25s after first `wifi_sta_got_ip` (settle).
2. After boot OTA check returns (piggyback TLS window) via `log_shipper_flush()`.
3. Periodic 20–30s while online.
4. Ring nearly full.
5. ERROR/WARN line (coalesce: flush soon, not once per line).

Each POST still opens HTTPS (no keepalive required). Same URL `CONFIG_SEARABOOM_LOG_URL/api/logs`.

### Envelope fill

- `rssi`: `esp_wifi_sta_get_ap_info` if STA up, else omit/0.
- `station`: from config_store if cheap; otherwise omit rather than coupling tightly. A weak read of current url_key is fine (include config_store.h).
- First boot line after hook: `log_shipper boot fw=... reset=...`.
- Crash reset: that boot line plus `panic reason=` / `Backtrace:` is a sticky first POST from RTC (console-only at init, not also in the RAM ring). Hold until 2xx. Power-on boots still go through the ring as before.

### API additions (`log_shipper.h`)

```c
esp_err_t log_shipper_init(void);          /* hook vprintf; mount SPIFFS; start task */
void log_shipper_set_paused(bool paused);  /* POST gate only */
bool log_shipper_is_paused(void);
void log_shipper_wifi_up(void);            /* start settle timer / allow ship */
void log_shipper_flush(void);              /* request soon flush (OTA check done) */
```

Call `log_shipper_init` early in `app_main`. Call `log_shipper_wifi_up` when STA gets IP (existing WIFI_OK path). Keep the current late `log_shipper_init()` site as a no-op second call (init must be idempotent) **or** remove the late call — prefer one call at the top + `wifi_up` later.

Serial: add `logstat` printing ring bytes, flash bytes, paused, ready, seq, last http status.

### Constraints

- Do not raise log levels. DEBUG still starves I2S.
- Do not grow the shipper task stack much past 12 KB if possible; 16 KB max.
- Do not call `dev_ota.sh` / `publish_firmware.sh` / `snapshot_factory.sh`.
- Local test flash: `idf.py -p /dev/ttyACM0 flash` after a successful build. That is the plugged-in box. Do **not** erase NVS.

---

## Files

Firmware (owner: firmware agent):

- `firmware/main/log_shipper.c` / `.h` (main work)
- `firmware/main/main.c` (init timing, `wifi_up`)
- `firmware/main/ota_update.c` (pause only around download; flush after check)
- `firmware/main/captive_portal.c` (SPIFFS mount share)
- `firmware/main/serial_cmd.c` (`logstat`)
- `firmware/main/CMakeLists.txt` only if a new .c is added

Server (owner: server agent):

- `server/app.py`
- `server/logs/` (runtime; gitignore jsonl + devices.json)
- `.gitignore` (`server/logs/`)
- `scripts/watch_logs.sh` (print `server_ts` + `level` if present)

Do not fight over `firmware/VERSION`, `radio_player.c`, factory scripts, or `admin.html`.

---

## Test

Server (no box required):

- POST old-shape JSON → 200, line stored.
- POST new envelope → device registry has fw/seq; JSONL file created; GET `/api/devices` shows it.
- Fake `CF-Connecting-IP` of a public IP → geo fields eventually present (allow a second GET after lookup).
- Restart simulation: load JSONL at startup (or call the load function / restart systemd) → GET `/api/logs?device=` still returns lines.

Firmware (plugged box `/dev/ttyACM0`):

- `idf.py build` then `idf.py -p /dev/ttyACM0 flash`.
- Serial: `log_shipper boot`, `log_shipper online` or `Remote logs ->`, then logs after ~25s settle.
- `echo logstat > /dev/ttyACM0` (or existing serial_cmd path) shows ring/flash/seq.
- GET `https://searaboom.goossen.dev/api/devices` contains this MAC with `fw`, `ip`, and hopefully `geo`.
- GET `/api/logs?device=<mac>&limit=50` returns boot/wifi/radio lines, not only post-init.
- Confirm audio still starts (CLIP / radio logs). If I2S starved, you over-logged or SPIFFS-wrote during stream — fix.

Do not run the captive-portal e2e (wipes Wi‑Fi). Flash spill can be checked via `logstat` flash bytes during boot before Wi‑Fi, or by reading serial for a spill log line.

# /ovintes usage roster

Internal dashboard for us. Map + loyalty stay. Add sessions, on-air vs silent, station split, health, USER_FLAG. This page is ours — MAC, RSSI, fw, flags are allowed (still omit IP).

Timezone for “today” and typical hour: **America/Fortaleza**.

---

## Server (owner: server agent) — `server/app.py` only

### Sessionize in `update_registry`

Use firmware `listen_s` **delta** (the amount just added to `listen_s_server`) as honest audio seconds.

State on each device record:

```
session_open: bool
session_start: unix float
session_listen_at_start: int   # listen_s_server when session opened
last_session_s: int
sessions_today: int
sessions_total: int
listen_today_s: int
today: "YYYY-MM-DD"            # Fortaleza calendar date
listen_by_station: {"URL1": int, "URL2": int}
hour_hist: [24 ints]           # session-start counts by Fortaleza hour
last_flag_at: iso
last_flag_seq: int
flag_count: int
```

Rules:

1. Roll `today`: if Fortaleza date changed, zero `listen_today_s` and `sessions_today`. Keep lifetime totals.
2. After applying listen_s / playing:
   - `online` = last_seen within 120s
   - `playing_now` = playing true and last_seen within 180s
3. **Open session** when `playing_now` becomes true (`session_open` was false): bump sessions_today/total, set start, increment `hour_hist[hour]`.
4. **While open and delta > 0**: add delta to `listen_today_s` and `listen_by_station[station]`.
5. **Close session** when `playing` is explicitly false, **or** at read time if `session_open` and last_seen older than 180s. `last_session_s = max(delta listen_s during session, wall clock)`. Prefer listen_s delta.
6. Parse every ingested log line for `USER_FLAG seq=N` (token exact). Update last_flag_*. On process start, scan existing jsonl tails (last 2000 lines/device) once to backfill flags.

Helper `close_stale_sessions(now)` called from `/api/ouvintes` (and devices list).

### `GET /api/ouvintes`

Include **every known device** (not only those with listen_s). Omit IP.

```json
{
  "ok": true,
  "listening": 1,
  "online": 2,
  "silent": 1,
  "stations": {
    "102.7": {"listen_s": 12000, "playing": 1},
    "104.7": {"listen_s": 4000, "playing": 0}
  },
  "listeners": [
    {
      "id": "765d757b",
      "device_id": "d0:cf:13:07:d1:c4",
      "name": "Lucas USB",
      "city": "Nova Russas",
      "station": "102.7",
      "listen_s": 200,
      "listen_today_s": 200,
      "last_session_s": 180,
      "session_s": 200,
      "sessions_today": 1,
      "sessions_total": 1,
      "playing": true,
      "online": true,
      "status": "playing",
      "rssi": -30,
      "fw": "0.3.0",
      "lat": -4.71,
      "lon": -40.55,
      "last_seen": "2026-08-21T16:35:00+00:00",
      "last_flag_at": null,
      "last_flag_seq": null,
      "flag_count": 0,
      "typical_hour": 7
    }
  ]
}
```

`status`: `playing` | `silent` (online, not playing) | `offline`.

Sort listeners: playing first, then silent, then offline, then `-listen_s`.

`typical_hour`: argmax of hour_hist, or null if empty.

Keep existing fields so the current page does not go blank mid-deploy.

Restart systemd after. Tests:

- POST playing true + listen_s growing twice → session opens, listen_today_s grows.
- POST playing false → last_session_s set, status silent/offline.
- POST text `W (1) volume_buttons: USER_FLAG seq=1 uptime_ms=9` → last_flag_seq=1.
- USB box `d0:cf:13:07:d1:c4` still present.
- No IP in JSON.

Do not edit ovintes.html (page agent owns it). Do not wipe devices.json.

---

## Page (owner: page agent) — `server/static/ovintes.html` (+ tiny CSS if you split)

Rebuild `/ovintes` as an **ops roster**. Portuguese. Palette `#052C31` `#007985` `#EC9F17` `#009035`. This is not a marketing page.

### Layout (desktop, min 1100px)

```
┌ header: title · live chips ─────────────────────────────────┐
│  Ouvintes          [● 1 no ar] [○ 1 quieto] [  4 off]      │
│  102.7  3h 12min            104.7  0 min                   │
├─────────────────────────────┬───────────────────────────────┤
│ MAP  (~62%)                 │ MAIS FIÉIS (~38%)            │
│ gold pulse = playing        │ ranked by listen_s            │
│ teal = silent online        │ name, city, time, status dot  │
│ no offline pins             │ click → pan map               │
├─────────────────────────────┴───────────────────────────────┤
│ ROSTER (full width)                                         │
│ table: status · nome · cidade · rádio · total · hoje ·     │
│ última · sessões · rssi · fw · flag                         │
└─────────────────────────────────────────────────────────────┘
```

Mobile (`max-width: 800px`): header chips wrap, map 42vh, leaderboard, then roster as **cards** (not a squeezed table).

### Status

| status | meaning | color |
|--------|---------|--------|
| playing | PCM flowing recently | gold `#EC9F17` pulse |
| silent | heartbeat < 120s, not playing | muted cyan |
| offline | else | dim grey |

Header chips use those colors. “quieto” = silent (online, no audio). That state is the point of the page.

### Roster columns

- **Nome**: `name` or `Ouvinte`; always show short mac (`d1:c4` last two octets) as muted subtitle. Full `device_id` in `title` tooltip.
- **Cidade**
- **Rádio**: 102.7 / 104.7
- **Total / Hoje / Última**: `Xh Ymin` (reuse a single formatter)
- **Sessões**: `sessions_today` today, muted `sessions_total` lifetime
- **RSSI**: `−30 dBm` or —
- **FW**: version or —
- **Flag**: if `last_flag_at` within 15 min, gold “FLAG seq=N · há 2 min”; else muted last flag or —

Click row: highlight, pan map if lat/lon. Selected row left-gold border.

Empty: “Nenhuma caixa ainda.”

### Map

- Playing: gold circleMarker r=10, pulse via CSS if easy else higher opacity
- Silent: `#00B7C8` r=7, opacity 0.7
- Popup: name, city, station, status, total, hoje, rssi, flag
- Fit bounds once on first points, then don’t steal pan every 15s
- OSM attribution stays

### Leaderboard

Keep **Mais fiéis** — top by `listen_s`, show top 12, gold dot if playing. Click pans.

### Poll

`/api/ouvintes` every 10s. Don’t render a broken frame if a field is missing (old payload). `font-variant-numeric: tabular-nums` on times.

### Verify

curl HTML for roster markup, chips, @media, Leaflet. curl API after server agent lands (`status` field). No browser MCP — do not claim you clicked. If API lacks new fields yet, still ship UI with fallbacks and poll.

Do not edit app.py (server agent owns it).

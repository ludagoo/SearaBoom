# SearaBoom Log Acknowledgment System

Server-side system to mark device logs as read/acknowledged, preventing log sweeps from reprocessing the same events.

## Overview

Device logs are persisted under `server/logs/` as `.jsonl` files. The acknowledgment system tracks which logs have been processed to avoid redundant analysis and issue creation.

## Storage

**File:** `server/logs/log_acks.json`

Contains:
- **Fingerprints:** Hashed signatures of recurring error patterns (device_id + tag + normalized message)
- **Watermarks:** Per-device timestamps marking "processed through" points

## How It Works

### Fingerprint Mode
For recurring error patterns (e.g., "Connection timeout", "Heap low"):
- Computes a hash from: `device_id + tag + normalized_message`
- Normalization removes dynamic content (IPs, timestamps, numbers) so similar errors have the same fingerprint
- Example: Both `"Heap low: 12345 bytes"` and `"Heap low: 67890 bytes"` → same fingerprint

### Watermark Mode
For bulk "processed through" tracking:
- Stores the latest `server_ts` processed for each device
- All logs with `server_ts <= watermark` are considered acknowledged

## API Endpoints

All endpoints require admin token authentication via `X-Admin-Token` header or `?token=` query parameter.

### List Acknowledgments
```bash
GET /api/admin/log-acks

Response:
{
  "ok": true,
  "fingerprints": [
    {
      "device_id": "aa:bb:cc:dd:ee:ff",
      "tag": "wifi",
      "normalized_msg": "Connection timeout after N seconds",
      "fingerprint": "a1b2c3d4e5f6g7h8",
      "acked_at": "2026-09-08T00:00:00+00:00",
      "note": "Fixed in PR #123",
      "pr_url": "https://github.com/org/repo/pull/123"
    }
  ],
  "watermarks": [
    {
      "device_id": "aa:bb:cc:dd:ee:ff",
      "watermark": 1725753600.0,
      "watermark_iso": "2026-09-07T12:00:00+00:00"
    }
  ]
}
```

### Mark Logs as Acknowledged

**Fingerprint Mode** (for specific error patterns):
```bash
POST /api/admin/log-acks/mark
Content-Type: application/json

{
  "device_id": "aa:bb:cc:dd:ee:ff",
  "tag": "wifi",
  "msg": "Connection timeout after 30 seconds",
  "note": "Fixed connection retry logic",
  "pr_url": "https://github.com/org/repo/pull/123"
}
```

**Watermark Mode** (for bulk "processed through"):
```bash
POST /api/admin/log-acks/mark
Content-Type: application/json

{
  "device_id": "aa:bb:cc:dd:ee:ff",
  "watermark": 1725753600.0,
  "note": "Processed historical logs",
  "pr_url": "https://github.com/org/repo/pull/456"
}
```

Fields:
- `device_id` (required): Device MAC address
- `tag` (optional for fingerprint): Log tag (e.g., "wifi", "audio")
- `msg` (required for fingerprint): Log message (will be normalized)
- `watermark` (required for watermark mode): Unix timestamp
- `note` (optional): Human-readable description
- `pr_url` (optional): Link to PR that addressed the issue

### Clear Acknowledgment

**Clear Fingerprint:**
```bash
POST /api/admin/log-acks/clear
Content-Type: application/json

{
  "fingerprint": "a1b2c3d4e5f6g7h8"
}
```

**Clear Watermark:**
```bash
POST /api/admin/log-acks/clear
Content-Type: application/json

{
  "device_id": "aa:bb:cc:dd:ee:ff"
}
```

### Compute Fingerprint
Preview what fingerprint would be generated for a log pattern:

```bash
GET /api/admin/log-acks/fingerprint?device_id=aa:bb:cc:dd:ee:ff&tag=wifi&msg=Connection%20timeout

Response:
{
  "ok": true,
  "fingerprint": "a1b2c3d4e5f6g7h8",
  "device_id": "aa:bb:cc:dd:ee:ff",
  "tag": "wifi",
  "normalized_msg": "Connection timeout",
  "is_acked": false,
  "ack": null
}
```

### Query Unacknowledged Logs

Add `?unacked=1` to the logs endpoint to filter out acknowledged entries:

```bash
GET /api/logs?unacked=1&limit=100

# Also supports existing filters:
GET /api/logs?unacked=1&device=aa:bb:cc:dd:ee:ff&level=E
```

## Workflow for Log Sweeps

1. **Scan for unacknowledged logs:**
   ```bash
   curl "https://searaboom.goossen.dev/api/logs?unacked=1&level=E&limit=1000"
   ```

2. **Identify recurring patterns** (same error type from multiple devices or timestamps)

3. **For each issue filed/fixed:**
   - Create a PR to fix the issue
   - Mark the log pattern as acknowledged:
     ```bash
     curl -X POST -H "X-Admin-Token: $TOKEN" \
       -H "Content-Type: application/json" \
       -d '{"device_id":"*","tag":"heap","msg":"Heap low warning","note":"Optimized memory usage","pr_url":"https://github.com/org/repo/pull/789"}' \
       https://searaboom.goossen.dev/api/admin/log-acks/mark
     ```

4. **Set watermarks** for devices after bulk processing:
   ```bash
   curl -X POST -H "X-Admin-Token: $TOKEN" \
       -H "Content-Type: application/json" \
       -d "{\"device_id\":\"aa:bb:cc:dd:ee:ff\",\"watermark\":$(date +%s),\"note\":\"Batch processing complete\"}" \
       https://searaboom.goossen.dev/api/admin/log-acks/mark
   ```

5. **Skip acknowledged signatures** on subsequent sweeps unless:
   - Volume spikes significantly (many new occurrences)
   - New devices show the pattern
   - Pattern reappears after being dormant

## Demo Script

Run `./demo_log_acks.sh` to see example API calls:

```bash
# Against production:
SEARABOOM_URL=https://searaboom.goossen.dev ./demo_log_acks.sh

# Against local development:
./demo_log_acks.sh
```

## Message Normalization

The system normalizes log messages to identify recurring patterns despite varying dynamic values:

- Numbers → `N` (e.g., `"timeout after 30s"` → `"timeout after Ns"`)
- Hex addresses → `0xH` (e.g., `"at 0xDEADBEEF"` → `"at 0xH"`)
- IPv4 addresses → `IP` (e.g., `"192.168.1.1"` → `IP`)
- Multiple spaces collapsed to single space
- Leading/trailing whitespace removed

Example:
```
Original 1: "Connection to 192.168.1.1:8080 failed after 123 retries"
Original 2: "Connection to 10.0.0.5:9000 failed after 456 retries"
Normalized: "Connection to N.N.N.N:N failed after N retries"
→ Same fingerprint for both
```

## Persistence

- **Auto-save:** Changes are written to `server/logs/log_acks.json` with 1-second debounce (like `devices.json`)
- **Load on startup:** Previous acks are restored from disk
- **Graceful shutdown:** Acks are flushed on `SIGTERM`/`SIGINT` and at exit

## Notes

- Acknowledgments are advisory: they guide sweeps but don't delete logs
- Original logs remain in `.jsonl` files for forensics
- Watermarks and fingerprints work independently (a log can match both)
- The `unacked=1` filter checks both methods: `is_acked_by_fingerprint OR is_acked_by_watermark`

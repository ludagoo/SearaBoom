#!/bin/bash
# API demo script for log acknowledgment system
# Demonstrates how to use the admin endpoints

set -e

TOKEN="${SEARABOOM_ADMIN_TOKEN:-searaboom-dev}"
BASE_URL="${SEARABOOM_URL:-http://localhost:8080}"

echo "========================================"
echo "Log Acknowledgment System API Demo"
echo "========================================"
echo "Token: $TOKEN"
echo "Base URL: $BASE_URL"
echo ""

# List current acks
echo "1. Listing current acknowledgments..."
curl -s -H "X-Admin-Token: $TOKEN" "$BASE_URL/api/admin/log-acks" | python3 -m json.tool
echo ""

# Compute a fingerprint
echo "2. Computing fingerprint for a log pattern..."
DEVICE_ID="aa:bb:cc:dd:ee:ff"
TAG="wifi"
MSG="Connection timeout after 30 seconds"
FP_RESULT=$(curl -s -H "X-Admin-Token: $TOKEN" \
  "$BASE_URL/api/admin/log-acks/fingerprint?device_id=$DEVICE_ID&tag=$TAG&msg=$(python3 -c "import urllib.parse; print(urllib.parse.quote('$MSG'))")")
echo "$FP_RESULT" | python3 -m json.tool
FP=$(echo "$FP_RESULT" | python3 -c "import sys, json; print(json.load(sys.stdin)['fingerprint'])")
echo ""

# Mark a log pattern as acked (fingerprint mode)
echo "3. Marking log pattern as acknowledged..."
curl -s -X POST -H "X-Admin-Token: $TOKEN" -H "Content-Type: application/json" \
  -d "{\"device_id\":\"$DEVICE_ID\",\"tag\":\"$TAG\",\"msg\":\"$MSG\",\"note\":\"Fixed in PR #123\",\"pr_url\":\"https://github.com/org/repo/pull/123\"}" \
  "$BASE_URL/api/admin/log-acks/mark" | python3 -m json.tool
echo ""

# Set a device watermark (timestamp mode)
echo "4. Setting device watermark..."
WATERMARK=$(python3 -c "import time; print(time.time() - 86400)")  # 24 hours ago
curl -s -X POST -H "X-Admin-Token: $TOKEN" -H "Content-Type: application/json" \
  -d "{\"device_id\":\"$DEVICE_ID\",\"watermark\":$WATERMARK,\"note\":\"Processed historical logs up to yesterday\"}" \
  "$BASE_URL/api/admin/log-acks/mark" | python3 -m json.tool
echo ""

# List acks again to see the new ones
echo "5. Listing acknowledgments after marking..."
curl -s -H "X-Admin-Token: $TOKEN" "$BASE_URL/api/admin/log-acks" | python3 -m json.tool
echo ""

# Query logs with unacked filter (if logs exist)
echo "6. Querying unacknowledged logs..."
curl -s "$BASE_URL/api/logs?limit=10&unacked=1" | python3 -m json.tool || echo "(No logs available or server not running)"
echo ""

# Clear a fingerprint ack
echo "7. Clearing fingerprint acknowledgment..."
curl -s -X POST -H "X-Admin-Token: $TOKEN" -H "Content-Type: application/json" \
  -d "{\"fingerprint\":\"$FP\"}" \
  "$BASE_URL/api/admin/log-acks/clear" | python3 -m json.tool || echo "(Already cleared or server not running)"
echo ""

# Clear a watermark
echo "8. Clearing device watermark..."
curl -s -X POST -H "X-Admin-Token: $TOKEN" -H "Content-Type: application/json" \
  -d "{\"device_id\":\"$DEVICE_ID\"}" \
  "$BASE_URL/api/admin/log-acks/clear" | python3 -m json.tool || echo "(Already cleared or server not running)"
echo ""

echo "========================================"
echo "Demo complete!"
echo ""
echo "To use with a running server:"
echo "  SEARABOOM_URL=https://searaboom.goossen.dev ./demo_log_acks.sh"
echo ""
echo "API endpoints:"
echo "  GET  /api/admin/log-acks              - List all acks"
echo "  POST /api/admin/log-acks/mark         - Mark logs as acked"
echo "  POST /api/admin/log-acks/clear        - Clear an ack"
echo "  GET  /api/admin/log-acks/fingerprint  - Compute fingerprint"
echo "  GET  /api/logs?unacked=1              - Query unacked logs"
echo "========================================"

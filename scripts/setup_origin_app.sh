#!/usr/bin/env bash
# Generate Origin App signing keys and print the click-path to register them.
set -euo pipefail
DIR="${SEARABOOM_ORIGIN_APP_DIR:-$HOME/.config/searaboom/origin-app}"
mkdir -p "$DIR"
chmod 700 "$DIR"
if [[ ! -f "$DIR/private.pem" ]]; then
  openssl genpkey -algorithm ED25519 -out "$DIR/private.pem"
  openssl pkey -in "$DIR/private.pem" -pubout -out "$DIR/public.pem"
  chmod 600 "$DIR/private.pem"
  echo "Wrote $DIR/private.pem and public.pem"
else
  echo "Keeping existing $DIR/private.pem"
fi

cat <<EOF

Public key (paste into https://cursor.com/codebase/settings/apps ):

$(cat "$DIR/public.pem")

Create an internal Origin App:
  Display name:  SearaBoom Lab
  Webhook URL:   https://searaboom.goossen.dev/api/lab/origin-webhook
  Signing key:   the PEM above

Install it on goossen/searaboom with:
  repository:contents:read
  repository:pull_requests:read
  repository:checks:write

Then write $DIR/env :

  ORIGIN_APP_ID=app_...
  ORIGIN_INSTALLATION_ID=i_...
  ORIGIN_APP_PRIVATE_KEY=$DIR/private.pem
  ORIGIN_OWNER=goossen
  ORIGIN_REPO=searaboom

And add to ~/.config/systemd/user/searaboom-server.service:

  EnvironmentFile=-$DIR/env

Then: systemctl --user daemon-reload && systemctl --user restart searaboom-server
EOF

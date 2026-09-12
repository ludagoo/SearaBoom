# Hosting SearaBoom (new machine, prod + test)

Portable standup for the Flask OTA/log/factory/QA server and its Cloudflare tunnel. **This PR does not move `searaboom.goossen.dev`.** Mini prod stays on the live user units `searaboom-server.service` and `searaboom-tunnel.service`.

Field firmware has `CONFIG_SEARABOOM_OTA_URL` and `CONFIG_SEARABOOM_LOG_URL` baked to `https://searaboom.goossen.dev`. A new process that is not behind that hostname is invisible to boxes.

## Layout

| Piece | Store (not in git) | Example in repo |
|-------|--------------------|-----------------|
| Instance env | `~/.config/searaboom/prod.env` or `test.env` (mode 600) | `deploy/prod.env.example`, `deploy/test.env.example` |
| Origin App | `~/.config/searaboom/origin-app/{env,private.pem,public.pem}` | `scripts/setup_origin_app.sh` |
| Tunnel config | `~/.config/searaboom/tunnel/{prod,test}.yml` | `deploy/tunnel.yml.example` |
| Tunnel credentials JSON | next to that yml (mode 600) | gitignored `tunnel/*.json` |
| Flask venv | `$SEARABOOM_ROOT/server/.venv` | `server/requirements.txt` |
| OTA + USB factory blobs | `$SEARABOOM_ROOT/server/firmware/` | gitignored bins |
| Device logs / acks | `$SEARABOOM_ROOT/server/logs/` | gitignored |
| systemd | `~/.config/systemd/user/searaboom-{server,tunnel}@.service` | `contrib/systemd/` |

Secret **names** (values never committed):

- `SEARABOOM_ADMIN_TOKEN`
- `SEARABOOM_OUVINTES_SALT` (optional)
- `ORIGIN_APP_ID`, `ORIGIN_INSTALLATION_ID`, `ORIGIN_APP_PRIVATE_KEY` (path), `ORIGIN_OWNER`, `ORIGIN_REPO`
- Tunnel `credentials-file` JSON
- Optional later: `~/.config/searaboom/secure_boot_signing_key.pem` + `ota_signing_pubkey.pem` (signed-OTA PR; live `main` Flask does not load them). Do not create `signing_key_backed_up`.

`SEARABOOM_CONFIG_DIR` is honored by `./scripts/searaboom-host` (check/serve/tunnel). The systemd templates always load `%h/.config/searaboom/%i.env` and `%h/.config/searaboom/origin-app/env` (instance file wins).

## Prod vs test on one machine

| | **prod** (`main`) | **test** (PR worktree) |
|--|-------------------|-------------------------|
| Checkout | long-lived clone on `main` | `git worktree` of the PR branch — **not** the live clone |
| Port | `18080` (Mini today) | another port, **not** `18080` (e.g. `18081`) |
| Bind | `0.0.0.0` | `127.0.0.1` is enough |
| Public URL / tunnel hostname | `searaboom.goossen.dev` only after a planned cut | **must not** be `searaboom.goossen.dev` |
| Tunnel | a **new** named tunnel + new creds JSON | another **new** tunnel; never the live UUID/name `searaboom` and never `Work/SearaBoom/tunnel/*.json` |
| systemd | `searaboom-server@prod` / `searaboom-tunnel@prod` | `…@test` |
| Origin webhook | live URL only | `SEARABOOM_REQUIRE_ORIGIN=0`; do not retarget the Origin App |
| USB QA boxes | stay with Mini until QA moves | do not point `/hw-test` here |

`scripts/searaboom-host check --instance test` **exits 1** if:

- `SEARABOOM_PUBLIC_URL` or tunnel ingress is `searaboom.goossen.dev`
- `tunnel:` / `SEARABOOM_TUNNEL_NAME` is the live name `searaboom` or UUID `e70a4d09-968d-4dac-b0a2-6a39e009da6b`
- `credentials-file` is the live JSON (path under the live clone `tunnel/`, that UUID filename, or JSON `TunnelID`/`TunnelName` matching live) — even with a different ingress hostname. A second connector on the live tunnel can **404** the live host.
- `SEARABOOM_ROOT` is the live clone (systemd `WorkingDirectory` of `searaboom-server.service`)
- `SEARABOOM_PORT` is `18080` or already bound

`install --instance prod` refuses if the Mini live units are **active or enabled** (stopped-but-enabled still starts on next login and would double-bind 18080).

## New machine (prod, not live DNS)

1. Clone `main`. Create `server/.venv` and `pip install -r server/requirements.txt`.
2. Install `cloudflared`. On Arch: `pacman -S cloudflared`. User lingering: `loginctl enable-linger $USER`.
3. `./scripts/setup_host.sh` if this host will USB-flash (udev + `uucp`). IDF 5.3 + ADF are for **builds** and Mini-local `POST /api/factory/flash`, not for serving OTA.
4. `./scripts/setup_origin_app.sh` if this host should take QA webhooks. Keep the Origin App **webhook URL** on Mini until cutover.
5. Create a **new** named tunnel (not `searaboom`) and a **new** hostname. Copy `deploy/tunnel.yml.example`. Force `protocol: http2` (QUIC/7844 is blocked on the Mini network). Do not reuse Mini's `tunnel/*.json`.
6. Copy `deploy/prod.env.example` → `~/.config/searaboom/prod.env`. Set `SEARABOOM_ROOT`, token, **new** tunnel name/paths, Origin names. `chmod 600`.
7. `./scripts/searaboom-host check --instance prod`
8. `./scripts/searaboom-host install --instance prod --enable` then `systemctl --user start searaboom-server@prod searaboom-tunnel@prod`
9. Confirm `curl -fsS http://127.0.0.1:18080/healthz` and the **new** hostname. Leave the live CNAME on Mini.

USB factory for testers is the single-binary flasher from the public flash page (`/api/factory/flasher/`; firmware fetched live from Flask). Chrome WebSerial on that page is a fallback. Mini-local admin flash is `POST /api/factory/flash` and needs a box on `SEARABOOM_SERIAL_PORT`.

## Test instance beside Mini prod

```bash
git -C /home/lucas/Work/SearaBoom worktree add /home/lucas/Work/SearaBoom-wt-pr <pr-branch>
# venv in that worktree
python3 -m venv /home/lucas/Work/SearaBoom-wt-pr/server/.venv
/home/lucas/Work/SearaBoom-wt-pr/server/.venv/bin/pip install -r /home/lucas/Work/SearaBoom-wt-pr/server/requirements.txt
```

Create a **separate** Cloudflare tunnel + hostname (not `searaboom.goossen.dev`, not tunnel name `searaboom`). `deploy/test.env.example` → `~/.config/searaboom/test.env`. Then:

```bash
./scripts/searaboom-host check --instance test
./scripts/searaboom-host install --instance test --enable
systemctl --user start searaboom-server@test.service searaboom-tunnel@test.service
```

Do not `systemctl restart searaboom-server` / `searaboom-tunnel` (those are Mini live). Do not publish OTA from the test tree.

## Commands

```bash
./scripts/searaboom-host check --instance prod|test [--role server|tunnel|all]
./scripts/searaboom-host serve --instance prod|test
./scripts/searaboom-host tunnel --instance prod|test
./scripts/searaboom-host install --instance test [--enable]
```

Check fails closed if the venv (server role), `cloudflared` (tunnel role), required env names, Origin key file (prod), or tunnel credentials file are missing. Tunnel creds must not be group/world-readable. The development admin-token default is refused unless `SEARABOOM_ALLOW_DEV_TOKEN=1`. `--role tunnel` does not require the Flask venv.

Legacy wrappers `scripts/run_server.sh` and `scripts/run_tunnel.sh` still exist for ad-hoc Mini use; they do **not** enforce this layout.

## What Flask stopping still means

If the **prod** Flask process dies: public USB factory page + bins, OTA check/download, `POST /api/logs`, Origin `/api/lab/origin-webhook`, admin/ouvintes all fail. Boxes keep playing radio. The tunnel process can stay up and 502. QA USB hardware is whatever machine has the boxes, not the git remote.

#!/usr/bin/env python3
"""Portable SearaBoom host: check deps/secrets, serve, tunnel, install units.

Does not retarget searaboom.goossen.dev. Never prints secret values.
Never connects a second connector to the live Mini tunnel.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import stat
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlparse

LIVE_HOST = "searaboom.goossen.dev"
LIVE_SERVER_UNIT = "searaboom-server.service"
LIVE_TUNNEL_UNIT = "searaboom-tunnel.service"
LIVE_UNITS = (LIVE_SERVER_UNIT, LIVE_TUNNEL_UNIT)
LIVE_TUNNEL_ID = "e70a4d09-968d-4dac-b0a2-6a39e009da6b"
LIVE_TUNNEL_NAME = "searaboom"
LIVE_PORT = 18080
TEMPLATE_SERVER = "searaboom-server@.service"
TEMPLATE_TUNNEL = "searaboom-tunnel@.service"
DEV_TOKEN_SENTINEL = "searaboom-dev"  # matches server/app.py default; do not log it

REQUIRED_SERVER_VARS = (
    "SEARABOOM_PORT",
    "SEARABOOM_PUBLIC_URL",
    "SEARABOOM_ADMIN_TOKEN",
)
REQUIRED_ORIGIN_VARS = (
    "ORIGIN_APP_ID",
    "ORIGIN_INSTALLATION_ID",
    "ORIGIN_APP_PRIVATE_KEY",
)


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def config_dir() -> Path:
    override = os.environ.get("SEARABOOM_CONFIG_DIR", "").strip()
    if override:
        return Path(override).expanduser()
    return Path.home() / ".config" / "searaboom"


def env_path(instance: str) -> Path:
    return config_dir() / f"{instance}.env"


def origin_env_path() -> Path:
    return config_dir() / "origin-app" / "env"


def parse_env_file(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :]
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip()
        if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
            val = val[1:-1]
        if key:
            out[key] = val
    return out


def load_instance_env(instance: str) -> Path | None:
    """Fill empty os.environ from origin-app/env then instance env (instance wins)."""
    pending: dict[str, str] = {}
    origin = origin_env_path()
    if origin.is_file():
        pending.update(parse_env_file(origin))
    path = env_path(instance)
    found = None
    if path.is_file():
        pending.update(parse_env_file(path))
        found = path
    for key, val in pending.items():
        if not os.environ.get(key, "").strip():
            os.environ[key] = val
    return found


def truthy(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


def public_host() -> str:
    url = os.environ.get("SEARABOOM_PUBLIC_URL", "").strip()
    if not url:
        return ""
    parsed = urlparse(url if "://" in url else f"https://{url}")
    return (parsed.hostname or "").lower()


def parse_tunnel_yaml(path: Path) -> dict[str, str | list[str]]:
    text = path.read_text(encoding="utf-8")
    hostnames: list[str] = []
    creds = ""
    tunnel = ""
    protocol = ""
    origin = ""
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        m = re.match(r"^\s*(?:-\s+)?hostname:\s*(\S+)\s*$", line)
        if m:
            hostnames.append(m.group(1).strip().strip("\"'"))
            continue
        m = re.match(r"^\s*(?:-\s+)?credentials-file:\s*(\S+)\s*$", line)
        if m:
            creds = m.group(1).strip().strip("\"'")
            continue
        m = re.match(r"^\s*(?:-\s+)?tunnel:\s*(\S+)\s*$", line)
        if m:
            tunnel = m.group(1).strip().strip("\"'")
            continue
        m = re.match(r"^\s*(?:-\s+)?protocol:\s*(\S+)\s*$", line)
        if m:
            protocol = m.group(1).strip().strip("\"'")
            continue
        m = re.match(r"^\s*(?:-\s+)?service:\s*(\S+)\s*$", line)
        if m:
            svc = m.group(1).strip().strip("\"'")
            if svc.startswith("http"):
                origin = svc
    return {
        "hostnames": hostnames,
        "credentials-file": creds,
        "tunnel": tunnel,
        "protocol": protocol,
        "service": origin,
    }


def credential_tunnel_identity(path: Path) -> tuple[str, str]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "", ""
    if not isinstance(data, dict):
        return "", ""
    return str(data.get("TunnelID") or "").strip(), str(data.get("TunnelName") or "").strip()


def systemd_user_dir() -> Path:
    xdg = os.environ.get("XDG_CONFIG_HOME", "").strip()
    if xdg:
        return Path(xdg) / "systemd" / "user"
    return Path.home() / ".config" / "systemd" / "user"


def systemctl_user(*args: str) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            ["systemctl", "--user", *args],
            check=False,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        return None


def unit_is_active(name: str) -> bool:
    r = systemctl_user("is-active", "--quiet", name)
    return bool(r and r.returncode == 0)


def unit_is_enabled(name: str) -> bool:
    r = systemctl_user("is-enabled", "--quiet", name)
    return bool(r and r.returncode == 0)


def live_units_present() -> bool:
    """True if Mini live units are active *or* enabled (next login would start them)."""
    return any(unit_is_active(n) or unit_is_enabled(n) for n in LIVE_UNITS)


def systemd_show_value(unit: str, prop: str) -> str:
    r = systemctl_user("show", "-p", prop, "--value", unit)
    if not r or r.returncode != 0:
        return ""
    return (r.stdout or "").strip()


def live_clone_roots() -> list[Path]:
    roots: list[Path] = []
    extra = os.environ.get("SEARABOOM_LIVE_ROOT", "").strip()
    if extra:
        roots.append(Path(extra).expanduser().resolve())
    wd = systemd_show_value(LIVE_SERVER_UNIT, "WorkingDirectory")
    if wd:
        p = Path(wd).expanduser().resolve()
        roots.append(p.parent if p.name == "server" else p)
    out: list[Path] = []
    seen: set[Path] = set()
    for p in roots:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def live_server_ports() -> set[int]:
    ports: set[int] = {LIVE_PORT}
    extra = os.environ.get("SEARABOOM_LIVE_PORT", "").strip()
    if extra:
        try:
            ports.add(int(extra))
        except ValueError:
            pass
    raw = systemd_show_value(LIVE_SERVER_UNIT, "Environment")
    for part in raw.split():
        if part.startswith("SEARABOOM_PORT="):
            try:
                ports.add(int(part.split("=", 1)[1]))
            except ValueError:
                pass
    return ports


def port_bound(host: str, port: int) -> bool:
    bind_host = host.strip() or "0.0.0.0"
    if bind_host in ("0.0.0.0", "::"):
        bind_host = "127.0.0.1"
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind((bind_host, port))
        except OSError:
            return True
        finally:
            sock.close()
    except OSError:
        return False
    return False


def which_or_missing(name: str) -> str | None:
    return shutil.which(name)


def venv_python(root: Path) -> Path:
    return root / "server" / ".venv" / "bin" / "python"


def ident_is_live_tunnel(value: str) -> bool:
    v = value.strip().lower()
    return v in {LIVE_TUNNEL_ID.lower(), LIVE_TUNNEL_NAME.lower()}


def live_tunnel_error(
    _instance: str,
    parsed: dict[str, str | list[str]],
    creds: str,
) -> str | None:
    ident = str(parsed.get("tunnel") or "")
    if ident_is_live_tunnel(ident):
        return (
            "tunnel id/name is the live Mini tunnel; a second connector "
            f"can 404 {LIVE_HOST}. Create a new named tunnel."
        )
    name = os.environ.get("SEARABOOM_TUNNEL_NAME", "").strip()
    if ident_is_live_tunnel(name):
        return (
            "SEARABOOM_TUNNEL_NAME is the live Mini tunnel; create a new named tunnel"
        )
    if not creds:
        return None
    path = Path(creds).expanduser()
    try:
        resolved = path.resolve()
    except OSError:
        resolved = path
    for live_root in live_clone_roots():
        live_tunnel_dir = (live_root / "tunnel").resolve()
        try:
            if resolved.is_relative_to(live_tunnel_dir):
                return "credentials-file is under the live clone tunnel/ directory"
        except (OSError, ValueError):
            pass
    if resolved.name.lower() == f"{LIVE_TUNNEL_ID.lower()}.json":
        return "credentials-file is the live tunnel JSON"
    if path.is_file():
        tid, tname = credential_tunnel_identity(path)
        if ident_is_live_tunnel(tid) or ident_is_live_tunnel(tname):
            return (
                "credentials-file is the live Mini tunnel; copying it still "
                f"registers a second connector on {LIVE_HOST}"
            )
    return None


def check_deps(root: Path, role: str) -> list[str]:
    errs: list[str] = []
    if not which_or_missing("python3"):
        errs.append("missing command: python3")
    if role in ("server", "all"):
        py = venv_python(root)
        if not py.is_file():
            errs.append(
                f"missing venv python: {py} (python3 -m venv server/.venv && "
                "server/.venv/bin/pip install -r server/requirements.txt)"
            )
        else:
            r = subprocess.run(
                [str(py), "-c", "import flask, cryptography"],
                check=False,
                capture_output=True,
                text=True,
            )
            if r.returncode != 0:
                errs.append("venv missing pip packages flask and/or cryptography")
    if role in ("tunnel", "all") and not which_or_missing("cloudflared"):
        errs.append("missing command: cloudflared")
    if truthy("SEARABOOM_REQUIRE_ESPTOOL", False):
        esp = os.environ.get("SEARABOOM_ESPTOOL_PYTHON", "").strip()
        if not esp or not Path(esp).is_file():
            errs.append("SEARABOOM_ESPTOOL_PYTHON is missing or not a file")
    return errs


def check_secrets(instance: str, role: str, env_file: Path | None, root: Path) -> tuple[list[str], list[str]]:
    errs: list[str] = []
    warns: list[str] = []
    if env_file is None and not os.environ.get("SEARABOOM_PORT") and role in ("server", "all"):
        errs.append(f"missing env file {env_path(instance)} (copy deploy/{instance}.env.example)")
    for key in REQUIRED_SERVER_VARS:
        if role in ("server", "all") and not os.environ.get(key, "").strip():
            errs.append(f"missing required env: {key}")
    token = os.environ.get("SEARABOOM_ADMIN_TOKEN", "").strip()
    if role in ("server", "all") and token == DEV_TOKEN_SENTINEL:
        if truthy("SEARABOOM_ALLOW_DEV_TOKEN", False):
            warns.append("SEARABOOM_ADMIN_TOKEN is the development default")
        else:
            errs.append(
                "SEARABOOM_ADMIN_TOKEN is the development default; "
                "set a unique token or SEARABOOM_ALLOW_DEV_TOKEN=1"
            )
    host = public_host()
    if role in ("server", "all") and instance == "test" and host == LIVE_HOST:
        errs.append(
            f"test instance must not use SEARABOOM_PUBLIC_URL host {LIVE_HOST}"
        )
    if instance == "test" and role in ("server", "all"):
        resolved = root.resolve()
        for live_root in live_clone_roots():
            if resolved == live_root:
                errs.append(
                    "test SEARABOOM_ROOT is the live clone; it would write "
                    "server/logs and serve live factory/OTA blobs"
                )
                break
        port_raw = os.environ.get("SEARABOOM_PORT", "").strip()
        try:
            port = int(port_raw) if port_raw else None
        except ValueError:
            port = None
            errs.append("SEARABOOM_PORT must be an integer")
        if port is not None:
            forbidden = live_server_ports()
            if port in forbidden:
                errs.append(
                    f"test SEARABOOM_PORT {port} is the live server port; "
                    "use another port (e.g. 18081)"
                )
            elif port_bound(os.environ.get("SEARABOOM_HOST", "127.0.0.1"), port):
                errs.append(f"SEARABOOM_PORT {port} is already bound")
    require_origin = truthy(
        "SEARABOOM_REQUIRE_ORIGIN",
        default=(instance == "prod" and role in ("server", "all")),
    )
    if require_origin:
        for key in REQUIRED_ORIGIN_VARS:
            if not os.environ.get(key, "").strip():
                errs.append(f"missing required env: {key}")
        key_path = os.environ.get("ORIGIN_APP_PRIVATE_KEY", "").strip()
        if key_path and not Path(key_path).expanduser().is_file():
            errs.append("ORIGIN_APP_PRIVATE_KEY path does not exist")
    if role in ("tunnel", "all"):
        cfg = os.environ.get("SEARABOOM_TUNNEL_CONFIG", "").strip()
        if not cfg:
            errs.append("missing required env: SEARABOOM_TUNNEL_CONFIG")
        else:
            path = Path(cfg).expanduser()
            if not path.is_file():
                errs.append(f"missing tunnel config: {path}")
            else:
                parsed = parse_tunnel_yaml(path)
                creds = str(parsed.get("credentials-file") or "")
                live_err = live_tunnel_error(instance, parsed, creds)
                if live_err:
                    errs.append(live_err)
                if not creds:
                    errs.append("tunnel config missing credentials-file")
                elif not Path(creds).expanduser().is_file():
                    errs.append("tunnel credentials-file does not exist")
                else:
                    mode = Path(creds).expanduser().stat().st_mode
                    if mode & (stat.S_IRWXG | stat.S_IRWXO):
                        errs.append("tunnel credentials-file must not be group/world readable")
                hosts = [h.lower() for h in (parsed.get("hostnames") or [])]
                if instance == "test" and LIVE_HOST in hosts:
                    errs.append(
                        f"test tunnel config must not ingress {LIVE_HOST}"
                    )
                if instance == "prod" and host and host not in hosts and hosts:
                    warns.append(
                        "SEARABOOM_PUBLIC_URL host is not in tunnel ingress hostnames"
                    )
                proto = str(parsed.get("protocol") or "").lower()
                if proto and proto != "http2":
                    warns.append("tunnel protocol is not http2 (Mini network needs HTTP/2)")
    return errs, warns


def resolve_root() -> Path:
    raw = os.environ.get("SEARABOOM_ROOT", "").strip()
    if raw:
        return Path(raw).expanduser().resolve()
    return repo_root()


def run_check(instance: str, role: str) -> int:
    env_file = load_instance_env(instance)
    root = resolve_root()
    os.environ["SEARABOOM_ROOT"] = str(root)
    os.environ["SEARABOOM_INSTANCE"] = instance
    secret_errs, warns = check_secrets(instance, role, env_file, root)
    errs = check_deps(root, role) + secret_errs
    for w in warns:
        print(f"warning: {w}", file=sys.stderr)
    if errs:
        for e in errs:
            print(f"error: {e}", file=sys.stderr)
        print(
            f"searaboom-host check failed instance={instance} role={role}",
            file=sys.stderr,
        )
        return 1
    print(f"ok instance={instance} role={role} root={root}")
    return 0


def cmd_serve(instance: str) -> int:
    rc = run_check(instance, "server")
    if rc != 0:
        return rc
    root = resolve_root()
    py = venv_python(root)
    server = root / "server"
    os.chdir(server)
    os.execv(str(py), [str(py), "app.py"])
    return 1


def cmd_tunnel(instance: str) -> int:
    rc = run_check(instance, "tunnel")
    if rc != 0:
        return rc
    cfg = Path(os.environ["SEARABOOM_TUNNEL_CONFIG"]).expanduser()
    name = os.environ.get("SEARABOOM_TUNNEL_NAME", "").strip()
    parsed = parse_tunnel_yaml(cfg)
    if not name:
        name = str(parsed.get("tunnel") or "")
    if ident_is_live_tunnel(name):
        print(
            "error: refusing to run the live Mini tunnel from searaboom-host",
            file=sys.stderr,
        )
        return 1
    cloudflared = which_or_missing("cloudflared")
    if not cloudflared:
        print("error: missing command: cloudflared", file=sys.stderr)
        return 1
    cmd = [cloudflared, "tunnel", "--config", str(cfg), "--protocol", "http2", "run"]
    if name:
        cmd.append(name)
    os.execv(cloudflared, cmd)
    return 1


def cmd_install(instance: str, enable: bool) -> int:
    if instance == "prod" and live_units_present():
        print(
            f"error: live units {LIVE_SERVER_UNIT}/{LIVE_TUNNEL_UNIT} are "
            "active or enabled; refusing to install prod templates on this "
            "host (both would start on the live port at next login). "
            "Install test instead, or install prod on a new machine.",
            file=sys.stderr,
        )
        return 1
    root = repo_root()
    src_dir = root / "contrib" / "systemd"
    dest = systemd_user_dir()
    dest.mkdir(parents=True, exist_ok=True)
    for name in (TEMPLATE_SERVER, TEMPLATE_TUNNEL):
        src = src_dir / name
        if not src.is_file():
            print(f"error: missing template {src}", file=sys.stderr)
            return 1
        target = dest / name
        shutil.copyfile(src, target)
        print(f"wrote {target}")
    if enable:
        try:
            subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
            for unit in (
                f"searaboom-server@{instance}.service",
                f"searaboom-tunnel@{instance}.service",
            ):
                subprocess.run(["systemctl", "--user", "enable", unit], check=True)
                print(f"enabled {unit} (not started)")
        except (FileNotFoundError, subprocess.CalledProcessError) as e:
            print(f"error: systemd --user failed: {e}", file=sys.stderr)
            return 1
        print(
            "start later with: systemctl --user start "
            f"searaboom-server@{instance}.service searaboom-tunnel@{instance}.service"
        )
    else:
        print("templates copied; not enabled. pass --enable after check succeeds.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "command",
        choices=("check", "serve", "tunnel", "install"),
        help="check deps+secrets, run Flask, run cloudflared, or install user units",
    )
    p.add_argument(
        "--instance",
        default=os.environ.get("SEARABOOM_INSTANCE", "prod"),
        choices=("prod", "test"),
        help="prod from main, or test from a PR worktree (different port/hostname)",
    )
    p.add_argument(
        "--role",
        default="all",
        choices=("server", "tunnel", "all"),
        help="check only (ignored for serve/tunnel/install)",
    )
    p.add_argument(
        "--enable",
        action="store_true",
        help="install: enable user units but do not start them",
    )
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    instance = args.instance
    if args.command == "check":
        return run_check(instance, args.role)
    if args.command == "serve":
        return cmd_serve(instance)
    if args.command == "tunnel":
        return cmd_tunnel(instance)
    if args.command == "install":
        return cmd_install(instance, args.enable)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "scripts" / "searaboom_host.py"
LIVE_TUNNEL_ID = "e70a4d09-968d-4dac-b0a2-6a39e009da6b"
NEW_TUNNEL_ID = "11111111-1111-1111-1111-111111111111"


def run(args: list[str], env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    merged = os.environ.copy()
    merged.update(env)
    return subprocess.run(
        [sys.executable, str(HOST), *args],
        cwd=str(ROOT),
        env=merged,
        text=True,
        capture_output=True,
    )


def fake_root(tmp: Path, *, venv: bool = True) -> Path:
    root = tmp / "repo"
    if venv:
        py = root / "server" / ".venv" / "bin" / "python"
        py.parent.mkdir(parents=True, exist_ok=True)
        py.write_text("#!/bin/sh\nexit 0\n")
        py.chmod(0o755)
    else:
        (root / "server").mkdir(parents=True, exist_ok=True)
    return root


def write_env(path: Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    path.chmod(0o600)


def write_creds(path: Path, *, tunnel_id: str, name: str, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({
        "AccountTag": "test",
        "TunnelID": tunnel_id,
        "TunnelName": name,
        "TunnelSecret": "unit-test-not-a-secret",
    }) + "\n")
    path.chmod(mode)


def write_tunnel_yml(path: Path, *, tunnel: str, creds: Path, hostname: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join(
            [
                "protocol: http2",
                f"tunnel: {tunnel}",
                f"credentials-file: {creds}",
                "ingress:",
                f"  - hostname: {hostname}",
                "    service: http://127.0.0.1:18081",
                "  - service: http_status:404",
            ]
        )
        + "\n"
    )


def test_env(root: Path, cfg: Path, extra: list[str] | None = None) -> str:
    lines = [
        "SEARABOOM_PORT=18081",
        "SEARABOOM_PUBLIC_URL=https://searaboom-test.example.com",
        "SEARABOOM_ADMIN_TOKEN=unit-test-token",
        "SEARABOOM_REQUIRE_ORIGIN=0",
        f"SEARABOOM_ROOT={root}",
    ]
    if extra:
        lines.extend(extra)
    return "\n".join(lines) + "\n"


def check_env(cfg: Path, root: Path, tmp: Path) -> dict[str, str]:
    return {
        "SEARABOOM_CONFIG_DIR": str(cfg),
        "PATH": os.environ.get("PATH", "/usr/bin"),
        "HOME": str(tmp / "home"),
        "SEARABOOM_ROOT": str(root),
    }


def write_fake_systemctl(bindir: Path, *, active: bool, enabled: bool) -> None:
    bindir.mkdir(parents=True, exist_ok=True)
    script = bindir / "systemctl"
    script.write_text(
        "\n".join(
            [
                "#!/usr/bin/env python3",
                "import sys",
                "args = sys.argv[1:]",
                "live = any(a in ('searaboom-server.service', 'searaboom-tunnel.service') for a in args)",
                "if 'is-active' in args:",
                f"    sys.exit(0 if live and {active!r} else 1)",
                "if 'is-enabled' in args:",
                f"    sys.exit(0 if live and {enabled!r} else 1)",
                "if 'show' in args:",
                "    sys.exit(0)",
                "if 'daemon-reload' in args or 'enable' in args:",
                "    sys.exit(0)",
                "sys.exit(1)",
                "",
            ]
        )
    )
    script.chmod(0o755)


def test_missing_env_refuses() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        r = run(
            ["check", "--instance", "test", "--role", "server"],
            {
                "SEARABOOM_CONFIG_DIR": str(tmp / "cfg"),
                "SEARABOOM_ROOT": str(fake_root(tmp)),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 1, r.stderr
        assert "missing env file" in r.stderr or "missing required env" in r.stderr


def test_test_instance_refuses_live_hostname() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        write_env(
            cfg / "test.env",
            test_env(root, cfg).replace(
                "https://searaboom-test.example.com",
                "https://searaboom.goossen.dev",
            ),
        )
        r = run(["check", "--instance", "test", "--role", "server"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "must not use" in r.stderr
        assert "searaboom.goossen.dev" in r.stderr
        assert "unit-test-token" not in r.stdout + r.stderr


def test_test_tunnel_refuses_live_ingress() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test")
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom-test", creds=creds, hostname="searaboom.goossen.dev"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "must not ingress" in r.stderr


def test_test_refuses_live_tunnel_uuid() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test")
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml,
            tunnel=LIVE_TUNNEL_ID,
            creds=creds,
            hostname="searaboom-test.example.com",
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "live Mini tunnel" in r.stderr
        assert "second connector" in r.stderr


def test_test_refuses_live_tunnel_name() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test")
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom", creds=creds, hostname="searaboom-test.example.com"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "live Mini tunnel" in r.stderr


def test_test_refuses_copied_live_creds_json() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "copied.json"
        write_creds(creds, tunnel_id=LIVE_TUNNEL_ID, name="searaboom")
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom-test", creds=creds, hostname="searaboom-test.example.com"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "credentials-file is the live Mini tunnel" in r.stderr
        assert "TunnelSecret" not in r.stdout + r.stderr


def test_test_refuses_creds_under_live_clone_tunnel() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        live = tmp / "live-clone"
        creds = live / "tunnel" / "other.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test")
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom-test", creds=creds, hostname="searaboom-test.example.com"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        env = check_env(cfg, root, tmp)
        env["SEARABOOM_LIVE_ROOT"] = str(live)
        r = run(["check", "--instance", "test", "--role", "tunnel"], env)
        assert r.returncode == 1, r.stderr
        assert "live clone tunnel/" in r.stderr


def test_test_refuses_live_root() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        write_env(cfg / "test.env", test_env(root, cfg))
        env = check_env(cfg, root, tmp)
        env["SEARABOOM_LIVE_ROOT"] = str(root)
        r = run(["check", "--instance", "test", "--role", "server"], env)
        assert r.returncode == 1, r.stderr
        assert "live clone" in r.stderr
        assert "server/logs" in r.stderr


def test_test_refuses_live_port() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        write_env(
            cfg / "test.env",
            test_env(root, cfg).replace("SEARABOOM_PORT=18081", "SEARABOOM_PORT=18080"),
        )
        r = run(["check", "--instance", "test", "--role", "server"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "live server port" in r.stderr


def test_ok_test_server() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        write_env(cfg / "test.env", test_env(root, cfg))
        r = run(["check", "--instance", "test", "--role", "server"], check_env(cfg, root, tmp))
        assert r.returncode == 0, r.stderr + r.stdout
        assert "ok instance=test" in r.stdout


def test_ok_test_tunnel_new_creds() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test")
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom-test", creds=creds, hostname="searaboom-test.example.com"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 0, r.stderr + r.stdout


def test_tunnel_role_skips_venv() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test")
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom-test", creds=creds, hostname="searaboom-test.example.com"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 0, r.stderr + r.stdout
        assert "venv" not in r.stderr


def test_prod_missing_origin_refuses() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        write_env(
            cfg / "prod.env",
            "\n".join(
                [
                    "SEARABOOM_PORT=18080",
                    "SEARABOOM_PUBLIC_URL=https://searaboom.goossen.dev",
                    "SEARABOOM_ADMIN_TOKEN=unit-test-token",
                    f"SEARABOOM_ROOT={root}",
                ]
            )
            + "\n",
        )
        r = run(["check", "--instance", "prod", "--role", "server"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "ORIGIN_APP_ID" in r.stderr


def test_tunnel_missing_creds_refuses() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml,
            tunnel="searaboom-test",
            creds=cfg / "tunnel" / "missing.json",
            hostname="searaboom-test.example.com",
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "credentials-file does not exist" in r.stderr


def test_world_readable_creds_refuses() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp, venv=False)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        write_creds(creds, tunnel_id=NEW_TUNNEL_ID, name="searaboom-test", mode=0o644)
        yml = cfg / "tunnel" / "test.yml"
        write_tunnel_yml(
            yml, tunnel="searaboom-test", creds=creds, hostname="searaboom-test.example.com"
        )
        write_env(cfg / "test.env", test_env(root, cfg, [f"SEARABOOM_TUNNEL_CONFIG={yml}"]))
        r = run(["check", "--instance", "test", "--role", "tunnel"], check_env(cfg, root, tmp))
        assert r.returncode == 1, r.stderr
        assert "must not be group/world readable" in r.stderr
        assert stat.S_IMODE(creds.stat().st_mode) & 0o077


def test_install_prod_refuses_when_live_units_enabled() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        xdg = tmp / "xdg"
        bindir = tmp / "bin"
        write_fake_systemctl(bindir, active=False, enabled=True)
        r = run(
            ["install", "--instance", "prod"],
            {
                "HOME": str(tmp / "home"),
                "XDG_CONFIG_HOME": str(xdg),
                "PATH": f"{bindir}:{os.environ.get('PATH', '/usr/bin')}",
            },
        )
        assert r.returncode == 1, r.stderr
        assert "active or enabled" in r.stderr
        dest = xdg / "systemd" / "user" / "searaboom-server@.service"
        assert not dest.exists()


def test_install_prod_copies_when_no_live_units() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        xdg = tmp / "xdg"
        bindir = tmp / "bin"
        write_fake_systemctl(bindir, active=False, enabled=False)
        r = run(
            ["install", "--instance", "prod"],
            {
                "HOME": str(tmp / "home"),
                "XDG_CONFIG_HOME": str(xdg),
                "PATH": f"{bindir}:{os.environ.get('PATH', '/usr/bin')}",
            },
        )
        assert r.returncode == 0, r.stderr + r.stdout
        dest = xdg / "systemd" / "user" / "searaboom-server@.service"
        assert dest.is_file()
        text = dest.read_text()
        assert "origin-app/env" in text


def test_install_test_templates_to_xdg() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        xdg = tmp / "xdg"
        bindir = tmp / "bin"
        write_fake_systemctl(bindir, active=True, enabled=True)
        r = run(
            ["install", "--instance", "test"],
            {
                "HOME": str(tmp / "home"),
                "XDG_CONFIG_HOME": str(xdg),
                "PATH": f"{bindir}:{os.environ.get('PATH', '/usr/bin')}",
            },
        )
        assert r.returncode == 0, r.stderr + r.stdout
        dest = xdg / "systemd" / "user" / "searaboom-server@.service"
        assert dest.is_file()
        assert dest.name == "searaboom-server@.service"


if __name__ == "__main__":
    tests = [
        test_missing_env_refuses,
        test_test_instance_refuses_live_hostname,
        test_test_tunnel_refuses_live_ingress,
        test_test_refuses_live_tunnel_uuid,
        test_test_refuses_live_tunnel_name,
        test_test_refuses_copied_live_creds_json,
        test_test_refuses_creds_under_live_clone_tunnel,
        test_test_refuses_live_root,
        test_test_refuses_live_port,
        test_ok_test_server,
        test_ok_test_tunnel_new_creds,
        test_tunnel_role_skips_venv,
        test_prod_missing_origin_refuses,
        test_tunnel_missing_creds_refuses,
        test_world_readable_creds_refuses,
        test_install_prod_refuses_when_live_units_enabled,
        test_install_prod_copies_when_no_live_units,
        test_install_test_templates_to_xdg,
    ]
    for fn in tests:
        fn()
        print(f"ok {fn.__name__}")
    print("ok")

#!/usr/bin/env python3
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "scripts" / "searaboom_host.py"


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


def fake_root(tmp: Path) -> Path:
    root = tmp / "repo"
    py = root / "server" / ".venv" / "bin" / "python"
    py.parent.mkdir(parents=True)
    py.write_text("#!/bin/sh\nexit 0\n")
    py.chmod(0o755)
    return root


def write_env(path: Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body)
    path.chmod(0o600)


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
            "\n".join(
                [
                    "SEARABOOM_PORT=18081",
                    "SEARABOOM_PUBLIC_URL=https://searaboom.goossen.dev",
                    "SEARABOOM_ADMIN_TOKEN=unit-test-token",
                    "SEARABOOM_REQUIRE_ORIGIN=0",
                    f"SEARABOOM_ROOT={root}",
                ]
            )
            + "\n",
        )
        r = run(
            ["check", "--instance", "test", "--role", "server"],
            {
                "SEARABOOM_CONFIG_DIR": str(cfg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 1, r.stderr
        assert "must not use" in r.stderr
        assert "searaboom.goossen.dev" in r.stderr


def test_test_tunnel_refuses_live_ingress() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        creds.parent.mkdir(parents=True)
        creds.write_text("{}\n")
        creds.chmod(0o600)
        yml = cfg / "tunnel" / "test.yml"
        yml.write_text(
            "\n".join(
                [
                    "protocol: http2",
                    "tunnel: searaboom-test",
                    f"credentials-file: {creds}",
                    "ingress:",
                    "  - hostname: searaboom.goossen.dev",
                    "    service: http://127.0.0.1:18081",
                    "  - service: http_status:404",
                ]
            )
            + "\n"
        )
        write_env(
            cfg / "test.env",
            "\n".join(
                [
                    "SEARABOOM_PORT=18081",
                    "SEARABOOM_PUBLIC_URL=https://searaboom-test.example.com",
                    "SEARABOOM_ADMIN_TOKEN=unit-test-token",
                    "SEARABOOM_REQUIRE_ORIGIN=0",
                    f"SEARABOOM_TUNNEL_CONFIG={yml}",
                    f"SEARABOOM_ROOT={root}",
                ]
            )
            + "\n",
        )
        r = run(
            ["check", "--instance", "test", "--role", "tunnel"],
            {
                "SEARABOOM_CONFIG_DIR": str(cfg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 1, r.stderr
        assert "must not ingress" in r.stderr


def test_ok_test_server() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        write_env(
            cfg / "test.env",
            "\n".join(
                [
                    "SEARABOOM_PORT=18081",
                    "SEARABOOM_PUBLIC_URL=https://searaboom-test.example.com",
                    "SEARABOOM_ADMIN_TOKEN=unit-test-token",
                    "SEARABOOM_REQUIRE_ORIGIN=0",
                    f"SEARABOOM_ROOT={root}",
                ]
            )
            + "\n",
        )
        r = run(
            ["check", "--instance", "test", "--role", "server"],
            {
                "SEARABOOM_CONFIG_DIR": str(cfg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 0, r.stderr + r.stdout
        assert "ok instance=test" in r.stdout


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
        r = run(
            ["check", "--instance", "prod", "--role", "server"],
            {
                "SEARABOOM_CONFIG_DIR": str(cfg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 1, r.stderr
        assert "ORIGIN_APP_ID" in r.stderr


def test_tunnel_missing_creds_refuses() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        yml = cfg / "tunnel" / "test.yml"
        yml.parent.mkdir(parents=True)
        yml.write_text(
            "\n".join(
                [
                    "protocol: http2",
                    "tunnel: searaboom-test",
                    f"credentials-file: {cfg / 'tunnel' / 'missing.json'}",
                    "ingress:",
                    "  - hostname: searaboom-test.example.com",
                    "    service: http://127.0.0.1:18081",
                    "  - service: http_status:404",
                ]
            )
            + "\n"
        )
        write_env(
            cfg / "test.env",
            "\n".join(
                [
                    "SEARABOOM_PORT=18081",
                    "SEARABOOM_PUBLIC_URL=https://searaboom-test.example.com",
                    "SEARABOOM_ADMIN_TOKEN=unit-test-token",
                    "SEARABOOM_REQUIRE_ORIGIN=0",
                    f"SEARABOOM_TUNNEL_CONFIG={yml}",
                    f"SEARABOOM_ROOT={root}",
                ]
            )
            + "\n",
        )
        r = run(
            ["check", "--instance", "test", "--role", "tunnel"],
            {
                "SEARABOOM_CONFIG_DIR": str(cfg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 1, r.stderr
        assert "credentials-file does not exist" in r.stderr


def test_world_readable_creds_refuses() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        root = fake_root(tmp)
        cfg = tmp / "cfg"
        creds = cfg / "tunnel" / "creds.json"
        creds.parent.mkdir(parents=True)
        creds.write_text("{}\n")
        creds.chmod(0o644)
        yml = cfg / "tunnel" / "test.yml"
        yml.write_text(
            "\n".join(
                [
                    "protocol: http2",
                    "tunnel: searaboom-test",
                    f"credentials-file: {creds}",
                    "ingress:",
                    "  - hostname: searaboom-test.example.com",
                    "    service: http://127.0.0.1:18081",
                    "  - service: http_status:404",
                ]
            )
            + "\n"
        )
        write_env(
            cfg / "test.env",
            "\n".join(
                [
                    "SEARABOOM_PORT=18081",
                    "SEARABOOM_PUBLIC_URL=https://searaboom-test.example.com",
                    "SEARABOOM_ADMIN_TOKEN=unit-test-token",
                    "SEARABOOM_REQUIRE_ORIGIN=0",
                    f"SEARABOOM_TUNNEL_CONFIG={yml}",
                    f"SEARABOOM_ROOT={root}",
                ]
            )
            + "\n",
        )
        r = run(
            ["check", "--instance", "test", "--role", "tunnel"],
            {
                "SEARABOOM_CONFIG_DIR": str(cfg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
                "HOME": str(tmp / "home"),
            },
        )
        assert r.returncode == 1, r.stderr
        assert "must not be group/world readable" in r.stderr
        assert stat.S_IMODE(creds.stat().st_mode) & 0o077


def test_install_prod_refuses_when_live_units_active() -> None:
    if subprocess.run(
        ["systemctl", "--user", "is-active", "--quiet", "searaboom-server.service"],
        check=False,
    ).returncode != 0:
        return
    before = Path.home() / ".config/systemd/user/searaboom-server@.service"
    existed = before.is_file()
    r = run(
        ["install", "--instance", "prod"],
        {"HOME": str(Path.home()), "PATH": os.environ.get("PATH", "/usr/bin")},
    )
    assert r.returncode == 1, r.stderr
    assert "live units" in r.stderr
    assert before.is_file() == existed


def test_install_test_templates_to_xdg() -> None:
    with tempfile.TemporaryDirectory() as raw:
        tmp = Path(raw)
        xdg = tmp / "xdg"
        r = run(
            ["install", "--instance", "test"],
            {
                "HOME": str(tmp / "home"),
                "XDG_CONFIG_HOME": str(xdg),
                "PATH": os.environ.get("PATH", "/usr/bin"),
            },
        )
        assert r.returncode == 0, r.stderr + r.stdout
        dest = xdg / "systemd" / "user" / "searaboom-server@.service"
        assert dest.is_file()
        assert "searaboom-server.service" not in dest.name or dest.name.endswith("@.service")
        live = Path.home() / ".config/systemd/user/searaboom-server.service"
        assert live.is_file()  # Mini prod unit still there; we used XDG


if __name__ == "__main__":
    tests = [
        test_missing_env_refuses,
        test_test_instance_refuses_live_hostname,
        test_test_tunnel_refuses_live_ingress,
        test_ok_test_server,
        test_prod_missing_origin_refuses,
        test_tunnel_missing_creds_refuses,
        test_world_readable_creds_refuses,
        test_install_prod_refuses_when_live_units_active,
        test_install_test_templates_to_xdg,
    ]
    for fn in tests:
        fn()
        print(f"ok {fn.__name__}")
    print("ok")

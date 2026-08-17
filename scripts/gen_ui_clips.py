#!/usr/bin/env python3
"""Generate SearaBoom UI AAC clips with Grok Carina (pt-BR)."""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import requests

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "firmware" / "clips"
AUTH = Path.home() / ".grok" / "auth.json"

CLIPS = [
    {
        "id": "ota_updating",
        "text": (
            "SearaBoom atualizando. [pause] Em poucos instantes, voltaremos à programação normal."
        ),
    },
    {
        "id": "ota_done",
        "text": (
            "SearaBoom atualizado. [pause] "
            "<excited>Obrigada pela sintonia!</excited>"
        ),
    },
    {
        "id": "ap_welcome",
        "text": (
            "Olá! Eu sou o SearaBoom, e ainda não consegui conectar no Wi-Fi. [pause] "
            "No seu celular, abra as configurações de Wi-Fi e conecte na rede SearBoomSetup. "
            "Não precisa de senha. [pause] Depois, abra o navegador e acesse o endereço "
            "4 ponto 3 ponto 2 ponto 1. Lá você escolhe a rede da sua casa e a rádio."
        ),
    },
    {
        "id": "ap_connected",
        "text": (
            "Muito bem, você já está conectado! [pause] "
            "Abra o navegador no celular. Se a página não aparecer sozinha, "
            "digite 4 ponto 3 ponto 2 ponto 1. [pause] "
            "Escolha o Wi-Fi da sua casa, digite a senha, selecione a rádio "
            "e toque em Salvar. Eu reinicio sozinho e começo a tocar."
        ),
    },
    {
        "id": "ap_saved",
        "text": (
            "Pronto! Configuração salva. [pause] Estou reiniciando para conectar na sua rede. Até já!"
        ),
    },
]


def grok_key() -> str:
    env = os.environ.get("XAI_API_KEY")
    if env:
        return env
    if not AUTH.exists():
        raise SystemExit("set XAI_API_KEY or sign in with grok CLI")
    data = json.loads(AUTH.read_text())
    rec = None
    for v in data.values():
        if isinstance(v, dict) and v.get("refresh_token"):
            rec = v
            break
    if not rec:
        raise SystemExit(f"no grok OIDC session in {AUTH}")
    issuer = str(rec.get("oidc_issuer") or "https://auth.x.ai").rstrip("/")
    disc = requests.get(f"{issuer}/.well-known/openid-configuration", timeout=15)
    disc.raise_for_status()
    token_url = disc.json()["token_endpoint"]
    r = requests.post(
        token_url,
        data={
            "grant_type": "refresh_token",
            "refresh_token": rec["refresh_token"],
            "client_id": rec["oidc_client_id"],
        },
        timeout=20,
    )
    r.raise_for_status()
    return r.json()["access_token"]


def synthesize(session: requests.Session, key: str, text: str, dest_mp3: Path) -> None:
    r = session.post(
        "https://api.x.ai/v1/tts",
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": "application/json",
        },
        json={
            "text": text,
            "voice_id": "carina",
            "language": "pt-BR",
            "output_format": {
                "codec": "mp3",
                "sample_rate": 24000,
                "bit_rate": 64000,
            },
            "replace": {
                "SearaBoom": "Seara Boom",
                "SearBoomSetup": "Sear Boom Setup",
            },
        },
        timeout=60,
    )
    if r.status_code >= 400:
        raise RuntimeError(f"TTS {r.status_code}: {r.text[:500]}")
    dest_mp3.write_bytes(r.content)


def to_aac(src: Path, dest: Path) -> None:
    subprocess.check_call(
        [
            "ffmpeg",
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-i",
            str(src),
            "-ac",
            "1",
            "-ar",
            "22050",
            "-c:a",
            "aac",
            "-b:a",
            "32k",
            "-f",
            "adts",
            str(dest),
        ]
    )


def probe(path: Path) -> str:
    out = subprocess.check_output(
        [
            "ffprobe",
            "-hide_banner",
            "-show_entries",
            "format=duration,size,bit_rate",
            "-of",
            "default=noprint_wrappers=1",
            str(path),
        ],
        stderr=subprocess.STDOUT,
        text=True,
    )
    return out.strip()


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    key = grok_key()
    session = requests.Session()
    for clip in CLIPS:
        mp3 = OUT_DIR / f"{clip['id']}.mp3"
        aac = OUT_DIR / f"{clip['id']}.aac"
        print(f"TTS {clip['id']} ...")
        synthesize(session, key, clip["text"], mp3)
        to_aac(mp3, aac)
        print(f"  {aac.name} {aac.stat().st_size} bytes")
        print("  " + probe(aac).replace("\n", " | "))
        mp3.unlink(missing_ok=True)
    (OUT_DIR / "SOURCE.txt").write_text("xAI Grok Carina pt-BR\n")
    print("done", OUT_DIR)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)

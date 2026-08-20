#!/usr/bin/env python3
"""Generate SearaBoom UI AAC clips with Grok Carina (pt-BR, Ceará).

Output matches the radio decoder: AAC-LC ADTS, 44100 Hz, mono.
"""
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

# Spoken as a person from Ceará: warm, direct, not a caricature.
# One idea per clip. Welcome does not mention the portal or the IP.
CLIPS = [
    {
        "id": "ap_welcome",
        "text": (
            "Bem-vindo a Seara Boom. [pause] "
            "Para configurar, no celular, conecte no Wi-Fi Seara Boom. "
            "Não precisa de senha."
        ),
    },
    {
        "id": "ap_connected",
        "text": (
            "Pronto, você já tá conectado. [pause] "
            "Abra o navegador. Se a página não aparecer, "
            "acesse quatro ponto três ponto dois ponto um."
        ),
    },
    {
        "id": "ap_page",
        "text": (
            "Esta é a página de configuração. [pause] "
            "Primeiro escolha a rádio. Depois a rede Wi-Fi da sua casa, "
            "a senha, e toque em Salvar."
        ),
    },
    {
        "id": "ap_form_station",
        "text": (
            "Escolha a rádio. Nova Russas, FM cento e dois ponto sete, "
            "ou Ibiapina, FM cento e quatro ponto sete."
        ),
    },
    {
        "id": "ap_form_wifi",
        "text": "Agora escolha a rede Wi-Fi da sua casa.",
    },
    {
        "id": "ap_form_password",
        "text": "Digite a senha do Wi-Fi da sua casa.",
    },
    {
        "id": "ap_form_save",
        "text": "Toque em Salvar. Eu reinicio sozinho e começo a tocar.",
    },
    {
        "id": "ap_saved",
        "text": "Pronto! Configuração salva. [pause] Tô reiniciando. Até já!",
    },
    {
        "id": "tune_102",
        "text": "Sintonizando Rádio Seara, FM cento e dois ponto sete.",
    },
    {
        "id": "tune_104",
        "text": "Sintonizando Rádio Seara, FM cento e quatro ponto sete.",
    },
    {
        "id": "ota_available",
        "text": (
            "Atualização disponível. [pause] Tô baixando agora. "
            "Daqui a pouco a gente volta."
        ),
    },
    {
        "id": "ota_rebooting",
        "text": "Download concluído. Reiniciando.",
    },
    {
        "id": "ota_done",
        "text": (
            "Atualização concluída. [pause] "
            "<excited>Obrigada pela sintonia!</excited>"
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
            "speed": 0.95,
            "text_normalization": True,
            "output_format": {
                "codec": "mp3",
                "sample_rate": 24000,
                "bit_rate": 64000,
            },
            "replace": {
                "SearaBoom": "Seara Boom",
                "Seara Boom": "Seara Boom",
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
            "44100",
            "-c:a",
            "aac",
            "-b:a",
            "32k",
            "-profile:a",
            "aac_low",
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
            "stream=codec_name,sample_rate,channels,profile",
            "-show_entries",
            "format=duration,size",
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
    keep = {f"{c['id']}.aac" for c in CLIPS}
    for clip in CLIPS:
        mp3 = OUT_DIR / f"{clip['id']}.mp3"
        aac = OUT_DIR / f"{clip['id']}.aac"
        print(f"TTS {clip['id']} ...")
        synthesize(session, key, clip["text"], mp3)
        to_aac(mp3, aac)
        print(f"  {aac.name} {aac.stat().st_size} bytes")
        print("  " + probe(aac).replace("\n", " | "))
        mp3.unlink(missing_ok=True)
    for stale in OUT_DIR.glob("*.aac"):
        if stale.name not in keep:
            print(f"remove stale {stale.name}")
            stale.unlink()
    print("done", OUT_DIR)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)

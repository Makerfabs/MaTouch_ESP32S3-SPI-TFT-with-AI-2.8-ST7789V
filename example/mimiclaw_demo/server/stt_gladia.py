import os
import time

import requests


class GladiaError(RuntimeError):
    pass


def _extract_text(result: dict) -> str:
    transcription = result.get("result", {}).get("transcription", {})
    full_transcript = transcription.get("full_transcript")
    if full_transcript:
        return str(full_transcript).strip()

    legacy_transcription = result.get("result", {}).get("transcription")
    if isinstance(legacy_transcription, str) and legacy_transcription.strip():
        return legacy_transcription.strip()

    utterances = transcription.get("utterances") or result.get("result", {}).get("utterances")
    if utterances and isinstance(utterances, list):
        merged = " ".join(str(item.get("text", "")).strip() for item in utterances if item.get("text"))
        if merged.strip():
            return merged.strip()

    return ""


def _upload_audio(wav_bytes: bytes, headers: dict) -> str:
    last_error = None
    for _ in range(3):
        try:
            response = requests.post(
                "https://api.gladia.io/v2/upload",
                headers=headers,
                files={"audio": ("audio.wav", wav_bytes, "audio/wav")},
                timeout=120,
            )
            response.raise_for_status()
            result = response.json()
            audio_url = result.get("audio_url")
            if not audio_url:
                raise GladiaError(f"unexpected upload response: {result}")
            return audio_url
        except requests.RequestException as exc:
            last_error = exc
            time.sleep(1)
    raise GladiaError(f"gladia upload failed: {last_error}")


def _create_transcription(audio_url: str, headers: dict, gladia_url: str) -> str:
    payload = {
        "audio_url": audio_url,
        "diarization": False,
        "subtitles": False,
        "language_config": {"languages": ["zh"]},
    }
    response = requests.post(
        gladia_url,
        headers={**headers, "Content-Type": "application/json"},
        json=payload,
        timeout=120,
    )
    response.raise_for_status()
    result = response.json()
    result_url = result.get("result_url")
    if not result_url:
        raise GladiaError(f"unexpected transcription create response: {result}")
    return result_url


def _poll_result(result_url: str, headers: dict) -> str:
    last_result = None
    last_error = None
    for _ in range(30):
        try:
            response = requests.get(result_url, headers=headers, timeout=120)
            response.raise_for_status()
            result = response.json()
            last_result = result
            status = result.get("status")
            if status == "done":
                return _extract_text(result)
            if status == "error":
                raise GladiaError(f"gladia transcription error: {result}")
        except requests.RequestException as exc:
            last_error = exc
        time.sleep(1)
    if last_result is not None:
        raise GladiaError(f"gladia transcription timed out: {last_result}")
    raise GladiaError(f"gladia polling failed: {last_error}")


def speech_to_text(wav_bytes: bytes) -> str:
    gladia_api_key = os.getenv("GLADIA_API_KEY", "")
    gladia_url = os.getenv("GLADIA_URL", "https://api.gladia.io/v2/pre-recorded")

    if not gladia_api_key:
        raise GladiaError("missing GLADIA_API_KEY")

    headers = {"X-Gladia-Key": gladia_api_key}
    audio_url = _upload_audio(wav_bytes, headers)
    result_url = _create_transcription(audio_url, headers, gladia_url)
    return _poll_result(result_url, headers)

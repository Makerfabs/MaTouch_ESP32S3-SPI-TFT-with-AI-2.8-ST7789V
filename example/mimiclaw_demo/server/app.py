import os
import uuid
import wave
from pathlib import Path

from dotenv import load_dotenv
from fastapi import Body, FastAPI, File, Form, HTTPException, UploadFile
from fastapi.staticfiles import StaticFiles

env_path = Path(__file__).resolve().parent / ".env"
load_dotenv(env_path)

from llm_ark import ArkError, ask_llm
from stt_gladia import GladiaError, speech_to_text
from tts_xfyun import XfyunTtsError, text_to_speech_pcm
from utils_audio import pcm_to_wav_bytes, save_wav_file

app = FastAPI()
AUDIO_DIR = "audio"
os.makedirs(AUDIO_DIR, exist_ok=True)
app.mount("/audio", StaticFiles(directory=AUDIO_DIR), name="audio")


@app.get("/health")
def health():
    return {"ok": True}


def _normalize_input_audio(audio_bytes: bytes, audio_format: str, sample_rate: int) -> bytes:
    normalized_format = audio_format.lower()
    if normalized_format == "pcm_s16le":
        return pcm_to_wav_bytes(audio_bytes, sample_rate=sample_rate)

    if normalized_format == "wav":
        with wave.open(__import__("io").BytesIO(audio_bytes), "rb") as wav_file:
            if wav_file.getnchannels() != 1:
                raise HTTPException(status_code=400, detail="wav must be mono")
            if wav_file.getsampwidth() != 2:
                raise HTTPException(status_code=400, detail="wav must be 16-bit")
        return audio_bytes

    raise HTTPException(status_code=400, detail="unsupported format")


@app.post("/voice-chat")
async def voice_chat(
    audio: UploadFile = File(...),
    device_id: str = Form(...),
    sample_rate: int = Form(...),
    format: str = Form(...),
):
    audio_bytes = await audio.read()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="empty audio")

    try:
        wav_bytes = _normalize_input_audio(audio_bytes, format, sample_rate)
        text = speech_to_text(wav_bytes)
        reply = ask_llm(text, device_id=device_id)
        tts_pcm = text_to_speech_pcm(reply)

        wav_name = f"{uuid.uuid4().hex}.wav"
        wav_path = os.path.join(AUDIO_DIR, wav_name)
        save_wav_file(wav_path, tts_pcm, sample_rate=16000)
    except (GladiaError, ArkError, XfyunTtsError) as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    return {
        "text": text,
        "reply": reply,
        "audio_url": f"/audio/{wav_name}",
    }


@app.post("/tts")
def tts(text: str = Body(..., embed=True)):
    if not text or not text.strip():
        raise HTTPException(status_code=400, detail="empty text")

    try:
        tts_pcm = text_to_speech_pcm(text)
        wav_name = f"{uuid.uuid4().hex}.wav"
        wav_path = os.path.join(AUDIO_DIR, wav_name)
        save_wav_file(wav_path, tts_pcm, sample_rate=16000)
    except XfyunTtsError as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    return {
        "audio_url": f"/audio/{wav_name}",
    }

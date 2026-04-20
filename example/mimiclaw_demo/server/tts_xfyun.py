import base64
import hashlib
import hmac
import json
import os
from datetime import datetime
from time import mktime
from urllib.parse import urlencode
from wsgiref.handlers import format_date_time

import websocket


class XfyunTtsError(RuntimeError):
    pass


def _build_auth_url(api_key: str, api_secret: str) -> str:
    host = "tts-api.xfyun.cn"
    path = "/v2/tts"
    base_url = f"wss://{host}{path}"

    now = datetime.utcnow()
    date = format_date_time(mktime(now.timetuple()))

    signature_origin = f"host: {host}\ndate: {date}\nGET {path} HTTP/1.1"
    signature_sha = hmac.new(
        api_secret.encode("utf-8"),
        signature_origin.encode("utf-8"),
        digestmod=hashlib.sha256,
    ).digest()
    signature = base64.b64encode(signature_sha).decode("utf-8")

    authorization_origin = (
        f'api_key="{api_key}", algorithm="hmac-sha256", '
        f'headers="host date request-line", signature="{signature}"'
    )
    authorization = base64.b64encode(authorization_origin.encode("utf-8")).decode("utf-8")

    query = urlencode({
        "authorization": authorization,
        "date": date,
        "host": host,
    })
    return f"{base_url}?{query}"


def text_to_speech_pcm(text: str) -> bytes:
    xfyun_app_id = os.getenv("XFYUN_APP_ID", "")
    xfyun_api_key = os.getenv("XFYUN_API_KEY", "")
    xfyun_api_secret = os.getenv("XFYUN_API_SECRET", "")
    xfyun_vcn = os.getenv("XFYUN_VCN", "xiaoyan")

    if not xfyun_app_id:
        raise XfyunTtsError("missing XFYUN_APP_ID")
    if not xfyun_api_key:
        raise XfyunTtsError("missing XFYUN_API_KEY")
    if not xfyun_api_secret:
        raise XfyunTtsError("missing XFYUN_API_SECRET")

    ws_url = _build_auth_url(xfyun_api_key, xfyun_api_secret)
    ws = websocket.create_connection(ws_url, timeout=20)

    payload = {
        "common": {
            "app_id": xfyun_app_id,
        },
        "business": {
            "aue": "raw",
            "auf": "audio/L16;rate=16000",
            "vcn": xfyun_vcn,
            "tte": "UTF8",
            "speed": 50,
            "volume": 50,
            "pitch": 50,
        },
        "data": {
            "status": 2,
            "text": base64.b64encode(text.encode("utf-8")).decode("utf-8"),
        },
    }

    ws.send(json.dumps(payload))

    audio_chunks = []
    while True:
        message = ws.recv()
        response = json.loads(message)
        code = response.get("code", -1)
        if code != 0:
            ws.close()
            raise XfyunTtsError(f"xfyun error: {response}")

        data = response.get("data", {})
        audio_b64 = data.get("audio")
        if audio_b64:
            audio_chunks.append(base64.b64decode(audio_b64))

        if data.get("status") == 2:
            break

    ws.close()
    return b"".join(audio_chunks)

# Voice Bridge Server

## 1. 准备环境

```bash
python -m pip install -r requirements.txt
copy .env.example .env
```

填写 `.env`：

- `GLADIA_API_KEY`
- `ARK_API_KEY`
- `ARK_MODEL`
- `XFYUN_APP_ID`
- `XFYUN_API_KEY`
- `XFYUN_API_SECRET`

## 2. 启动服务

```bash
python -m uvicorn app:app --app-dir . --host 127.0.0.1 --port 8000
```

## 3. 健康检查

```bash
python -c "import requests; print(requests.get('http://127.0.0.1:8000/health').text)"
```

## 4. 本地音频测试

支持输入：
- `.wav`：16-bit mono wav
- `.pcm`：16kHz 16-bit mono 原始 PCM

```bash
python test_voice_chat.py samples/test_tone.wav
python test_voice_chat.py your_audio.wav
python test_voice_chat.py your_audio.pcm --sample-rate 16000
```

成功后返回：
- `text`
- `reply`
- `audio_url`

然后可访问返回的 `audio_url` 下载 wav。

## 5. ESP32 接入字段

`POST /voice-chat`

表单字段：
- `audio`
- `device_id`
- `sample_rate`
- `format`，取值 `pcm_s16le`

返回：

```json
{
  "text": "...",
  "reply": "...",
  "audio_url": "/audio/xxxx.wav"
}
```

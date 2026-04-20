import argparse
from pathlib import Path

import requests


def detect_format(audio_path: Path) -> str:
    suffix = audio_path.suffix.lower()
    if suffix == ".wav":
        return "wav"
    if suffix == ".pcm":
        return "pcm_s16le"
    raise ValueError("only .wav or .pcm are supported")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", help="path to local wav or pcm file")
    parser.add_argument("--url", default="http://127.0.0.1:8000/voice-chat")
    parser.add_argument("--device-id", default="esp32s3-local-test")
    parser.add_argument("--sample-rate", type=int, default=16000)
    args = parser.parse_args()

    audio_path = Path(args.audio)
    audio_format = detect_format(audio_path)
    with audio_path.open("rb") as f:
        response = requests.post(
            args.url,
            files={"audio": (audio_path.name, f, "application/octet-stream")},
            data={
                "device_id": args.device_id,
                "sample_rate": str(args.sample_rate),
                "format": audio_format,
            },
            timeout=180,
        )

    print("status:", response.status_code)
    print(response.text)


if __name__ == "__main__":
    main()

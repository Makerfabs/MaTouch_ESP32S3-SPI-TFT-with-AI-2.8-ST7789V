import os
from pathlib import Path
from dotenv import load_dotenv


env_path = Path(__file__).resolve().parent / ".env"
load_dotenv(env_path)

keys = [
    "GLADIA_API_KEY",
    "ARK_API_KEY",
    "ARK_MODEL",
    "XFYUN_APP_ID",
    "XFYUN_API_KEY",
    "XFYUN_API_SECRET",
]

for key in keys:
    print(f"{key}: {'OK' if os.getenv(key) else 'MISSING'}")

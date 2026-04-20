import os
from openai import OpenAI


class ArkError(RuntimeError):
    pass


def ask_llm(text: str, device_id: str) -> str:
    if not text.strip():
        return "我没有听清，请再说一次。"

    ark_api_key = os.getenv("ARK_API_KEY", "")
    ark_base_url = os.getenv("ARK_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3")
    ark_model = os.getenv("ARK_MODEL", "")

    if not ark_api_key:
        raise ArkError("missing ARK_API_KEY")
    if not ark_model:
        raise ArkError("missing ARK_MODEL")

    client = OpenAI(api_key=ark_api_key, base_url=ark_base_url)
    completion = client.chat.completions.create(
        model=ark_model,
        messages=[
            {
                "role": "system",
                "content": "你是一个中文语音助手。回答要自然、简洁、适合直接播报，控制在60字以内。"
            },
            {
                "role": "user",
                "content": text
            }
        ],
        temperature=0.7,
        max_tokens=200,
    )

    content = completion.choices[0].message.content
    if not content:
        raise ArkError("empty Ark reply")
    return content.strip()

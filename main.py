import base64
import os
import time

import requests
from fastapi import FastAPI, HTTPException
from openai import OpenAI

CAMERA_URL = "http://192.168.100.5:8080/shot.jpg"

client = OpenAI(api_key=os.getenv("OPENAI_API_KEY"))
app = FastAPI()

COLOR_CODES = {"white": 0, "black": 1, "red": 2, "green": 3}

last_result = {"code": None, "color": None, "timestamp": None}


def get_snapshot():
    response = requests.get(CAMERA_URL, timeout=5)
    response.raise_for_status()
    return base64.b64encode(response.content).decode("utf-8")


def detect_dominant_color(image_b64):
    response = client.chat.completions.create(
        model="gpt-4o",
        messages=[
            {
                "role": "user",
                "content": [
                    {
                        "type": "text",
                        "text": "What is the dominant color in this image? Reply with only the color name in one word.",
                    },
                    {
                        "type": "image_url",
                        "image_url": {
                            "url": f"data:image/jpeg;base64,{image_b64}",
                            "detail": "low",
                        },
                    },
                ],
            }
        ],
        max_tokens=20,
    )
    return response.choices[0].message.content.strip().lower().rstrip(".")


@app.get("/color")
def get_color():
    try:
        image_b64 = get_snapshot()
        color = detect_dominant_color(image_b64)
        last_result["code"] = COLOR_CODES.get(color, -1)
        last_result["color"] = color
        last_result["timestamp"] = time.strftime("%Y-%m-%d %H:%M:%S")
        return last_result
    except requests.exceptions.RequestException:
        raise HTTPException(status_code=502, detail="Camera unavailable")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/last")
def get_last():
    if last_result["color"] is None:
        raise HTTPException(status_code=404, detail="No color detected yet")
    return last_result

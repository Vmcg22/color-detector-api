import base64
import io
import os
import time

import requests
from fastapi import FastAPI, HTTPException
from openai import OpenAI
from PIL import Image

# La imagen se divide en una cuadrícula 3x3 y solo se envía la fila inferior
# (los 3 recuadros de abajo): ancho completo, altura = 1/3 inferior.
# Esto enfoca el análisis en lo que está justo frente al robot.
GRID_ROWS = 3
GRID_COLS = 3

CAMERA_URL_FLOOR = os.getenv("CAMERA_URL_FLOOR", "http://192.168.100.7:8080/snapshot.jpg")
CAMERA_URL_WOOD = os.getenv("CAMERA_URL_WOOD", "http://192.168.100.8:8080/snapshot.jpg")

client = OpenAI(api_key=os.getenv("OPENAI_API_KEY"))
app = FastAPI()

COLOR_CODES = {"white": 0, "black": 1, "red": 2, "green": 3}

last_result = {"code": None, "color": None, "timestamp": None}
last_result_wood = {"code": None, "color": None, "timestamp": None}


def get_snapshot(camera_url):
    response = requests.get(camera_url, timeout=5)
    response.raise_for_status()
    return base64.b64encode(_crop_bottom_row(response.content)).decode("utf-8")


def _crop_bottom_row(image_bytes: bytes) -> bytes:
    """Recorta la fila inferior de una cuadrícula 3x3 (ancho completo,
    altura = 1/GRID_ROWS inferior). Eso es lo que mira el robot justo delante."""
    image = Image.open(io.BytesIO(image_bytes))
    width, height = image.size
    row_h = height // GRID_ROWS
    top = height - row_h
    cropped = image.crop((0, top, width, height))

    out = io.BytesIO()
    fmt = image.format or "JPEG"
    cropped.convert("RGB").save(out, format=fmt)
    return out.getvalue()


def detect_dominant_color(image_b64):
    response = client.chat.completions.create(
        model="gpt-4o",
        messages=[
            {
                "role": "user",
                "content": [
                    {
                        "type": "text",
                        "text": (
                            "You are a vision system for a robot that collects colored cylinders from a white arena. "
                            "The image has already been cropped: it shows only the bottom row of a 3x3 grid taken from the camera view. "
                            "This strip represents what is directly in front of the robot at floor level. "
                            "Only report a cylinder color if the cylinder occupies most of this strip (clearly centered and filling the frame). "
                            "A cylinder that only peeks in from the top edge, or appears tiny/far away, does NOT count; answer 'white' instead. "
                            "The answer must be exactly one of: white, black, red, green. "
                            "If you only see the white arena floor or no cylinder is clearly present, answer 'white'. "
                            "Reply with only the color name, lowercase, no punctuation."
                        ),
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


def detect_color_on_wood(image_b64):
    response = client.chat.completions.create(
        model="gpt-4o",
        messages=[
            {
                "role": "user",
                "content": [
                    {
                        "type": "text",
                        "text": (
                            "You are a vision system for a robot that collects colored cylinders from a wooden arena. "
                            "The arena surface is light wood (cream/brown grain) and a black tape stripe runs across it. "
                            "The image has already been cropped: it shows only the bottom row of a 3x3 grid. "
                            "This strip represents what is directly in front of the robot. "
                            "Only report a cylinder color if the cylinder occupies most of this strip (clearly centered and filling the frame). "
                            "A cylinder that only peeks in from the top edge, or appears tiny/far away, does NOT count; answer 'white' instead. "
                            "If you see a dark/black circle on top of the black stripe, that IS a black cylinder (do not confuse it with the stripe itself). "
                            "The answer must be exactly one of: white, black, red, green. "
                            "If no cylinder is clearly present, answer 'white'. "
                            "Reply with only the color name, lowercase, no punctuation."
                        ),
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
        image_b64 = get_snapshot(CAMERA_URL_FLOOR)
        color = detect_dominant_color(image_b64)
        last_result["code"] = COLOR_CODES.get(color, -1)
        last_result["color"] = color
        last_result["timestamp"] = time.strftime("%Y-%m-%d %H:%M:%S")
        return last_result
    except requests.exceptions.RequestException:
        raise HTTPException(status_code=502, detail="Camera unavailable")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/color-wood")
def get_color_wood():
    try:
        image_b64 = get_snapshot(CAMERA_URL_WOOD)
        color = detect_color_on_wood(image_b64)
        last_result_wood["code"] = COLOR_CODES.get(color, -1)
        last_result_wood["color"] = color
        last_result_wood["timestamp"] = time.strftime("%Y-%m-%d %H:%M:%S")
        return last_result_wood
    except requests.exceptions.RequestException:
        raise HTTPException(status_code=502, detail="Camera unavailable")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/last")
def get_last():
    if last_result["color"] is None:
        raise HTTPException(status_code=404, detail="No color detected yet")
    return last_result


@app.get("/last-wood")
def get_last_wood():
    if last_result_wood["color"] is None:
        raise HTTPException(status_code=404, detail="No color detected yet")
    return last_result_wood

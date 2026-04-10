# Color Detector API

API que detecta el color dominante de una imagen capturada desde una cámara IP usando GPT-4o.

## Requisitos

- Docker y Docker Compose
- Cámara IP con endpoint de snapshot (ej. IP Webcam en Android)
- API key de OpenAI

## Configuración

Crear un archivo `.env` en la raíz del proyecto:

```
OPENAI_API_KEY=tu_api_key_aqui
```

La URL de la cámara está configurada en `main.py`:

```
CAMERA_URL = "http://192.168.100.5:8080/shot.jpg"
```

## Levantar el servicio con Docker

Construir e iniciar el contenedor en segundo plano:

```bash
docker compose up -d --build
```

Ver los logs en tiempo real:

```bash
docker compose logs -f
```

Detener el servicio:

```bash
docker compose down
```

Reconstruir tras cambios en el código:

```bash
docker compose up -d --build --force-recreate
```

Una vez levantado, la API queda disponible en `http://localhost:8001`.

## Endpoints

### `GET /color`

Toma una foto de la cámara, la envía a GPT-4o y retorna el color dominante.

**Respuesta:**

```json
{
  "color": "blue",
  "timestamp": "2026-04-09 15:30:00"
}
```

**Errores:**

- `502` — Cámara no disponible
- `500` — Error interno

### `GET /last`

Retorna el último color detectado sin hacer una nueva llamada a la API (ahorra tokens).

**Respuesta:**

```json
{
  "color": "blue",
  "timestamp": "2026-04-09 15:30:00"
}
```

**Errores:**

- `404` — Aún no se ha detectado ningún color

## Ejemplo de uso desde Arduino (ESP32/ESP8266)

```cpp
#include <HTTPClient.h>
#include <ArduinoJson.h>

HTTPClient http;
http.begin("http://TU_IP:8001/color");
int code = http.GET();

if (code == 200) {
  JsonDocument doc;
  deserializeJson(doc, http.getString());
  const char* color = doc["color"];
}
http.end();
```

## Puerto

El servicio corre en el puerto **8001** (mapeado internamente al 8000).

```
http://localhost:8001/color
http://localhost:8001/last
```
# color-detector-api

#include <AccelStepper.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

// ============================================================
//  v2 — Steppers + sweep de servos
//  Solo cámara de abajo (/color). La madera se añade en otra versión.
// ============================================================

// --- WiFi y API ---
const char* ssid     = "Totalplay-2.4G-46c8";
const char* password = "CZMkJwFYR4fj7hs5";
const char* urlFloor = "http://192.168.100.28:8001/color";

// ============================================================
//  Motores a pasos (28BYJ-48 + ULN2003, modo FULL4WIRE)
// ============================================================
#define M1_IN1 15
#define M1_IN2 2
#define M1_IN3 4
#define M1_IN4 16

#define M2_IN1 5
#define M2_IN2 18
#define M2_IN3 19
#define M2_IN4 21

const float pasosPorCm = 81.5;
AccelStepper motor1(4, M1_IN1, M1_IN3, M1_IN2, M1_IN4);
AccelStepper motor2(4, M2_IN1, M2_IN3, M2_IN2, M2_IN4);

// ============================================================
//  Servos
// ============================================================
// Mapeo (mismo criterio del simulador verificado):
//   misServos[0] -> s1: pinza  ROJA  piso       (pin 26)
//   misServos[1] -> s2: pinza  NEGRA piso       (pin 27)
//   misServos[2] -> s3: compuerta ROJA  madera  (pin 25)  [no se usa en esta versión]
//   misServos[3] -> s4: compuerta NEGRA madera  (pin 14)  [no se usa en esta versión]
//   misServos[4] -> s5: empujador madera        (pin 13)  [no se usa en esta versión]
Servo misServos[5];
const int pinesServos[] = {26, 27, 25, 14, 13};

// Ángulos copiados del sketch recibido. Las pinzas usan barrido lento
// (sweepServo) para empujar el cilindro sin lanzarlo.
const int S1_REPOSO = 180;   // pinza ROJA  arranca a 180
const int S1_EMPUJE = 0;     // barre 180 -> 0 (lento), luego vuelve a 180
const int S2_REPOSO = 0;     // pinza NEGRA arranca a 0 (espejada de S1)
const int S2_EMPUJE = 180;   // barre 0 -> 180 (lento), luego vuelve a 0

// Reposos para los que todavía no usamos pero ya están cableados
const int S3_REPOSO = 180;   // compuerta ROJA  cerrada
const int S4_REPOSO = 0;     // compuerta NEGRA cerrada
const int S5_REPOSO = 0;     // empujador quieto

const int SWEEP_DELAY_MS = 15;   // ms por grado durante el barrido
const int PAUSA_PINZA_MS = 500;  // pausa entre fases del movimiento

// ============================================================
//  Parámetros del recorrido
// ============================================================
const float CM_POR_PASO_LINEA   = 2.0;   // distancia de cada "paso" de línea (calibrar)
const int   ESTABILIZAR_MS      = 300;   // pausa tras parar antes de consultar cámara
const int   MAX_PASOS_POR_LINEA = 60;    // tope de seguridad por línea
const int   PASOS_BLANCOS_FIN   = 6;     // fin: N blancos seguidos tras haber visto algo

// ============================================================
//  Estado
// ============================================================
int idFloor = -1;

// ============================================================
//  Declaraciones
// ============================================================
void conectarWifi();
bool consultar(const char* url, int &idOut);
const char* nombreColor(int id);
void avanzarCm(float cm);
void esperarMotores();
void pasoAdelante();
void recolectarPiso(int id);
void recorrerLinea();
void sweepServo(int idx, int desde, int hasta, int delayMs);

// ============================================================
//  setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // --- Motores a pasos ---
  motor1.setMaxSpeed(500);
  motor1.setAcceleration(200);
  motor2.setMaxSpeed(500);
  motor2.setAcceleration(200);

  // --- Servos ---
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  for (int i = 0; i < 5; i++) {
    misServos[i].setPeriodHertz(50);
    misServos[i].attach(pinesServos[i], 500, 2400);
  }

  // Cada servo en su reposo propio (NO todos a 0 como el sketch original)
  misServos[0].write(S1_REPOSO);
  misServos[1].write(S2_REPOSO);
  misServos[2].write(S3_REPOSO);
  misServos[3].write(S4_REPOSO);
  misServos[4].write(S5_REPOSO);
  delay(500);

  // --- WiFi ---
  conectarWifi();

  Serial.println();
  Serial.println("============================================================");
  Serial.println("  MODO REACTIVO (v2, solo camara de abajo)");
  Serial.println("  Reglas:");
  Serial.println("    white  -> avanzar");
  Serial.println("    red    -> detener + activar s1 (pinza ROJA, sweep 180->0)");
  Serial.println("    black  -> detener + activar s2 (pinza NEGRA, sweep 0->180)");
  Serial.println("    green  -> detener sin activar ningun servo");
  Serial.println("============================================================");

  recorrerLinea();

  Serial.println();
  Serial.println("============================================================");
  Serial.println("  RUTINA TERMINADA");
  Serial.println("============================================================");
}

void loop() {
  // vacío: todo corre una vez en setup()
}

// ============================================================
//  Recorrido de línea
// ============================================================
// Avanza paso a paso consultando /color en cada paso:
//   - white  -> sigue avanzando
//   - red    -> activa s1
//   - black  -> activa s2
//   - green  -> se detiene pero no toca servos
// Termina tras PASOS_BLANCOS_FIN blancos seguidos después de haber detectado
// al menos un cilindro. También corta por MAX_PASOS_POR_LINEA.
void recorrerLinea() {
  int blancosSeguidos = 0;
  bool vioAlgo = false;

  Serial.printf("[LINEA] inicio recorrido, tope=%d pasos, fin=%d blancos seguidos\n",
                MAX_PASOS_POR_LINEA, PASOS_BLANCOS_FIN);

  for (int i = 0; i < MAX_PASOS_POR_LINEA; i++) {
    pasoAdelante();
    delay(ESTABILIZAR_MS);

    if (!consultar(urlFloor, idFloor)) {
      Serial.printf("[LINEA] paso %d  ERROR /color, reintento\n", i);
      delay(300);
      continue;
    }
    Serial.printf("[LINEA] paso %d  floor=%d (%s)\n",
                  i, idFloor, nombreColor(idFloor));

    if (idFloor == 0) {
      if (vioAlgo) {
        blancosSeguidos++;
        Serial.printf("[LINEA]   blancos seguidos=%d / %d\n",
                      blancosSeguidos, PASOS_BLANCOS_FIN);
        if (blancosSeguidos >= PASOS_BLANCOS_FIN) {
          Serial.println("[LINEA] fin de línea detectado.");
          return;
        }
      }
      continue;
    }

    // floor != blanco (red, black o green)
    blancosSeguidos = 0;
    vioAlgo = true;

    Serial.println("  ---- decisión ----");
    Serial.printf("    Piso = %d (%s) -> %s\n",
                  idFloor, nombreColor(idFloor),
                  (idFloor == 1 || idFloor == 2) ? "RECOLECTAR" : "IGNORAR");
    Serial.println("  ------------------");

    recolectarPiso(idFloor);
  }

  Serial.println("[LINEA] MAX_PASOS_POR_LINEA alcanzado sin fin explícito.");
}

// ============================================================
//  Movimiento de los steppers
// ============================================================
void pasoAdelante() {
  avanzarCm(CM_POR_PASO_LINEA);
  esperarMotores();
}

// Movimiento relativo: suma al target actual (no usa moveTo absoluto)
void avanzarCm(float cm) {
  long pasosAdicionales = (long)(cm * pasosPorCm);
  motor1.move(pasosAdicionales);
  motor2.move(pasosAdicionales);
}

void esperarMotores() {
  while (motor1.distanceToGo() != 0 || motor2.distanceToGo() != 0) {
    motor1.run();
    motor2.run();
  }
}

// ============================================================
//  Recolección en piso (sweep lento estilo del sketch recibido)
// ============================================================
// id: 1 = negro -> misServos[1] (s2, sweep 0 -> 180)
//     2 = rojo  -> misServos[0] (s1, sweep 180 -> 0)
//     otro (0 white, 3 green, -1 error) -> nada
void recolectarPiso(int id) {
  Serial.println("[PISO] recolectarPiso() iniciado");
  Serial.printf("[PISO]   idFloor=%d (%s)\n", id, nombreColor(id));

  if (id == 1) {
    Serial.println("[PISO]   >>> ACTIVANDO s2 (pinza NEGRA)");
    sweepServo(1, S2_REPOSO, S2_EMPUJE, SWEEP_DELAY_MS);
    delay(PAUSA_PINZA_MS);
    misServos[1].write(S2_REPOSO);
    delay(PAUSA_PINZA_MS);
    Serial.println("[PISO]   <<< s2 completado");
  } else if (id == 2) {
    Serial.println("[PISO]   >>> ACTIVANDO s1 (pinza ROJA)");
    sweepServo(0, S1_REPOSO, S1_EMPUJE, SWEEP_DELAY_MS);
    delay(PAUSA_PINZA_MS);
    misServos[0].write(S1_REPOSO);
    delay(PAUSA_PINZA_MS);
    Serial.println("[PISO]   <<< s1 completado");
  } else {
    Serial.printf("[PISO]   -> SALTADO (color %s no requiere recolección)\n",
                  nombreColor(id));
    Serial.println("[PISO]   servos NO activados: s1=OFF, s2=OFF");
  }
}

// Barre un servo grado por grado, pausando delayMs entre cada grado.
// Así el cilindro se empuja con suavidad (en vez de ser lanzado).
void sweepServo(int idx, int desde, int hasta, int delayMs) {
  if (desde == hasta) return;
  if (desde < hasta) {
    for (int pos = desde; pos <= hasta; pos++) {
      misServos[idx].write(pos);
      delay(delayMs);
    }
  } else {
    for (int pos = desde; pos >= hasta; pos--) {
      misServos[idx].write(pos);
      delay(delayMs);
    }
  }
}

// ============================================================
//  Utilidades
// ============================================================
const char* nombreColor(int id) {
  switch (id) {
    case 0:  return "white";
    case 1:  return "black";
    case 2:  return "red";
    case 3:  return "green";
    default: return "desconocido";
  }
}

// ============================================================
//  HTTP
// ============================================================
bool consultar(const char* url, int &idOut) {
  idOut = -1;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi caído, reintentando conexión...");
    conectarWifi();
    if (WiFi.status() != WL_CONNECTED) return false;
  }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);
  int httpCode = http.GET();

  bool ok = false;
  if (httpCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<200> doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok) {
      idOut = doc["code"] | -1;
      ok = (idOut != -1);
    }
  } else {
    Serial.printf("HTTP %s -> error: %s\n", url, http.errorToString(httpCode).c_str());
  }
  http.end();
  return ok;
}

// ============================================================
//  WiFi
// ============================================================
void conectarWifi() {
  WiFi.disconnect(true, true); delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_protocol(WIFI_IF_STA,
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  Serial.print("Conectando a "); Serial.println(ssid);
  WiFi.begin(ssid, password);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("No se pudo conectar a WiFi.");
  }
}

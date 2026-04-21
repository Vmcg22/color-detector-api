#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

// ============================================================
//  Configuración
// ============================================================
const char* ssid     = "INFINITUM88AE";
const char* password = "hPCuC2CKZg";

// IP de la PC corriendo docker (puerto 8001).
// /color      -> cámara de abajo (piso blanco)
// /color-wood -> cámara de arriba (repisa de madera con franja negra)
const char* urlFloor = "http://192.168.1.187:8001/color";
const char* urlWood  = "http://192.168.1.187:8001/color-wood";

// --- Motores (puente H L298N) ---
// IN1/IN2 controlan dirección motor A (izquierdo)
// IN3/IN4 controlan dirección motor B (derecho)
// ENA/ENB regulan velocidad por PWM (quita los jumpers del L298N).
const int IN1 = 1;  const int IN2 = 2;  const int ENA = 40;   // motor A (izquierdo)
const int IN3 = 42; const int IN4 = 41; const int ENB = 39;   // motor B (derecho)

// Configuración PWM para ENA/ENB (API LEDC viejo, core ESP32 2.x)
const int freq       = 5000;   // 5 kHz
const int resolution = 8;      // 8 bits -> 0..255
const int CANAL_A    = 4;      // canal LEDC para motor A (evita colisión con servos 0-3)
const int CANAL_B    = 5;      // canal LEDC para motor B

// Velocidades (0-255). Ajusta para que el robot avance recto:
// si tira a la izquierda -> sube velocidadIzq o baja velocidadDer.
int velocidadIzq = 220;
int velocidadDer = 255;

// --- Servos ---
// s1: pinza que empuja cilindros ROJOS  del piso (cámara de abajo)
// s2: pinza que empuja cilindros NEGROS del piso (cámara de abajo)
// s3: compuerta del almacén ROJO  (se abre cuando llega un rojo de la madera)
// s4: compuerta del almacén NEGRO (se abre cuando llega un negro de la madera)
// s5: empujador que lanza el cilindro de la madera hacia la compuerta abierta
Servo s1, s2, s3, s4, s5;
const int pinS1 = 7;   // pinza ROJO     (piso)
const int pinS2 = 6;   // pinza NEGRO    (piso)
const int pinS3 = 4;   // compuerta ROJO
const int pinS4 = 15;  // compuerta NEGRO
const int pinS5 = 5;   // empujador

// Ángulos (ajusta con tu mecánica real)
const int PINZA_REPOSO      = 0;
const int PINZA_EMPUJE      = 90;
const int COMPUERTA_CERRADA = 0;
const int COMPUERTA_ABIERTA = 90;
const int EMPUJADOR_REPOSO  = 0;
const int EMPUJADOR_EMPUJE  = 90;

// --- Parámetros de movimiento ---
const int PASO_MS             = 30;    // avance por iteración
const int ESTABILIZAR_MS      = 300;   // pausa tras frenar antes de consultar cámara
const int MAX_PASOS_POR_LINEA = 60;    // tope de seguridad por línea
const int PASOS_BLANCOS_FIN   = 6;     // fin de línea: N pasos seguidos en blanco tras haber detectado algo

// Color objetivo durante la prueba de "avanzar hasta encontrar":
// 0=white, 1=black, 2=red, 3=green  — 2 = rojo
const int COLOR_OBJETIVO      = 2;

// --- Estado ---
int idFloor = -1;
int idWood  = -1;

// ============================================================
//  Declaraciones
// ============================================================
void conectarWifi();
void adelante(); void atras(); void izquierda(); void derecha(); void detener();
void aplicarVelocidad();
void pasoAdelante();
bool consultar(const char* url, int &idOut);
void recolectarPiso(int id);
void recolectarMadera(int id);
bool avanzarHastaColor(int colorObjetivo);
const char* nombreColor(int id);
void recorrerLinea();
void irADeposito();
void descargar();
void volverALineaDos();

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  // PWM para ENA y ENB (control de velocidad de cada motor)
  ledcSetup(CANAL_A, freq, resolution);
  ledcSetup(CANAL_B, freq, resolution);
  ledcAttachPin(ENA, CANAL_A);
  ledcAttachPin(ENB, CANAL_B);

  detener();
  delay(500);

  conectarWifi();

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2); ESP32PWM::allocateTimer(3);
  s1.setPeriodHertz(50); s2.setPeriodHertz(50);
  s3.setPeriodHertz(50); s4.setPeriodHertz(50); s5.setPeriodHertz(50);
  s1.attach(pinS1, 500, 2400);
  s2.attach(pinS2, 500, 2400);
  s3.attach(pinS3, 500, 2400);
  s4.attach(pinS4, 500, 2400);
  s5.attach(pinS5, 500, 2400);

  // Posiciones iniciales
  s1.write(PINZA_REPOSO);
  s2.write(PINZA_REPOSO);
  s3.write(COMPUERTA_CERRADA);
  s4.write(COMPUERTA_CERRADA);
  s5.write(EMPUJADOR_REPOSO);
  delay(500);

  // ---------- MODO REACTIVO ----------
  // Recorre la línea en línea recta (sin giros). En cada paso consulta /color.
  //  - white  -> sigue avanzando
  //  - red/black -> se detiene, consulta /color-wood y recolecta según toque
  //  - green  -> se detiene, consulta /color-wood, pero NO activa pinza piso
  // La madera sigue la misma regla (green = no activar).
  // Termina después de PASOS_BLANCOS_FIN pasos en blanco seguidos (línea vacía).
  Serial.println();
  Serial.println("============================================================");
  Serial.println("  MODO REACTIVO: recorrer línea y recolectar según color");
  Serial.println("  Reglas:");
  Serial.println("    white  -> avanzar");
  Serial.println("    red    -> recolectar (s1 piso / s3+s5 madera)");
  Serial.println("    black  -> recolectar (s2 piso / s4+s5 madera)");
  Serial.println("    green  -> ignorar servo correspondiente");
  Serial.println("============================================================");

  recorrerLinea();

  detener();
  Serial.println();
  Serial.println("============================================================");
  Serial.println("  RUTINA TERMINADA");
  Serial.println("============================================================");
}

void loop() {
  // vacío: todo corre una vez en setup()
}

// ============================================================
//  Lógica principal
// ============================================================

// Recorre una línea: avanza paso a paso, consulta cámara de abajo.
//  - Si ve blanco, sigue avanzando.
//  - Si ve red/black/green, se detiene, consulta cámara de arriba y
//    recolecta lo que corresponda en cada pinza (green => no hacer nada).
// La línea termina cuando acumula PASOS_BLANCOS_FIN pasos en blanco
// seguidos, después de haber detectado al menos un cilindro.
// También corta por MAX_PASOS_POR_LINEA como tope de seguridad.
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

    // floor detectó algo != white
    blancosSeguidos = 0;
    vioAlgo = true;

    Serial.println("[LINEA] consultando cámara de madera...");
    if (!consultar(urlWood, idWood)) {
      Serial.println("[LINEA]   ERROR /color-wood, idWood=-1");
      idWood = -1;
    } else {
      Serial.printf("[LINEA]   wood=%d (%s)\n", idWood, nombreColor(idWood));
    }

    // Resumen de decisiones antes de actuar
    Serial.println("  ---- decisiones ----");
    Serial.printf("    Piso  = %d (%s) -> %s\n",
                  idFloor, nombreColor(idFloor),
                  (idFloor == 1 || idFloor == 2) ? "RECOLECTAR" : "IGNORAR");
    Serial.printf("    Madera= %d (%s) -> %s\n",
                  idWood, nombreColor(idWood),
                  (idWood == 1 || idWood == 2) ? "RECOLECTAR" : "IGNORAR");
    Serial.println("  --------------------");

    // Las funciones ya loguean "SALTADO" si el color no aplica.
    recolectarPiso(idFloor);
    recolectarMadera(idWood);
  }

  Serial.println("[LINEA] MAX_PASOS_POR_LINEA alcanzado sin fin explícito.");
}

void pasoAdelante() {
  adelante();
  delay(PASO_MS);
  detener();
}

// Avanza paso a paso y devuelve true cuando la cámara de abajo reporta
// colorObjetivo (1=black, 2=red). Cualquier otro color (blanco, verde,
// el que no sea objetivo) se ignora y sigue avanzando. Devuelve false si
// agota MAX_PASOS_POR_LINEA sin encontrarlo.
bool avanzarHastaColor(int colorObjetivo) {
  Serial.printf("[BUSCA] Objetivo = %d (%s), tope = %d pasos\n",
                colorObjetivo, nombreColor(colorObjetivo), MAX_PASOS_POR_LINEA);
  for (int i = 0; i < MAX_PASOS_POR_LINEA; i++) {
    pasoAdelante();
    delay(ESTABILIZAR_MS);

    if (!consultar(urlFloor, idFloor)) {
      Serial.printf("[BUSCA] paso %d  ERROR consulta /color, reintento\n", i);
      delay(300);
      continue;
    }
    Serial.printf("[BUSCA] paso %d  floor=%d (%s)\n",
                  i, idFloor, nombreColor(idFloor));

    if (idFloor == colorObjetivo) {
      Serial.printf("[BUSCA] objetivo alcanzado en paso %d\n", i);
      return true;
    }
  }
  return false;
}

// ============================================================
//  Pinzas (AJUSTA según tu mecánica)
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

// id: 1 = negro -> s2 ; 2 = rojo -> s1 ; cualquier otro (green, white) -> nada
void recolectarPiso(int id) {
  Serial.println("[PISO] recolectarPiso() iniciado");
  Serial.printf("[PISO]   idFloor=%d (%s)\n", id, nombreColor(id));

  Servo* pinza = nullptr;
  const char* etiqueta = "";
  int canal = 0;
  if (id == 1)      { pinza = &s2; etiqueta = "NEGRO"; canal = 2; }
  else if (id == 2) { pinza = &s1; etiqueta = "ROJO";  canal = 1; }
  else {
    Serial.printf("[PISO]   -> SALTADO (color %s no requiere recolección)\n",
                  nombreColor(id));
    Serial.printf("[PISO]   servos NO activados: s1=OFF, s2=OFF\n");
    return;
  }

  Serial.printf("[PISO]   >>> ACTIVANDO servo s%d (pinza %s)\n", canal, etiqueta);
  Serial.printf("[PISO]       s%d.write(%d) (empuje)\n", canal, PINZA_EMPUJE);
  pinza->write(PINZA_EMPUJE);
  delay(600);
  Serial.printf("[PISO]       s%d.write(%d) (reposo)\n", canal, PINZA_REPOSO);
  pinza->write(PINZA_REPOSO);
  delay(400);
  Serial.printf("[PISO]   <<< servo s%d completado\n", canal);
}

// id: 1 = negro -> s5 empuja, 1s después abre s4 ; 2 = rojo -> s5 empuja, 1s después abre s3
// Orden nuevo: primero empuja el cilindro con s5, espera 1 segundo y luego
// abre la compuerta correspondiente para que caiga al almacén correcto.
void recolectarMadera(int id) {
  Serial.println("[MADERA] recolectarMadera() iniciado");
  Serial.printf("[MADERA]   idWood=%d (%s)\n", id, nombreColor(id));

  Servo* compuerta = nullptr;
  const char* etiqueta = "";
  int canalCompuerta = 0;
  if (id == 1)      { compuerta = &s4; etiqueta = "NEGRO"; canalCompuerta = 4; }
  else if (id == 2) { compuerta = &s3; etiqueta = "ROJO";  canalCompuerta = 3; }
  else {
    Serial.printf("[MADERA]   -> SALTADO (color %s no requiere recolección)\n",
                  nombreColor(id));
    Serial.println("[MADERA]   servos NO activados: s3=OFF, s4=OFF, s5=OFF");
    return;
  }

  Serial.printf("[MADERA]   >>> empujador s5 + compuerta s%d (%s)\n",
                canalCompuerta, etiqueta);

  // 1. Empuja primero
  Serial.printf("[MADERA]       s5.write(%d) (empuje)\n", EMPUJADOR_EMPUJE);
  s5.write(EMPUJADOR_EMPUJE);
  delay(1000);  // espera 1 segundo antes de abrir la compuerta

  // 2. Abre la compuerta
  Serial.printf("[MADERA]       s%d.write(%d) (abrir compuerta)\n",
                canalCompuerta, COMPUERTA_ABIERTA);
  compuerta->write(COMPUERTA_ABIERTA);
  delay(400);

  // 3. Cierra la compuerta
  Serial.printf("[MADERA]       s%d.write(%d) (cerrar compuerta)\n",
                canalCompuerta, COMPUERTA_CERRADA);
  compuerta->write(COMPUERTA_CERRADA);
  delay(400);

  // 4. Regresa el empujador a reposo
  Serial.printf("[MADERA]       s5.write(%d) (reposo)\n", EMPUJADOR_REPOSO);
  s5.write(EMPUJADOR_REPOSO);
  delay(300);

  Serial.printf("[MADERA]   <<< empujador s5 + compuerta s%d completado\n",
                canalCompuerta);
}

// ============================================================
//  Navegación entre fases (STUBS por tiempo: calibra con tu arena)
// ============================================================
void irADeposito() {
  detener();            delay(300);
  izquierda();          delay(700); detener(); delay(200);
  adelante();           delay(1500); detener(); delay(300);
}

void descargar() {
  // Mecanismo de descarga aún no definido.
  Serial.println("  -> descargando cilindros (pendiente)");
  delay(500);
}

void volverALineaDos() {
  atras();              delay(1500); detener(); delay(300);
  derecha();            delay(700);  detener(); delay(200);
  adelante();           delay(1000); detener(); delay(300);
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

// ============================================================
//  Motores (con control de velocidad PWM en ENA/ENB)
// ============================================================
void aplicarVelocidad() {
  ledcWrite(CANAL_A, velocidadIzq);
  ledcWrite(CANAL_B, velocidadDer);
}

void adelante()  {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  aplicarVelocidad();
}

void atras() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  aplicarVelocidad();
}

void izquierda() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  aplicarVelocidad();
}

void derecha() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  aplicarVelocidad();
}

void detener() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  ledcWrite(CANAL_A, 0);
  ledcWrite(CANAL_B, 0);
}

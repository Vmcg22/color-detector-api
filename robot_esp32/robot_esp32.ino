#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

// ============================================================
//  Configuración
// ============================================================
const char* ssid     = "RedMia";
const char* password = "12345678";

// IP de la PC corriendo docker (puerto 8001).
// /color      -> cámara de abajo (piso blanco)
// /color-wood -> cámara de arriba (repisa de madera con franja negra)
const char* urlFloor = "http://10.15.71.100:8001/color";
const char* urlWood  = "http://10.15.71.100:8001/color-wood";

// --- Motores (puente H) ---
const int IN1 = 1;  const int IN2 = 2;
const int IN3 = 42; const int IN4 = 41;

// --- Servos ---
// s1: pinza que empuja cilindros NEGROS del piso (cámara de abajo)
// s2: pinza que empuja cilindros ROJOS del piso (cámara de abajo)
// s3: compuerta del almacén ROJO (se abre cuando llega un rojo de la madera)
// s4: compuerta del almacén NEGRO (se abre cuando llega un negro de la madera)
// s5: empujador que lanza el cilindro de la madera hacia la compuerta abierta
Servo s1, s2, s3, s4, s5;
const int pinS1 = 4;   // pinza negro (piso)
const int pinS2 = 5;   // pinza rojo  (piso)
const int pinS3 = 6;   // compuerta rojo
const int pinS4 = 7;   // compuerta negro
const int pinS5 = 8;   // empujador arriba

// Ángulos (ajusta con tu mecánica real)
const int PINZA_REPOSO      = 0;
const int PINZA_EMPUJE      = 90;
const int COMPUERTA_CERRADA = 0;
const int COMPUERTA_ABIERTA = 90;
const int EMPUJADOR_REPOSO  = 0;
const int EMPUJADOR_EMPUJE  = 90;

// --- Parámetros de movimiento ---
const int PASO_MS            = 300;   // avance por iteración
const int ESTABILIZAR_MS     = 150;   // pausa tras frenar antes de consultar cámara
const int MAX_PASOS_POR_LINEA= 60;    // tope de seguridad por línea
const int PASOS_BLANCOS_FIN  = 6;     // fin de línea: N pasos seguidos en blanco tras haber detectado algo

// --- Estado ---
int idFloor = -1;
int idWood  = -1;

// ============================================================
//  Declaraciones
// ============================================================
void conectarWifi();
void adelante(); void atras(); void izquierda(); void derecha(); void detener();
void pasoAdelante();
bool consultar(const char* url, int &idOut);
void recolectarPiso(int id);
void recolectarMadera(int id);
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

  // ---------- FASE 1: línea 1 ----------
  Serial.println("=== Recorriendo línea 1 ===");
  recorrerLinea();

  Serial.println("=== Llevando al depósito ===");
  irADeposito();
  descargar();

  // ---------- FASE 2: línea 2 ----------
  Serial.println("=== Yendo a línea 2 ===");
  volverALineaDos();

  Serial.println("=== Recorriendo línea 2 ===");
  recorrerLinea();

  Serial.println("=== Llevando al depósito (final) ===");
  irADeposito();
  descargar();

  detener();
  Serial.println("=== Rutina terminada ===");
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

  for (int i = 0; i < MAX_PASOS_POR_LINEA; i++) {
    pasoAdelante();
    delay(ESTABILIZAR_MS);

    if (!consultar(urlFloor, idFloor)) {
      Serial.println("Error consultando /color, reintentando paso.");
      delay(300);
      continue;
    }
    Serial.printf("[linea] paso %d  floor=%d\n", i, idFloor);

    if (idFloor == 0) {
      // Piso blanco
      if (vioAlgo) {
        blancosSeguidos++;
        if (blancosSeguidos >= PASOS_BLANCOS_FIN) {
          Serial.println("Fin de línea detectado.");
          return;
        }
      }
      continue;
    }

    // floor detectó algo (red/black/green): reseteo contador y consulto madera
    blancosSeguidos = 0;
    vioAlgo = true;

    if (!consultar(urlWood, idWood)) {
      Serial.println("Error consultando /color-wood, seguiré solo con piso.");
      idWood = -1;
    }
    Serial.printf("         wood=%d\n", idWood);

    // Recolección por lado (green y white => no hacer nada)
    if (idFloor == 1 || idFloor == 2) recolectarPiso(idFloor);
    if (idWood  == 1 || idWood  == 2) recolectarMadera(idWood);
  }

  Serial.println("MAX_PASOS_POR_LINEA alcanzado sin fin explícito.");
}

void pasoAdelante() {
  adelante();
  delay(PASO_MS);
  detener();
}

// ============================================================
//  Pinzas (AJUSTA según tu mecánica)
// ============================================================
// id: 1 = negro -> s1 ; 2 = rojo -> s2 ; cualquier otro (green, white) -> nada
void recolectarPiso(int id) {
  Servo* pinza = nullptr;
  const char* etiqueta = "";
  if (id == 1)      { pinza = &s1; etiqueta = "NEGRO piso (s1)"; }
  else if (id == 2) { pinza = &s2; etiqueta = "ROJO piso (s2)";  }
  else return;

  Serial.printf("  -> %s empuja\n", etiqueta);
  pinza->write(PINZA_EMPUJE);
  delay(600);
  pinza->write(PINZA_REPOSO);
  delay(400);
}

// id: 1 = negro -> abrir s4, empujar s5 ; 2 = rojo -> abrir s3, empujar s5
void recolectarMadera(int id) {
  Servo* compuerta = nullptr;
  const char* etiqueta = "";
  if (id == 1)      { compuerta = &s4; etiqueta = "NEGRO madera (s4+s5)"; }
  else if (id == 2) { compuerta = &s3; etiqueta = "ROJO madera (s3+s5)";  }
  else return;

  Serial.printf("  -> %s: abrir compuerta\n", etiqueta);
  compuerta->write(COMPUERTA_ABIERTA);
  delay(400);

  Serial.println("     empujador s5");
  s5.write(EMPUJADOR_EMPUJE);
  delay(700);
  s5.write(EMPUJADOR_REPOSO);
  delay(400);

  compuerta->write(COMPUERTA_CERRADA);
  delay(400);
}

// ============================================================
//  Navegación entre fases (STUBS por tiempo: calibra con tu arena)
// ============================================================
void irADeposito() {
  // Ejemplo genérico: girar, avanzar, llegar al área de depósito.
  // Reemplaza estos tiempos por los tuyos.
  detener();            delay(300);
  izquierda();          delay(700); detener(); delay(200);
  adelante();           delay(1500); detener(); delay(300);
}

void descargar() {
  // Mecanismo de descarga aún no definido. Cuando lo tengas
  // (ej. abrir compuertas s3/s4 para soltar todo, o una tolva extra),
  // agrega aquí la secuencia.
  Serial.println("  -> descargando cilindros (pendiente)");
  delay(500);
}

void volverALineaDos() {
  // Desde el depósito, moverse al inicio de la línea 2.
  // Reemplaza tiempos por los tuyos.
  atras();              delay(1500); detener(); delay(300);
  derecha();            delay(700);  detener(); delay(200);
  adelante();           delay(1000); detener(); delay(300);
}

// ============================================================
//  HTTP
// ============================================================
// Devuelve true si logró leer un code válido; idOut queda con el valor
// (-1 si hubo error de red/parseo).
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
//  Motores
// ============================================================
void adelante()  { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  }
void atras()     { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
void izquierda() { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  }
void derecha()   { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
void detener()   { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);  }

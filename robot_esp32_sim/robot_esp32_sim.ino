#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ============================================================
//  SIMULACIÓN — sin WiFi ni HTTP, sin ESP32Servo.
//  Usa ledc (PWM nativo) para mover los 5 servos.
// ============================================================

// --- I2C del LCD ---
const int PIN_SDA = 8;
const int PIN_SCL = 9;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// --- Mapeo de servos (API LEDC 3.x: se trabaja directo con pines) ---
// s1 (pin 7)  -> pinza ROJO piso
// s2 (pin 6)  -> pinza NEGRO piso
// s3 (pin 4)  -> compuerta ROJO madera
// s4 (pin 15) -> compuerta NEGRO madera
// s5 (pin 5)  -> empujador madera
const int pinS1 = 7;
const int pinS2 = 6;
const int pinS3 = 4;
const int pinS4 = 15;
const int pinS5 = 5;

// Configuración PWM servo: 50 Hz, 14 bits de resolución (período = 20000 us)
const int SERVO_FREQ = 50;
const int SERVO_RES  = 14;
const int SERVO_MAX  = (1 << SERVO_RES);  // 16384

// Ángulos lógicos
// En Wokwi: 0°=horn arriba, 90°=derecha, 180°=abajo.
// S1: reposo=90 (derecha) -> empuje=0   (arriba)
// S2: reposo=90 (derecha) -> empuje=180 (abajo, opuesto de S1)
const int S1_REPOSO         = 90;
const int S1_EMPUJE         = 0;
const int S2_REPOSO         = 90;
const int S2_EMPUJE         = 180;
const int COMPUERTA_CERRADA = 0;
const int COMPUERTA_ABIERTA = 90;
const int EMPUJADOR_REPOSO  = 0;
const int EMPUJADOR_EMPUJE  = 90;

const int TIEMPO_PINZA_MS           = 900;
const int TIEMPO_COMPUERTA_MS       = 800;
const int TIEMPO_EMPUJADOR_MS       = 1000;
const int PAUSA_ENTRE_ESCENARIOS_MS = 4000;

// Convierte un ángulo (0-180) a duty cycle PWM para servo
// Pulso: 544 us (0°) a 2400 us (180°), período 20000 us
// (rango estándar de la librería Arduino Servo; Wokwi ignora pulsos > 2400 us)
void servoWrite(int pin, int angulo) {
  int pulso_us = map(angulo, 0, 180, 544, 2400);
  int duty = (int)((long)pulso_us * SERVO_MAX / 20000L);
  ledcWrite(pin, duty);
}

void servoAttach(int pin) {
  ledcAttach(pin, SERVO_FREQ, SERVO_RES);
}

// --- Escenarios ---
struct Escenario { int floorColor; int woodColor; };
Escenario escenarios[] = {
  { 2, 1 }, { 1, 2 }, { 2, 2 }, { 1, 1 },
  { 3, 1 }, { 2, 3 }, { 3, 3 }, { 0, 0 },
};
const int N_ESCENARIOS = sizeof(escenarios) / sizeof(escenarios[0]);

const char* nombreCorto(int id) {
  switch (id) {
    case 0:  return "WHT";
    case 1:  return "BLK";
    case 2:  return "RED";
    case 3:  return "GRN";
    default: return "?";
  }
}

void lcdLinea2(const char* msg) {
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(msg);
}

void mostrarEnLCD(int numEsc, int floorId, int woodId, const char* acciones) {
  char buf[17];
  lcd.clear();
  snprintf(buf, sizeof(buf), "Esc %d/%d %s+%s", numEsc, N_ESCENARIOS,
           nombreCorto(floorId), nombreCorto(woodId));
  lcd.setCursor(0, 0);
  lcd.print(buf);
  lcd.setCursor(0, 1);
  lcd.print(acciones);
}

// id: 1=NEGRO->s2 ; 2=ROJO->s1 ; otro -> nada
String recolectarPiso(int id) {
  int pin = -1;
  int reposo = 0;
  int empuje = 0;
  const char* etiq = "";
  if (id == 1)      { pin = pinS2; reposo = S2_REPOSO; empuje = S2_EMPUJE; etiq = "s2"; }
  else if (id == 2) { pin = pinS1; reposo = S1_REPOSO; empuje = S1_EMPUJE; etiq = "s1"; }
  else return String("piso OFF");

  servoWrite(pin, empuje);
  delay(TIEMPO_PINZA_MS);
  servoWrite(pin, reposo);
  delay(300);
  return String(etiq) + " ON";
}

// id: 1=NEGRO->s5 + s4 ; 2=ROJO->s5 + s3 ; otro -> nada
// Orden: primero empuja con s5, 1 segundo después abre la compuerta
// correspondiente (s3 para rojo, s4 para negro).
String recolectarMadera(int id) {
  int pinComp = -1;
  const char* etiq = "";
  if (id == 1)      { pinComp = pinS4; etiq = "s5+s4"; }
  else if (id == 2) { pinComp = pinS3; etiq = "s5+s3"; }
  else return String("madera OFF");

  // 1. Empuja primero
  servoWrite(pinS5, EMPUJADOR_EMPUJE);
  delay(1000);  // 1 seg antes de abrir la compuerta

  // 2. Abre la compuerta correspondiente
  servoWrite(pinComp, COMPUERTA_ABIERTA);
  delay(TIEMPO_COMPUERTA_MS);

  // 3. Deja caer el cilindro y cierra
  servoWrite(pinComp, COMPUERTA_CERRADA);
  delay(TIEMPO_COMPUERTA_MS);

  // 4. Vuelve el empujador a reposo
  servoWrite(pinS5, EMPUJADOR_REPOSO);
  delay(300);

  return String(etiq) + " ON";
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(PIN_SDA, PIN_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando...");
  lcdLinea2("ledc attach");

  servoAttach(pinS1);
  servoAttach(pinS2);
  servoAttach(pinS3);
  servoAttach(pinS4);
  servoAttach(pinS5);

  lcdLinea2("reposo");
  servoWrite(pinS1, S1_REPOSO);
  servoWrite(pinS2, S2_REPOSO);
  servoWrite(pinS3, COMPUERTA_CERRADA);
  servoWrite(pinS4, COMPUERTA_CERRADA);
  servoWrite(pinS5, EMPUJADOR_REPOSO);
  delay(500);

  lcdLinea2("listo!");
  delay(600);

  for (int i = 0; i < N_ESCENARIOS; i++) {
    const Escenario& e = escenarios[i];
    int num = i + 1;
    Serial.printf("\n=== Escenario %d: floor=%s wood=%s ===\n",
                  num, nombreCorto(e.floorColor), nombreCorto(e.woodColor));

    mostrarEnLCD(num, e.floorColor, e.woodColor, "actuando...");
    delay(800);

    String a1 = recolectarPiso(e.floorColor);
    String a2 = recolectarMadera(e.woodColor);
    String combo = a1 + " " + a2;
    if (combo.length() > 16) combo = combo.substring(0, 16);

    mostrarEnLCD(num, e.floorColor, e.woodColor, combo.c_str());
    delay(PAUSA_ENTRE_ESCENARIOS_MS);
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  FIN");
  lcd.setCursor(0, 1);
  lcd.print("simulacion OK");
  Serial.println("\n=== Fin ===");
}

void loop() {}

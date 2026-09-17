/*
 * ============================================================================
 *  TP Integrador de IoT - 4K2 - 2026
 *  SKETCH 1: Toma y envio de datos a ThingSpeak
 *  Placa: UTN FRC (ESP32 DOIT DEVKIT V1)
 * ============================================================================
 *
 *  Funciones:
 *   1) Brillo del LED integrado (GPIO 2) regulado por el potenciometro. 12 bits.
 *   2) Temperatura y humedad del DHT22 en pantalla, en tiempo real.
 *   3) Brillo del LED verde externo (GPIO 23) regulado por dos pines touch.
 *      Touch 13 sube, touch 4 baja. 12 bits, se muestra el porcentaje.
 *   4) Envio cada 16 s de 6 campos a un canal publico de ThingSpeak.
 *   +) ArduinoOTA activo: permite subir el Sketch 2 por red, sin cable USB.
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <DHT.h>
#include <assert.h>

const char*         WIFI_SSID        = "ACNET2";    // hotspot del celular, 2.4 GHz
const char*         WIFI_PASS        = "";
const unsigned long TS_CHANNEL_ID    = 3491303;  // solo informativo: la escritura va por API key
const char*         TS_WRITE_API_KEY = "DL4NFMT58QUR5S4A";    
const char*         OTA_HOSTNAME     = "esp32-Grupo06";
const char*         TITULO_PANTALLA  = "TP IoT 4K2 - G:06";

// Endpoint de escritura de ThingSpeak (mismo que usa el caso practico 16)
const char*         TS_SERVER        = "http://api.thingspeak.com/update";

// ---------------------- Pines de la placa  -------------------------------
const int PIN_POTE       = 32;   // ADC1: obligatorio, ADC2 no funciona con WiFi
const int PIN_DHT        = 33;   // DHT22, salida digital
const int PIN_LED_INT    = 2;    // LED azul integrado de la ESP32
const int PIN_LED_EXT    = 23;   // LED verde de la placa de la catedra
const int PIN_TOUCH_UP   = 13;   // T4: sube el brillo del LED externo
const int PIN_TOUCH_DOWN = 4;    // T0: baja el brillo del LED externo
// OLED por I2C: SDA -> GPIO 21, SCL -> GPIO 22

// ---------------------- Parametros ------------------------------------------
const int PWM_FREQ = 5000;
const int PWM_BITS = 12;         // 12 bits -> duty 0..4095, lo pide el enunciado
const int PWM_MAX  = 4095;

const unsigned long MS_DHT        = 2000;   // el DHT22 no admite lecturas mas rapidas
const unsigned long MS_TOUCH      = 100;    // ritmo de la rampa de brillo (como el caso 9)
const unsigned long MS_DISPLAY    = 100;
const unsigned long MS_PANTALLA   = 2000;   // tiempo por pantalla en rotacion (clima <-> thingspeak)
const unsigned long MS_FIJA       = 5000;   // cuanto se muestra la pantalla del pote o del touch
const unsigned long MS_ENVIO      = 16000;  // ThingSpeak free: minimo 15 s
const unsigned long MS_RECONEXION = 5000;
const unsigned long MS_REINTENTO  = 5000;   // espera antes de reintentar un envio fallido
const unsigned long MS_LOG_TOUCH  = 500;    // ritmo del log de calibracion del touch

// ---- Potenciometro -------------------------------------------------------
const int POTE_MUESTRAS   = 16;   // promedio por lectura
const int POTE_HISTERESIS = 16;   // cambio minimo para actualizar el brillo del LED
const int POTE_MOVIMIENTO = 100;  // cambio minimo para mostrar la pantalla (el ruido no llega a esto)

// ---- Touch capacitivo ----------------------------------------------------
// Medido en la placa: sin tocar ~1000, tocando ~400. Por debajo de 600 = tocado.
const int   TOUCH_UMBRAL = 600;
const int   TOUCH_PASO   = 136;   // 0 -> 100% en ~3 s de contacto sostenido

#define AUTOTEST  1   
#define LOG_TOUCH 1  

// ---------------------- Objetos ---------------------------------------------
Adafruit_SH1106G display(128, 64, &Wire, -1);
DHT dht(PIN_DHT, DHT22);

// ---------------------- Estado ----------------------------------------------
int   pwmLedInterno = 0;        // 0..4095, valor del potenciometro
int   poteReferencia = 0;       // valor del pote la ultima vez que se mostro su pantalla
int   brilloExterno = 0;        // 0..4095, controlado por los touch
float temperatura   = NAN;
float humedad       = NAN;

volatile bool touchDetectadoUp   = false;
volatile bool touchDetectadoDown = false;
bool  estabaTocado = false;     // estado del touch en la verificacion anterior

enum { PANT_CLIMA, PANT_LED_INT, PANT_LED_EXT, PANT_TS, NUM_PANTALLAS };
int   pantalla       = PANT_CLIMA;
bool  pantallaFija   = false;
unsigned long tsPantalla = 0, tsInteraccion = 0;

int   ultimaEntradaTS = 0;
int   enviosOk       = 0;
int   enviosErr      = 0;
int   ultimoCodigoTS = 0;
bool  huboEnvio      = false;
bool  otaEnCurso     = false;
bool  reintentoPend  = false;   // hay un envio fallido esperando reintento

unsigned long tsDHT = 0, tsTouch = 0, tsDisplay = 0, tsEnvio = 0,
              tsReconexion = 0, tsReintento = 0, tsLogTouch = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void pwmInit(int pin)           { ledcAttach(pin, PWM_FREQ, PWM_BITS); }
  void pwmSet(int pin, int valor) { ledcWrite(pin, valor); }
#else
  int canalDe(int pin) { return (pin == PIN_LED_INT) ? 0 : 1; }
  void pwmInit(int pin) {
    int canal = canalDe(pin);
    ledcSetup(canal, PWM_FREQ, PWM_BITS);
    ledcAttachPin(pin, canal);
  }
  void pwmSet(int pin, int valor) { ledcWrite(canalDe(pin), valor); }
#endif

// ============================================================================
//  Helpers
// ============================================================================
int clamp12(int v) {
  if (v < 0)       return 0;
  if (v > PWM_MAX) return PWM_MAX;
  return v;
}

int aPorcentaje(int valor12bits) {
  return map(clamp12(valor12bits), 0, PWM_MAX, 0, 100);
}

int leerPoteFiltrado() {
  long suma = 0;
  for (int i = 0; i < POTE_MUESTRAS; i++) suma += analogRead(PIN_POTE);
  int crudo = clamp12(suma / POTE_MUESTRAS);
  if (crudo <= POTE_HISTERESIS)           return 0;
  if (crudo >= PWM_MAX - POTE_HISTERESIS) return PWM_MAX;

  if (abs(crudo - pwmLedInterno) >= POTE_HISTERESIS) return crudo;
  return pwmLedInterno;
}

// ============================================================================
//  Touch
// ============================================================================
void IRAM_ATTR handleTouchUp()   { touchDetectadoUp = true; }
void IRAM_ATTR handleTouchDown() { touchDetectadoDown = true; }

void configurarTouch() {
  touchAttachInterrupt(PIN_TOUCH_UP,   handleTouchUp,   TOUCH_UMBRAL);
  touchAttachInterrupt(PIN_TOUCH_DOWN, handleTouchDown, TOUCH_UMBRAL);
}

bool pinTocado(int pin) {
  return touchRead(pin) < TOUCH_UMBRAL;
}

void actualizarTouch() {
  bool tocaUp   = pinTocado(PIN_TOUCH_UP);
  bool tocaDown = pinTocado(PIN_TOUCH_DOWN);

  // La interrupcion avisa; se confirma leyendo el pin para descartar ruido
  if (touchDetectadoUp && tocaUp)     brilloExterno = clamp12(brilloExterno + TOUCH_PASO);
  if (touchDetectadoDown && tocaDown) brilloExterno = clamp12(brilloExterno - TOUCH_PASO);
  touchDetectadoUp   = false;
  touchDetectadoDown = false;
  pwmSet(PIN_LED_EXT, brilloExterno);

  bool tocado = tocaUp || tocaDown;
  if (tocado && !estabaTocado) {
    interaccion(PANT_LED_EXT);              // paso de libre a TOCADO: mostrar pantalla
  } else if (tocado && pantalla == PANT_LED_EXT) {
    tsInteraccion = millis();               // sigue tocando: el tiempo cuenta desde el ultimo toque
  }
  estabaTocado = tocado;
}

#if LOG_TOUCH
void logTouch() {
  int up   = touchRead(PIN_TOUCH_UP);
  int down = touchRead(PIN_TOUCH_DOWN);
  Serial.printf("[TOUCH] pin13=%4d %-6s | pin4=%4d %-6s | umbral=%d | brillo=%d%%\n",
                up,   up   < TOUCH_UMBRAL ? "TOCADO" : "libre",
                down, down < TOUCH_UMBRAL ? "TOCADO" : "libre",
                TOUCH_UMBRAL, aPorcentaje(brilloExterno));
}
#endif

// ============================================================================
//  DHT22
// ============================================================================
void leerDHT() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperatura = t;
  if (!isnan(h)) humedad     = h;

  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] lectura fallida, se conserva el valor anterior");
  }
}

// ============================================================================
//  ThingSpeak
// ============================================================================
String diagnosticoTS(int httpCode, const String& cuerpo) {
  if (httpCode == 200) {
    if (cuerpo == "0") return "RECHAZADO: API key invalida o envio antes de los 15 s";
    return "OK, entrada #" + cuerpo;
  }
  if (httpCode > 0) return "el servidor respondio HTTP " + String(httpCode);
  return "fallo de conexion: " + HTTPClient::errorToString(httpCode);
}

void enviarThingSpeak(bool esReintento) {
  if (WiFi.status() != WL_CONNECTED) {
    ultimoCodigoTS = -1;
    enviosErr++;
    huboEnvio = true;
    Serial.println("[TS] sin WiFi, se saltea el envio");
    return;
  }

  int aleatorio = random(100, 501);      
  int uptime    = millis() / 1000;
  int brilloPct = aPorcentaje(brilloExterno);
  float tEnvio = isnan(temperatura) ? 0.0f : temperatura;
  float hEnvio = isnan(humedad)     ? 0.0f : humedad;
  if (isnan(temperatura) || isnan(humedad)) {
    Serial.println("[TS] AVISO: DHT22 sin lectura valida, se envian 0.0 en f2/f3");
  }

  // Los 6 campos van como parametros de la URL
  String url = String(TS_SERVER) + "?api_key=" + TS_WRITE_API_KEY
             + "&field1=" + String(aleatorio)
             + "&field2=" + String(tEnvio, 1)
             + "&field3=" + String(hEnvio, 1)
             + "&field4=" + String(uptime)
             + "&field5=" + String(pwmLedInterno)
             + "&field6=" + String(brilloPct);

  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(5000);
  http.begin(url);

  int    httpCode = http.GET();
  String cuerpo   = (httpCode > 0) ? http.getString() : "";
  cuerpo.trim();
  http.end();

  bool ok = (httpCode == 200 && cuerpo != "0");
  ultimoCodigoTS = ok ? 200 : (httpCode == 200 ? 0 : httpCode);
  huboEnvio = true;

  if (ok) {
    enviosOk++;
    ultimaEntradaTS = cuerpo.toInt();
  } else {
    enviosErr++;
  }

  Serial.printf("[TS]%s f1=%d f2=%.1f f3=%.1f f4=%d f5=%d f6=%d -> %s | ok=%d err=%d\n",
                esReintento ? " (reintento)" : "",
                aleatorio, tEnvio, hEnvio, uptime, pwmLedInterno, brilloPct,
                diagnosticoTS(httpCode, cuerpo).c_str(), enviosOk, enviosErr);

  if (!ok && !esReintento) {
    reintentoPend = true;
    tsReintento   = millis();
    Serial.printf("[TS] se reintenta en %lu ms\n", MS_REINTENTO);
  }
}

// ============================================================================
//  Display
// ============================================================================
void mensajePantalla(const char* linea1, const char* linea2) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(linea1);
  if (linea2) display.println(linea2);
  display.display();
}

void interaccion(int p) {
  pantalla      = p;
  pantallaFija  = true;
  tsInteraccion = millis();
}

void encabezado(const char* titulo) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(titulo);
  // Puntos de pagina solo en las dos pantallas que rotan
  if (pantalla == PANT_CLIMA || pantalla == PANT_TS) {
    bool enClima = (pantalla == PANT_CLIMA);
    if (enClima) display.fillCircle(118, 3, 2, SH110X_WHITE); else display.drawCircle(118, 3, 2, SH110X_WHITE);
    if (enClima) display.drawCircle(125, 3, 2, SH110X_WHITE); else display.fillCircle(125, 3, 2, SH110X_WHITE);
  }
  display.drawFastHLine(0, 40, 128, SH110X_WHITE);
}

void numeroGrande(const char* valor, const char* unidad) {
  display.setTextSize(3);
  display.setCursor(4, 13);
  display.print(valor);
  display.setTextSize(2);
  display.setCursor(display.getCursorX() + 2, 21);
  display.print(unidad);
  display.setTextSize(1);
}

void barra(int y, int pct) {
  display.drawRect(0, y, 128, 7, SH110X_WHITE);
  display.fillRect(2, y + 2, map(constrain(pct, 0, 100), 0, 100, 0, 124), 3, SH110X_WHITE);
}

// Rotando se llena; fija se vacia.
void barraTiempo(unsigned long ahora) {
  long ancho = pantallaFija
             ? 128 - (long)(ahora - tsInteraccion) * 128 / (long)MS_FIJA
             : (long)(ahora - tsPantalla) * 128 / (long)MS_PANTALLA;
  display.drawFastHLine(0, 63, constrain(ancho, 0L, 128L), SH110X_WHITE);
}

void pantallaClima() {
  encabezado("CLIMA");
  char buf[8] = "--.-";
  if (!isnan(temperatura)) snprintf(buf, sizeof(buf), "%.1f", temperatura);
  numeroGrande(buf, "C");

  display.setCursor(6, 44);
  if (isnan(humedad)) display.print("HUMEDAD       --.- %");
  else                display.printf("HUMEDAD      %4.1f %%", humedad);
  barra(54, isnan(humedad) ? 0 : (int)humedad);
}

void pantallaLedInterno() {
  encabezado("LED INTERNO");
  char buf[6];
  snprintf(buf, sizeof(buf), "%d", pwmLedInterno);
  numeroGrande(buf, "");

  barra(44, aPorcentaje(pwmLedInterno));
  display.setCursor(0, 54);
  display.print("0     PWM 12b   4095");
}

void pantallaLedExterno() {
  encabezado("LED EXTERNO");
  char buf[5];
  snprintf(buf, sizeof(buf), "%d", aPorcentaje(brilloExterno));
  numeroGrande(buf, "%");

  display.setCursor(0, 44);
  display.print("(-)pin4    pin13(+)");
  barra(54, aPorcentaje(brilloExterno));
}

void pantallaThingSpeak() {
  encabezado("THINGSPEAK");
  long faltan = ((long)MS_ENVIO - (long)(millis() - tsEnvio)) / 1000 + 1;
  char buf[4];
  snprintf(buf, sizeof(buf), "%ld", constrain(faltan, 0L, 99L));
  numeroGrande(buf, "s");

  display.setCursor(0, 44);
  if (!huboEnvio)                 display.print("esperando envio");
  else if (ultimoCodigoTS == 200) display.printf("OK:%d ERR:%d #%d", enviosOk, enviosErr, ultimaEntradaTS);
  else                            display.printf("OK:%d ERR:%d FALLO", enviosOk, enviosErr);

  unsigned long seg = millis() / 1000;
  display.setCursor(0, 54);
  display.printf("WiFi %s   up %02lu:%02lu",
                 WiFi.status() == WL_CONNECTED ? "OK" : "--", seg / 60, seg % 60);
}

void (*PANTALLAS[NUM_PANTALLAS])() = {
  pantallaClima, pantallaLedInterno, pantallaLedExterno, pantallaThingSpeak
};

// Usa millis() propio: interaccion() puede haber guardado un tiempo posterior
// al "ahora" del loop, y la resta unsigned daria la vuelta.
void actualizarPantalla() {
  unsigned long ahora = millis();

  // Pote y touch solo se ven al accionarlos; al terminar vuelve a clima
  if (pantallaFija && ahora - tsInteraccion >= MS_FIJA) {
    pantallaFija = false;
    pantalla     = PANT_CLIMA;
    tsPantalla   = ahora;
  }
  // Rotacion continua: solo clima <-> thingspeak
  if (!pantallaFija && ahora - tsPantalla >= MS_PANTALLA) {
    pantalla   = (pantalla == PANT_CLIMA) ? PANT_TS : PANT_CLIMA;
    tsPantalla = ahora;
  }

  PANTALLAS[pantalla]();
  if (pantalla != PANT_TS) barraTiempo(ahora);
  display.display();
}

// ============================================================================
//  WiFi y OTA
// ============================================================================
void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] conectando");
  mensajePantalla("Conectando WiFi...", WIFI_SSID);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] conectado, IP: ");
    Serial.println(WiFi.localIP());
    mensajePantalla("WiFi conectado", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[WiFi] sin conexion, se reintenta desde el loop");
    mensajePantalla("WiFi FALLO", "reintentando...");
  }
  delay(1200);
}

void configurarOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);

  ArduinoOTA.onStart([]() {
    otaEnCurso = true;
    pwmSet(PIN_LED_INT, 0);  
    pwmSet(PIN_LED_EXT, 0);
    mensajePantalla("OTA en curso...", "no desconectar");
  });

  ArduinoOTA.onProgress([](unsigned int hechos, unsigned int total) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA en curso...");
    display.printf("%u%%\n", (hechos * 100) / total);
    display.display();
  });

  ArduinoOTA.onEnd([]() {
    mensajePantalla("OTA completa", "reiniciando...");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaEnCurso = false;
    char buf[24];
    snprintf(buf, sizeof(buf), "OTA error %u", error);
    mensajePantalla(buf, NULL);
    Serial.printf("[OTA] error %u\n", error);
  });

  ArduinoOTA.begin();
  Serial.printf("[OTA] listo como '%s'\n", OTA_HOSTNAME);
}

// ============================================================================
//  Autochequeo: falla ruidosamente si alguien rompe la aritmetica del sketch
// ============================================================================
#if AUTOTEST
void autotest() {
  // 1) el clamp no deja salir del rango de 12 bits
  assert(clamp12(-500) == 0);
  assert(clamp12(0)    == 0);
  assert(clamp12(9999) == PWM_MAX);
  assert(clamp12(2048) == 2048);

  // 2) el mapeo a porcentaje da los extremos exactos
  assert(aPorcentaje(-1)      == 0);
  assert(aPorcentaje(0)       == 0);
  assert(aPorcentaje(PWM_MAX) == 100);
  assert(aPorcentaje(2048)    == 50);

  // 3) el aleatorio del field 1 nunca se sale de [100, 500]
  for (int i = 0; i < 1000; i++) {
    int r = random(100, 501);
    assert(r >= 100 && r <= 500);
  }

  Serial.println("[AUTOTEST] OK");
}
#endif

// ============================================================================
//  setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Sketch 1 - TP Integrador IoT 4K2 ===");

  // Sin seed, la ESP32 genera la misma secuencia en cada arranque.
  randomSeed(analogRead(35) ^ micros());

#if AUTOTEST
  autotest();
#endif

  Wire.begin(21, 22);
  display.begin(0x3C, true);
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setTextWrap(false);
  mensajePantalla(TITULO_PANTALLA, "iniciando...");

  dht.begin();

  pwmInit(PIN_LED_INT);
  pwmInit(PIN_LED_EXT);
  pwmSet(PIN_LED_INT, 0);
  pwmSet(PIN_LED_EXT, 0);
  pwmLedInterno  = leerPoteFiltrado();   // evita que el loop lo tome como movimiento al arrancar
  poteReferencia = pwmLedInterno;

  configurarTouch();

  conectarWiFi();
  configurarOTA();

  leerDHT();   // primera lectura para no arrancar con la pantalla vacia

  tsEnvio = millis();   // el primer envio sale a los 16 s del arranque
  tsPantalla = millis();
}

void loop() {
  ArduinoOTA.handle();   // primero: si hay una subida en curso, tiene prioridad
  if (otaEnCurso) return;

  unsigned long ahora = millis();

  // --- Item 1: potenciometro -> brillo del LED integrado (12 bits) ---
  pwmLedInterno = leerPoteFiltrado();
  pwmSet(PIN_LED_INT, pwmLedInterno);

  // Solo un movimiento real del pote muestra su pantalla, no el ruido del ADC
  if (abs(pwmLedInterno - poteReferencia) >= POTE_MOVIMIENTO) {
    poteReferencia = pwmLedInterno;
    interaccion(PANT_LED_INT);
  }

  // --- Item 3: touch -> brillo del LED externo (12 bits) ---
  if (ahora - tsTouch >= MS_TOUCH) {
    tsTouch = ahora;
    actualizarTouch();
  }

  // --- Item 2: DHT22 ---
  if (ahora - tsDHT >= MS_DHT) {
    tsDHT = ahora;
    leerDHT();
  }

  // --- Pantalla ---
  if (ahora - tsDisplay >= MS_DISPLAY) {
    tsDisplay = ahora;
    actualizarPantalla();
  }

  // --- Log de calibracion del touch ---
#if LOG_TOUCH
  if (ahora - tsLogTouch >= MS_LOG_TOUCH) {
    tsLogTouch = ahora;
    logTouch();
  }
#endif

  // --- Item 4: envio a ThingSpeak cada 16 s ---
  if (ahora - tsEnvio >= MS_ENVIO) {
    tsEnvio = ahora;
    enviarThingSpeak(false);
  }

  // --- Reintento de un envio fallido ---
  // Corre el reloj de los 16 s desde el reintento, para no quedar por debajo
  // del minimo de 15 s que exige ThingSpeak en el envio siguiente.
  if (reintentoPend && ahora - tsReintento >= MS_REINTENTO) {
    reintentoPend = false;
    tsEnvio = millis();
    enviarThingSpeak(true);
  }

  // --- Reconexion WiFi no bloqueante ---
  if (WiFi.status() != WL_CONNECTED && ahora - tsReconexion >= MS_RECONEXION) {
    tsReconexion = ahora;
    Serial.println("[WiFi] desconectado, reintentando...");
    WiFi.reconnect();
  }
}

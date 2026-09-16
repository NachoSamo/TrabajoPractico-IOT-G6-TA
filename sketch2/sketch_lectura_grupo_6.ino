/*
 * TP Integrador de IoT - 4K2 - 2026
 * SKETCH 2: Lectura de datos desde ThingSpeak
 *
 * Realizado a partir de los ejemplos de la catedra ubicados en TPA 2026:
 * - ThingSpeak_LeerDatos
 * - Caso 15 OTA display
 * - Caso 16 OTA envio de datos
 * - OTAWebUpdater pulsador
 */

#include <WiFi.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <ArduinoOTA.h>

// ==================== CONFIGURACION ====================

// Credenciales de la red WiFi
const char* ssid = "Olivia";
const char* password = "olivia111";

// Datos del canal publico de ThingSpeak
unsigned long channelID = 3491303;

// Pulsador de la placa de la catedra
const int pinBoton = 19;
int ultimoEstado = HIGH;
unsigned long cantidadPulsaciones = 0;

// Intervalos de trabajo
unsigned long previousMillisLectura = 0;
const long intervalLectura = 16000;

unsigned long previousMillisPantalla = 0;
const long intervalPantalla = 4000;

// ==================== OBJETOS ====================

WiFiClient client;

Adafruit_SH1106G display = Adafruit_SH1106G(128, 64, &Wire, -1);

// ==================== VARIABLES DE THINGSPEAK ====================

String valorAleatorio = "--";
String temperatura = "--";
String humedad = "--";
String tiempoTranscurrido = "--";
String pwmLedIntegrado = "--";
String brilloLedExterno = "--";

int pantallaActual = 0;
int estadoLectura = 0;

// ==================== FUNCIONES WIFI ====================

void conectarWiFi() {
  Serial.print("\nConectando a: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Conectando a WiFi");
  display.display();

  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }

  Serial.println("\nConectado a WiFi");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("WiFi conectado");
  display.println(WiFi.localIP());
  display.display();
  delay(1000);
}

// ==================== FUNCIONES OTA ====================

void configurarOTA() {
  ArduinoOTA.setHostname("esp32-Grupo06");

  ArduinoOTA.onStart([]() {
    Serial.println("Inicio OTA");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Actualizacion OTA");
    display.println("No desconectar");
    display.display();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nFin OTA");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("OTA finalizada");
    display.println("Reiniciando...");
    display.display();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error OTA [%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("OTA iniciado");
}

// ==================== LECTURA DE THINGSPEAK ====================

void leerThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
  }

  // Lee todos los campos del ultimo registro del canal publico.
  estadoLectura = ThingSpeak.readMultipleFields(channelID);

  if (estadoLectura == 200) {
    valorAleatorio = ThingSpeak.getFieldAsString(1);
    temperatura = ThingSpeak.getFieldAsString(2);
    humedad = ThingSpeak.getFieldAsString(3);
    tiempoTranscurrido = ThingSpeak.getFieldAsString(4);
    pwmLedIntegrado = ThingSpeak.getFieldAsString(5);
    brilloLedExterno = ThingSpeak.getFieldAsString(6);

    Serial.println("Datos leidos desde ThingSpeak:");
    Serial.println("Campo 1 - Aleatorio: " + valorAleatorio);
    Serial.println("Campo 2 - Temperatura: " + temperatura);
    Serial.println("Campo 3 - Humedad: " + humedad);
    Serial.println("Campo 4 - Tiempo: " + tiempoTranscurrido);
    Serial.println("Campo 5 - PWM LED integrado: " + pwmLedIntegrado);
    Serial.println("Campo 6 - Brillo LED externo: " + brilloLedExterno);
  }
  else {
    Serial.println("Problemas leyendo el canal. HTTP error code " + String(estadoLectura));
  }
}

// ==================== PANTALLAS ====================

void mostrarPantallaUno() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Datos ThingSpeak 1/3");
  display.println("--------------------");
  display.print("Aleatorio: ");
  display.println(valorAleatorio);
  display.print("Temp: ");
  display.print(temperatura);
  display.println(" C");
  display.print("Hum: ");
  display.print(humedad);
  display.println(" %");
  display.display();
}

void mostrarPantallaDos() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Datos ThingSpeak 2/3");
  display.println("--------------------");
  display.print("Tiempo: ");
  display.print(tiempoTranscurrido);
  display.println(" s");
  display.print("PWM interno: ");
  display.println(pwmLedIntegrado);
  display.print("LED externo: ");
  display.print(brilloLedExterno);
  display.println(" %");
  display.display();
}

void mostrarPantallaTres() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Pulsador 3/3");
  display.println("--------------------");
  display.println("Cantidad de");
  display.println("pulsaciones:");
  display.setTextSize(2);
  display.println(cantidadPulsaciones);
  display.setTextSize(1);

  if (estadoLectura == 200) {
    display.println("ThingSpeak: OK");
  }
  else {
    display.print("Error TS: ");
    display.println(estadoLectura);
  }

  display.display();
}

void actualizarPantalla() {
  if (pantallaActual == 0) {
    mostrarPantallaUno();
  }
  else if (pantallaActual == 1) {
    mostrarPantallaDos();
  }
  else {
    mostrarPantallaTres();
  }
}

// ==================== PULSADOR ====================

void leerPulsador() {
  int lectura = digitalRead(pinBoton);

  // Detecta el flanco de bajada. El pulsador trabaja con INPUT_PULLUP.
  if (ultimoEstado == HIGH && lectura == LOW) {
    cantidadPulsaciones++;
    pantallaActual = 2;
    previousMillisPantalla = millis();

    Serial.print("Cantidad de pulsaciones: ");
    Serial.println(cantidadPulsaciones);

    delay(20);  // antirrebote simple utilizado en el ejemplo de la catedra
  }

  ultimoEstado = lectura;
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);

  pinMode(pinBoton, INPUT_PULLUP);

  // Inicializar pantalla OLED
  display.begin(0x3C, true);  // Direccion por defecto: 0x3C
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setTextWrap(false);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("TP IoT - Grupo 06");
  display.println("Sketch 2");
  display.display();
  delay(1000);

  conectarWiFi();

  ThingSpeak.begin(client);

  configurarOTA();

  leerThingSpeak();
  actualizarPantalla();

  previousMillisLectura = millis();
  previousMillisPantalla = millis();
}

// ==================== LOOP ====================

void loop() {
  ArduinoOTA.handle();

  leerPulsador();

  unsigned long currentMillis = millis();

  if (currentMillis - previousMillisLectura >= intervalLectura) {
    previousMillisLectura = currentMillis;
    leerThingSpeak();
    actualizarPantalla();
  }

  if (currentMillis - previousMillisPantalla >= intervalPantalla) {
    previousMillisPantalla = currentMillis;
    pantallaActual++;

    if (pantallaActual > 2) {
      pantallaActual = 0;
    }

    actualizarPantalla();
  }
}

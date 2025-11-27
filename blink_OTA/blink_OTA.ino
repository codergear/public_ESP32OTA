// ESP32 Dev Module (WROOM)

#include <WiFi.h>
#include <WiFiClientSecure.h>   // ← NECESARIO PARA HTTPS
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

Preferences preferences;

// ===============================
// CONFIGURACIÓN DEL PROYECTO
// ===============================

// Cambia este número SOLO cuando flashees manualmente
#define VERSION_LOCAL 1   // ejemplo: 1 → 2 → 3

const char* GITHUB_API_URL =
  "https://api.github.com/repos/codergear/public_ESP32OTA/releases/latest";

// Pines
const int LED_PIN = 2;

// Variables WiFi
String ssid = "";
String pass = "";

// ===============================
// UTILITIES
// ===============================
int versionStringToInt(const String& tag) {
  if (tag.length() < 2) return 0;      // por si viene mal formado
  return tag.substring(1).toInt();     // de "v14" → 14
}

// ===============================
// NVS: LEER CREDENCIALES
// ===============================
bool leerCredenciales() {
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  preferences.end();

  return ssid.length() > 0 && pass.length() > 0;
}

// ===============================
// NVS: GUARDAR CREDENCIALES
// ===============================
void guardarCredenciales(String s, String p) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", s);
  preferences.putString("pass", p);
  preferences.end();
}

// ===============================
// PEDIR CREDENCIALES POR SERIAL
// ===============================
void modoConfiguracion() {
  Serial.println("\n=== CONFIGURACIÓN DE WIFI ===");

  Serial.print("Ingrese SSID: ");
  while (!Serial.available()) {}
  ssid = Serial.readStringUntil('\n');
  ssid.trim();

  Serial.print("Ingrese Password: ");
  while (!Serial.available()) {}
  pass = Serial.readStringUntil('\n');
  pass.trim();

  Serial.println("\nGuardando en NVS...");
  guardarCredenciales(ssid, pass);

  Serial.println("Reiniciando...");
  delay(1500);
  ESP.restart();
}

// ===============================
// CONEXIÓN A WIFI
// ===============================
void conectarWiFi() {
  Serial.printf("Conectando a WiFi: %s\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 30) {
    delay(300);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nNo se pudo conectar. entrando en modo configuración...");
    modoConfiguracion();
  }
}

// ===============================
// CONSULTAR VERSION REMOTA
// ===============================
String versionRemota = "";
String urlFirmware = "";

bool comprobarVersion() {
  Serial.println("\nConsultando versión remota en GitHub...");

  WiFiClientSecure client;
  client.setInsecure();  // GitHub usa https

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, GITHUB_API_URL)) {
    Serial.println("❌ No se pudo iniciar conexión");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("❌ Error HTTP: %d\n", code);
    http.end();
    return false;
  }

  String json = http.getString();
  http.end();

  StaticJsonDocument<4096> doc;
  auto err = deserializeJson(doc, json);
  if (err) {
    Serial.println("❌ Error al parsear JSON");
    return false;
  }

  versionRemota = doc["tag_name"].as<String>();
  urlFirmware   = doc["assets"][0]["browser_download_url"].as<String>();

  Serial.println("Versión remota: " + versionRemota);
  Serial.println("URL firmware:   " + urlFirmware);

  int vLocal  = VERSION_LOCAL;
  int vRemota = versionStringToInt(versionRemota);

  Serial.printf("Comparando versiones: local=%d  remota=%d\n", vLocal, vRemota);

  return vRemota > vLocal;
}

// ===============================
// OTA
// ===============================
bool realizarOTA(const String& url) {
  Serial.println("\nIniciando OTA desde:");
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.println("❌ Error al conectar al servidor OTA");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("❌ Error HTTP en OTA: %d\n", code);
    http.end();
    return false;
  }

  int length = http.getSize();
  if (length <= 0) {
    Serial.println("❌ Tamaño inválido");
    http.end();
    return false;
  }

  Serial.printf("Tamaño firmware: %d bytes\n", length);

  bool ok = Update.begin(length);
  if (!ok) {
    Serial.println("❌ No se pudo iniciar OTA");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != length) {
    Serial.printf("❌ Error escribiendo (%d/%d)\n", written, length);
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("❌ Error OTA: %s\n", Update.errorString());
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("❌ OTA incompleta");
    http.end();
    return false;
  }

  Serial.println("✔ OTA completada, reiniciando...");
  delay(1000);
  ESP.restart();
  return true;
}

// ===============================
// SETUP
// ===============================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);

  Serial.println("\nIniciando...");

  // Leer credenciales
  if (!leerCredenciales()) {
    Serial.println("\nNo hay credenciales en NVS.");
    modoConfiguracion();
  }

  // Conectar
  conectarWiFi();

  // Comprobar versión OTA
  if (comprobarVersion()) {
    Serial.println("⚠ Nueva versión disponible → Actualizando...");
    realizarOTA(urlFirmware);
  } else {
    Serial.println("✔ Ya estás en la última versión.");
  }
}

// ===============================
// LOOP
// ===============================
void loop() {
  // Código original de parpadeo
  digitalWrite(LED_PIN, HIGH);
  delay(500);
  digitalWrite(LED_PIN, LOW);
  delay(500);
}

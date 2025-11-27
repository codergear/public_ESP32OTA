// ESP32 Dev Module (WROOM)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

Preferences preferences;

// ===============================
// CONFIGURACION DEL PROYECTO
// ===============================

const char* GITHUB_API_URL =
  "https://api.github.com/repos/codergear/public_ESP32OTA/releases/latest";

const int LED_PIN = 2;

// Variables WiFi
String ssid = "";
String pass = "";

// Version local almacenada en NVS
int versionLocal = 0;

// Version remota y URL
String versionRemota = "";
String urlFirmware = "";

// ===============================
// UTILIDADES
// ===============================
int versionStringToInt(const String& tag) {
  if (tag.length() < 2) return 0;
  return tag.substring(1).toInt();   // v12 -> 12
}

// ===============================
// NVS: LEER CREDENCIALES Y VERSION
// ===============================
bool leerDatosNVS() {
  // Leer WiFi
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  preferences.end();

  // Leer version
  preferences.begin("version", true);
  versionLocal = preferences.getInt("local", 0);
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
// NVS: GUARDAR VERSION
// ===============================
void guardarVersionLocal(int v) {
  preferences.begin("version", false);
  preferences.putInt("local", v);
  preferences.end();
}

// ===============================
// CONFIGURACION POR SERIAL
// ===============================
void modoConfiguracion() {
  Serial.println("");
  Serial.println("=== CONFIGURACION DE WIFI ===");

  Serial.print("SSID: ");
  while (!Serial.available()) {}
  ssid = Serial.readStringUntil('\n');
  ssid.trim();

  Serial.print("Password: ");
  while (!Serial.available()) {}
  pass = Serial.readStringUntil('\n');
  pass.trim();

  guardarCredenciales(ssid, pass);

  Serial.println("Reiniciando...");
  delay(1500);
  ESP.restart();
}

// ===============================
// CONECTAR A WIFI
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
    Serial.println("\nNo se pudo conectar. Entrando en modo configuracion...");
    modoConfiguracion();
  }
}

// ===============================
// CONSULTAR VERSION REMOTA
// ===============================
bool comprobarVersion() {
  Serial.println("");
  Serial.println("Consultando version remota en GitHub...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, GITHUB_API_URL)) {
    Serial.println("Error: No se pudo iniciar conexion");
    return false;
  }

  http.addHeader("User-Agent", "ESP32-Updater");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Error HTTP: %d\n", code);
    http.end();
    return false;
  }

  String json = http.getString();
  http.end();

  StaticJsonDocument<8192> doc;
  auto err = deserializeJson(doc, json);

  if (err) {
    Serial.println("Error JSON");
    return false;
  }

  versionRemota = doc["tag_name"].as<String>();
  urlFirmware   = doc["assets"][0]["browser_download_url"].as<String>();

  Serial.println("Version remota: " + versionRemota);
  Serial.println("URL Firmware:   " + urlFirmware);

  int vRemota = versionStringToInt(versionRemota);

  Serial.printf("Comparando versiones: local=%d  remota=%d\n",
                versionLocal, vRemota);

  return vRemota > versionLocal;
}

// ===============================
// OTA
// ===============================
bool realizarOTA(const String& url) {
  Serial.println("");
  Serial.println("Iniciando OTA desde:");
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.println("Error OTA begin()");
    return false;
  }

  http.addHeader("User-Agent", "ESP32-Updater");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Error HTTP OTA: %d\n", code);
    http.end();
    return false;
  }

  int length = http.getSize();
  if (length <= 0) {
    Serial.println("Error: Tamano invalido");
    http.end();
    return false;
  }

  Serial.printf("Tamano firmware: %d bytes\n", length);

  if (!Update.begin(length)) {
    Serial.println("Error: Update.begin()");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != length) {
    Serial.printf("Error escribiendo (%d/%d)\n", written, length);
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("Error OTA: %s\n", Update.errorString());
    http.end();
    return false;
  }

  Serial.println("OTA completada. Guardando version...");
  int vRemota = versionStringToInt(versionRemota);
  guardarVersionLocal(vRemota);

  Serial.println("Reiniciando...");
  delay(1200);
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

  Serial.println("");
  Serial.println("Iniciando...");

  if (!leerDatosNVS()) {
    Serial.println("No hay credenciales de WiFi. Se requiere configuracion.");
    modoConfiguracion();
  }

  conectarWiFi();

  Serial.printf("Version local almacenada: %d\n", versionLocal);

  // Asegurar lectura de version local ANTES del primer check
  preferences.begin("version", true);
  versionLocal = preferences.getInt("local", 0);
  preferences.end();

  if (comprobarVersion()) {
    Serial.println("Hay nueva version disponible. Ejecutando OTA...");
    realizarOTA(urlFirmware);
  } else {
    Serial.println("Ya estas en la ultima version.");
  }
}

// ===============================
// LOOP
// ===============================
void loop() {
  // Blink original
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);
  delay(300);

  // Comandos por puerto serie
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.equalsIgnoreCase("ota")) {
      Serial.println("Comando OTA recibido.");

      preferences.begin("version", true);
      versionLocal = preferences.getInt("local", 0);
      preferences.end();

      if (comprobarVersion()) {
        Serial.println("Nueva version disponible. Ejecutando OTA...");
        realizarOTA(urlFirmware);
      } else {
        Serial.println("No hay nuevas versiones.");
      }
    }
    else if (cmd.equalsIgnoreCase("wifi")) {
      Serial.println("Reconfigurando WiFi...");
      modoConfiguracion();
    }
    else {
      Serial.println("Comandos validos:");
      Serial.println("ota  -> Forzar actualizacion OTA");
      Serial.println("wifi -> Reconfigurar WiFi");
    }
  }
}

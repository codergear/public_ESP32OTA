// ESP32 Dev Module (WROOM)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

Preferences preferences;

// ===============================
// CONFIGURACIÓN DEL PROYECTO
// ===============================

const char* GITHUB_API_URL =
  "https://api.github.com/repos/codergear/public_ESP32OTA/releases/latest";

const int LED_PIN = 2;

// Variables WiFi
String ssid = "";
String pass = "";

// Versión local almacenada en NVS
int versionLocal = 0;

// Version remota y URL de firmware
String versionRemota = "";
String urlFirmware = "";

// ===============================
// UTILITIES
// ===============================
int versionStringToInt(const String& tag) {
  if (tag.length() < 2) return 0; // "v"
  return tag.substring(1).toInt(); // "v14" -> 14
}

// ===============================
// NVS: LEER CREDENCIALES Y VERSIÓN
// ===============================
bool leerDatosNVS() {
  // Leer WiFi
  preferences.begin("wifi", true);
  ssid = preferences.getString("ssid", "");
  pass = preferences.getString("pass", "");
  preferences.end();

  // Leer versión
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
// NVS: GUARDAR VERSIÓN
// ===============================
void guardarVersionLocal(int v) {
  preferences.begin("version", false);
  preferences.putInt("local", v);
  preferences.end();
}

// ===============================
// CONFIGURACIÓN POR SERIAL
// ===============================
void modoConfiguracion() {
  Serial.println("\n=== CONFIGURACIÓN DE WIFI ===");

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
    Serial.print("\nWiFi OK. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n No conecta. Modo configuración...");
    modoConfiguracion();
  }
}

// ===============================
// CONSULTAR VERSION REMOTA
// ===============================
bool comprobarVersion() {
  Serial.println("\nConsultando versión remota en GitHub...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, GITHUB_API_URL)) {
    Serial.println(" No se pudo iniciar conexión");
    return false;
  }

  // >>> CAMBIO IMPRESCINDIBLE <<<
  http.addHeader("User-Agent", "ESP32-Updater");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf(" Error HTTP: %d\n", code);
    http.end();
    return false;
  }

  String json = http.getString();
  http.end();

  StaticJsonDocument<8192> doc;
  auto err = deserializeJson(doc, json);

  if (err) {
    Serial.println(" Error JSON");
    return false;
  }

  versionRemota = doc["tag_name"].as<String>();
  urlFirmware   = doc["assets"][0]["browser_download_url"].as<String>();

  Serial.println("Versión remota: " + versionRemota);
  Serial.println("URL Firmware:  " + urlFirmware);

  int vRemota = versionStringToInt(versionRemota);

  Serial.printf("Comparando versiones: local=%d   remota=%d\n",
                versionLocal, vRemota);

  return vRemota > versionLocal;
}

// ===============================
// OTA
// ===============================
bool realizarOTA(const String& url) {
  Serial.println("\nIniciando OTA:");
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.println(" Error OTA begin()");
    return false;
  }

  // >>> CAMBIO IMPRESCINDIBLE <<<
  http.addHeader("User-Agent", "ESP32-Updater");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf(" Error HTTP OTA: %d\n", code);
    http.end();
    return false;
  }

  int length = http.getSize();
  if (length <= 0) {
    Serial.println(" Tamaño inválido");
    http.end();
    return false;
  }

  Serial.printf("Tamaño firmware: %d bytes\n", length);

  if (!Update.begin(length)) {
    Serial.println(" Error Update.begin()");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != length) {
    Serial.printf(" Error escribiendo (%d/%d)\n", written, length);
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf(" OTA Error: %s\n", Update.errorString());
    http.end();
    return false;
  }

  Serial.println(" OTA completada. Guardando versión...");

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

  Serial.println("\nIniciando...");

  if (!leerDatosNVS()) {
    Serial.println("No hay WiFi. Modo configuración...");
    modoConfiguracion();
  }

  conectarWiFi();

  Serial.printf("Versión local almacenada: %d\n", versionLocal);

  if (comprobarVersion()) {
    Serial.println(" Nueva versión disponible → Actualizando...");
    realizarOTA(urlFirmware);
  } else {
    Serial.println(" Ya estás en la última versión.");
  }
}

// ===============================
// LOOP
// ===============================
void loop() {
  // Parpadeo original
  digitalWrite(LED_PIN, HIGH);
  delay(300);
  digitalWrite(LED_PIN, LOW);
  delay(300);

  // Comandos por Serial
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.equalsIgnoreCase("ota")) {
      Serial.println("\n Comando OTA recibido → revisando...");
      versionLocal = preferences.getInt("local", 0);

      if (comprobarVersion()) {
        Serial.println("\n Nueva versión → OTA...");
        realizarOTA(urlFirmware);
      } else {
        Serial.println("\n No hay nuevas versiones.");
      }
    }
    else if (cmd.equalsIgnoreCase("wifi")) {
      Serial.println("\n Reconfigurar WiFi...");
      modoConfiguracion();
    }
    else {
      Serial.println("\n Comando no reconocido: ota / wifi");
    }
  }
}

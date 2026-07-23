// ESP32 Dev Module (WROOM) - puerto a ESP-IDF de blink_OTA.ino

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "esp_http_client.h"
#include "esp_https_ota.h"

#include "cJSON.h"

// ===============================
// CONFIGURACION DEL PROYECTO
// ===============================

static const char *GITHUB_API_URL =
    "https://api.github.com/repos/codergear/public_ESP32OTA/releases?per_page=10";

// Prefijo de tag que identifica los releases de este firmware (ESP-IDF).
// El repo tambien publica releases "arduino-vN" para el proyecto Arduino;
// hay que ignorarlos.
static const char *VERSION_PREFIX = "idf-v";

#define LED_PIN GPIO_NUM_2

// Variables WiFi
static char ssid[64] = {0};
static char pass[64] = {0};

// Version local almacenada en NVS
static int32_t versionLocal = 0;

// Version remota y URL
static char versionRemota[32] = {0};
static char urlFirmware[256] = {0};

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t wifi_event_group;

static void modoConfiguracion(void);
static void conectarWiFi(void);
static bool comprobarVersion(void);
static bool realizarOTA(const char *url);

// ===============================
// UTILIDADES
// ===============================
static int versionStringToInt(const char *tag) {
  size_t prefix_len = strlen(VERSION_PREFIX);
  if (strncmp(tag, VERSION_PREFIX, prefix_len) != 0) return 0;
  return atoi(tag + prefix_len);   // idf-v12 -> 12
}

// Equivalente bloqueante a Serial.readStringUntil('\n') + trim()
static void leerLineaSerial(char *buf, size_t buflen) {
  if (fgets(buf, buflen, stdin) != NULL) {
    buf[strcspn(buf, "\r\n")] = '\0';
  } else {
    buf[0] = '\0';
  }
}

static bool serialDisponible(void) {
  size_t len = 0;
  uart_get_buffered_data_len(UART_NUM_0, &len);
  return len > 0;
}

// ===============================
// NVS: LEER CREDENCIALES Y VERSION
// ===============================
static bool leerDatosNVS(void) {
  nvs_handle_t h;
  size_t len;

  ssid[0] = '\0';
  pass[0] = '\0';
  if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
    len = sizeof(ssid);
    nvs_get_str(h, "ssid", ssid, &len);
    len = sizeof(pass);
    nvs_get_str(h, "pass", pass, &len);
    nvs_close(h);
  }

  versionLocal = 0;
  if (nvs_open("version", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_i32(h, "local", &versionLocal);
    nvs_close(h);
  }

  return strlen(ssid) > 0 && strlen(pass) > 0;
}

// ===============================
// NVS: GUARDAR CREDENCIALES
// ===============================
static void guardarCredenciales(const char *s, const char *p) {
  nvs_handle_t h;
  nvs_open("wifi", NVS_READWRITE, &h);
  nvs_set_str(h, "ssid", s);
  nvs_set_str(h, "pass", p);
  nvs_commit(h);
  nvs_close(h);
}

// ===============================
// NVS: GUARDAR VERSION
// ===============================
static void guardarVersionLocal(int v) {
  nvs_handle_t h;
  nvs_open("version", NVS_READWRITE, &h);
  nvs_set_i32(h, "local", v);
  nvs_commit(h);
  nvs_close(h);
}

// ===============================
// CONFIGURACION POR SERIAL
// ===============================
static void modoConfiguracion(void) {
  char buf[128];

  printf("\n=== CONFIGURACION DE WIFI ===\n");

  printf("SSID: ");
  fflush(stdout);
  leerLineaSerial(buf, sizeof(buf));
  strncpy(ssid, buf, sizeof(ssid) - 1);
  ssid[sizeof(ssid) - 1] = '\0';

  printf("Password: ");
  fflush(stdout);
  leerLineaSerial(buf, sizeof(buf));
  strncpy(pass, buf, sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = '\0';

  guardarCredenciales(ssid, pass);

  printf("Reiniciando...\n");
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
}

// ===============================
// CONECTAR A WIFI
// ===============================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
  static int intentos = 0;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (intentos < 30) {
      vTaskDelay(pdMS_TO_TICKS(300));
      printf(".");
      fflush(stdout);
      intentos++;
      esp_wifi_connect();
    } else {
      xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    printf("\nWiFi conectado. IP: " IPSTR "\n", IP2STR(&event->ip_info.ip));
    intentos = 0;
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

static void conectarWiFi(void) {
  printf("Conectando a WiFi: %s\n", ssid);

  wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                       &wifi_event_handler, NULL, &instance_any_id);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                       &wifi_event_handler, NULL, &instance_got_ip);

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE, pdFALSE, portMAX_DELAY);

  if (bits & WIFI_FAIL_BIT) {
    printf("\nNo se pudo conectar. Entrando en modo configuracion...\n");
    modoConfiguracion();
  }
}

// ===============================
// CONSULTAR VERSION REMOTA
// ===============================
#define HTTP_RESPONSE_BUFFER_SIZE 16384
static char http_response_buffer[HTTP_RESPONSE_BUFFER_SIZE];
static int http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (http_response_len + evt->data_len < HTTP_RESPONSE_BUFFER_SIZE) {
      memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
      http_response_len += evt->data_len;
      http_response_buffer[http_response_len] = '\0';
    }
  }
  return ESP_OK;
}

static bool comprobarVersion(void) {
  printf("\nConsultando version remota en GitHub...\n");

  http_response_len = 0;
  http_response_buffer[0] = '\0';

  esp_http_client_config_t config = {
      .url = GITHUB_API_URL,
      .event_handler = http_event_handler,
      .skip_cert_common_name_check = true,
      .user_agent = "ESP32-Updater",
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    printf("Error: No se pudo iniciar conexion\n");
    return false;
  }

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200) {
    printf("Error HTTP: %d\n", status);
    return false;
  }

  cJSON *root = cJSON_Parse(http_response_buffer);
  if (root == NULL) {
    printf("Error JSON\n");
    return false;
  }

  bool encontrado = false;
  cJSON *release;
  cJSON_ArrayForEach(release, root) {
    cJSON *tag = cJSON_GetObjectItem(release, "tag_name");
    if (!cJSON_IsString(tag)) continue;

    if (strncmp(tag->valuestring, VERSION_PREFIX, strlen(VERSION_PREFIX)) == 0) {
      cJSON *assets = cJSON_GetObjectItem(release, "assets");
      cJSON *asset0 = (assets && cJSON_GetArraySize(assets) > 0) ? cJSON_GetArrayItem(assets, 0) : NULL;
      cJSON *url = asset0 ? cJSON_GetObjectItem(asset0, "browser_download_url") : NULL;

      if (!cJSON_IsString(url)) continue;

      strncpy(versionRemota, tag->valuestring, sizeof(versionRemota) - 1);
      versionRemota[sizeof(versionRemota) - 1] = '\0';
      strncpy(urlFirmware, url->valuestring, sizeof(urlFirmware) - 1);
      urlFirmware[sizeof(urlFirmware) - 1] = '\0';
      encontrado = true;
      break;
    }
  }

  cJSON_Delete(root);

  if (!encontrado) {
    printf("No se encontro ningun release de ESP-IDF en el repo\n");
    return false;
  }

  printf("Version remota: %s\n", versionRemota);
  printf("URL Firmware:   %s\n", urlFirmware);

  int vRemota = versionStringToInt(versionRemota);
  printf("Comparando versiones: local=%d  remota=%d\n", (int)versionLocal, vRemota);

  return vRemota > versionLocal;
}

// ===============================
// OTA
// ===============================
static bool realizarOTA(const char *url) {
  printf("\nIniciando OTA desde:\n%s\n", url);

  esp_http_client_config_t http_config = {
      .url = url,
      .skip_cert_common_name_check = true,
      .user_agent = "ESP32-Updater",
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &http_config,
  };

  esp_err_t ret = esp_https_ota(&ota_config);
  if (ret != ESP_OK) {
    printf("Error OTA: %s\n", esp_err_to_name(ret));
    return false;
  }

  printf("OTA completada. Guardando version...\n");
  int vRemota = versionStringToInt(versionRemota);
  guardarVersionLocal(vRemota);

  printf("Reiniciando...\n");
  vTaskDelay(pdMS_TO_TICKS(1200));
  esp_restart();
  return true;
}

// ===============================
// SETUP
// ===============================
void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Habilita lectura bloqueante sobre stdin, equivalente a Serial.available()/readStringUntil()
  uart_vfs_dev_use_driver(UART_NUM_0);

  gpio_reset_pin(LED_PIN);
  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

  printf("\nIniciando...\n");

  if (!leerDatosNVS()) {
    printf("No hay credenciales de WiFi. Se requiere configuracion.\n");
    modoConfiguracion();
  }

  conectarWiFi();

  printf("Version local almacenada: %d\n", (int)versionLocal);

  // Asegurar lectura de version local ANTES del primer check
  nvs_handle_t h;
  if (nvs_open("version", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_i32(h, "local", &versionLocal);
    nvs_close(h);
  }

  if (comprobarVersion()) {
    printf("Hay nueva version disponible. Ejecutando OTA...\n");
    realizarOTA(urlFirmware);
  } else {
    printf("Ya estas en la ultima version.\n");
  }

  // ===============================
  // LOOP
  // ===============================
  while (1) {
    // Blink original
    gpio_set_level(LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(300));

    // Comandos por puerto serie
    if (serialDisponible()) {
      char cmd[64];
      leerLineaSerial(cmd, sizeof(cmd));

      if (strcasecmp(cmd, "ota") == 0) {
        printf("Comando OTA recibido.\n");

        nvs_handle_t hv;
        if (nvs_open("version", NVS_READONLY, &hv) == ESP_OK) {
          nvs_get_i32(hv, "local", &versionLocal);
          nvs_close(hv);
        }

        if (comprobarVersion()) {
          printf("Nueva version disponible. Ejecutando OTA...\n");
          realizarOTA(urlFirmware);
        } else {
          printf("No hay nuevas versiones.\n");
        }
      } else if (strcasecmp(cmd, "wifi") == 0) {
        printf("Reconfigurando WiFi...\n");
        modoConfiguracion();
      } else {
        printf("Comandos validos:\n");
        printf("ota  -> Forzar actualizacion OTA\n");
        printf("wifi -> Reconfigurar WiFi\n");
      }
    }
  }
}

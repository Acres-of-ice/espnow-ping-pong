#include "web.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h" // For MACSTR and MAC2STR macros
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_DEVICES 20
#define RSSI_HISTORY_SIZE 10

static const char *TAG = "espnow-web";

// Mutex for protecting RSSI data access
static SemaphoreHandle_t rssi_mutex;

// Device RSSI tracking structure
typedef struct {
  uint8_t mac[ESP_NOW_ETH_ALEN];       // MAC address
  char mac_str[18];                    // MAC address string
  int rssi;                            // Current RSSI
  int rssi_history[RSSI_HISTORY_SIZE]; // History for graph
  int history_index;                   // Current position in history
  time_t last_seen;                    // Last time device was seen
  bool active;                         // Whether this device is active
} device_rssi_t;

// Device list
static device_rssi_t device_list[MAX_DEVICES];
static int device_count = 0;

// HTML, CSS, and JavaScript for the web interface
static const char *html_template =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "    <title>ESP-NOW RSSI Monitor</title>\n"
    "    <meta name=\"viewport\" content=\"width=device-width, "
    "initial-scale=1\">\n"
    "    <style>\n"
    "        body { font-family: Arial, sans-serif; margin: 0; padding: 20px; "
    "}\n"
    "        h1 { color: #333; }\n"
    "        .device { margin-bottom: 15px; padding: 15px; border: 1px solid "
    "#ddd; border-radius: 5px; }\n"
    "        .device-header { display: flex; justify-content: space-between; "
    "}\n"
    "        .rssi-value { font-size: 24px; font-weight: bold; }\n"
    "        .rssi-good { color: green; }\n"
    "        .rssi-medium { color: orange; }\n"
    "        .rssi-bad { color: red; }\n"
    "        .last-seen { color: #666; font-size: 12px; }\n"
    "        .chart-container { height: 50px; width: 100%%; margin-top: 10px; "
    "}\n"
    "    </style>\n"
    "</head>\n"
    "<body>\n"
    "    <h1>ESP-NOW RSSI Monitor</h1>\n"
    "    <div id=\"devices\">Loading...</div>\n"
    "\n"
    "    <script>\n"
    "        function updateRSSI() {\n"
    "            fetch('/rssi')\n"
    "                .then(response => response.json())\n"
    "                .then(data => {\n"
    "                    const devicesDiv = "
    "document.getElementById('devices');\n"
    "                    if (data.devices.length === 0) {\n"
    "                        devicesDiv.innerHTML = '<p>No devices detected "
    "yet.</p>';\n"
    "                        return;\n"
    "                    }\n"
    "\n"
    "                    let html = '';\n"
    "                    data.devices.forEach(device => {\n"
    "                        let rssiClass = 'rssi-bad';\n"
    "                        if (device.rssi > -70) rssiClass = 'rssi-good';\n"
    "                        else if (device.rssi > -85) rssiClass = "
    "'rssi-medium';\n"
    "\n"
    "                        let lastSeen = 'just now';\n"
    "                        if (device.last_seen > 0) {\n"
    "                            lastSeen = device.last_seen + ' seconds "
    "ago';\n"
    "                        }\n"
    "\n"
    "                        html += `\n"
    "                            <div class=\"device\">\n"
    "                                <div class=\"device-header\">\n"
    "                                    <div>${device.mac}</div>\n"
    "                                    <div class=\"rssi-value "
    "${rssiClass}\">${device.rssi} dBm</div>\n"
    "                                </div>\n"
    "                                <div class=\"last-seen\">Last seen: "
    "${lastSeen}</div>\n"
    "                            </div>\n"
    "                        `;\n"
    "                    });\n"
    "\n"
    "                    devicesDiv.innerHTML = html;\n"
    "                })\n"
    "                .catch(error => console.error('Error fetching RSSI "
    "data:', error));\n"
    "        }\n"
    "\n"
    "        // Update every second\n"
    "        updateRSSI();\n"
    "        setInterval(updateRSSI, 1000);\n"
    "    </script>\n"
    "</body>\n"
    "</html>";

// Find or add a device to our tracking list
static int find_or_add_device(const uint8_t *mac_addr) {
  char mac_str[18];
  sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", mac_addr[0], mac_addr[1],
          mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);

  // First, look for existing device
  for (int i = 0; i < device_count; i++) {
    if (memcmp(device_list[i].mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
      return i;
    }
  }

  // If not found and we have space, add it
  if (device_count < MAX_DEVICES) {
    int new_idx = device_count++;
    memcpy(device_list[new_idx].mac, mac_addr, ESP_NOW_ETH_ALEN);
    strcpy(device_list[new_idx].mac_str, mac_str);
    device_list[new_idx].rssi = -120; // Default weak signal
    device_list[new_idx].last_seen = 0;
    device_list[new_idx].active = true;
    device_list[new_idx].history_index = 0;

    // Initialize history with default values
    for (int i = 0; i < RSSI_HISTORY_SIZE; i++) {
      device_list[new_idx].rssi_history[i] = -120;
    }

    ESP_LOGI(TAG, "Added new device: %s", mac_str);
    return new_idx;
  }

  // No space left
  ESP_LOGW(TAG, "Device list full, cannot add: %s", mac_str);
  return -1;
}

// Update device RSSI - this is the public API function
esp_err_t rssi_web_update_device(const uint8_t *mac_addr, int rssi) {
  if (!rssi_mutex) {
    return ESP_ERR_INVALID_STATE;
  }

  int device_idx = find_or_add_device(mac_addr);
  if (device_idx < 0) {
    return ESP_ERR_NO_MEM;
  }

  xSemaphoreTake(rssi_mutex, portMAX_DELAY);

  device_list[device_idx].rssi = rssi;
  device_list[device_idx].last_seen = 0;
  device_list[device_idx].active = true;

  // Update history
  device_list[device_idx].rssi_history[device_list[device_idx].history_index] =
      rssi;
  device_list[device_idx].history_index =
      (device_list[device_idx].history_index + 1) % RSSI_HISTORY_SIZE;

  xSemaphoreGive(rssi_mutex);

  return ESP_OK;
}

// ESP-NOW receive callback for the RSSI web server
void rssi_web_espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                             const uint8_t *data, int len) {
  uint8_t *mac_addr = recv_info->src_addr;

  // Get RSSI if available via the rx_ctrl field
  int rssi = -120; // Default value if RSSI not available
  if (recv_info->rx_ctrl != NULL) {
    rssi = recv_info->rx_ctrl->rssi;
  }

  // Update RSSI data
  rssi_web_update_device(mac_addr, rssi);

  // Print RSSI to console
  ESP_LOGI(TAG, "Message from " MACSTR ", RSSI: %d dBm", MAC2STR(mac_addr),
           rssi);
}

// HTTP GET handler for the root page
static esp_err_t root_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html_template, strlen(html_template));
  return ESP_OK;
}

// HTTP GET handler for RSSI data in JSON format
static esp_err_t rssi_get_handler(httpd_req_t *req) {
  char json_response[2048]; // Make sure this is large enough
  char *p = json_response;

  xSemaphoreTake(rssi_mutex, portMAX_DELAY);

  // Start JSON
  p += sprintf(p, "{\"devices\":[");

  // Add each active device
  bool first = true;
  for (int i = 0; i < device_count; i++) {
    if (device_list[i].active) {
      if (!first) {
        p += sprintf(p, ",");
      }
      first = false;

      // Basic device info
      p += sprintf(p, "{\"mac\":\"%s\",\"rssi\":%d,\"last_seen\":%lld",
                   device_list[i].mac_str, device_list[i].rssi,
                   device_list[i].last_seen);

      // Add history data for graphs
      p += sprintf(p, ",\"history\":[");
      for (int j = 0; j < RSSI_HISTORY_SIZE; j++) {
        if (j > 0)
          p += sprintf(p, ",");
        int idx = (device_list[i].history_index + j) % RSSI_HISTORY_SIZE;
        p += sprintf(p, "%d", device_list[i].rssi_history[idx]);
      }
      p += sprintf(p, "]");

      p += sprintf(p, "}");
    }
  }

  // End JSON
  p += sprintf(p, "]}");

  xSemaphoreGive(rssi_mutex);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_response, strlen(json_response));

  return ESP_OK;
}

// Age device "last seen" timestamps in background
static void age_device_task(void *pvParameter) {
  while (1) {
    xSemaphoreTake(rssi_mutex, portMAX_DELAY);

    for (int i = 0; i < device_count; i++) {
      if (device_list[i].active) {
        device_list[i].last_seen++;

        // Mark inactive after 30 seconds of no activity
        if (device_list[i].last_seen > 30) {
          device_list[i].active = false;
          ESP_LOGI(TAG, "Device inactive: %s", device_list[i].mac_str);
        }
      }
    }

    xSemaphoreGive(rssi_mutex);

    vTaskDelay(1000 / portTICK_PERIOD_MS); // Update every second
  }
}

// Start HTTP server
httpd_handle_t rssi_web_start(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;

  ESP_LOGI(TAG, "Starting HTTP server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    // URI handlers
    httpd_uri_t root_uri = {.uri = "/",
                            .method = HTTP_GET,
                            .handler = root_get_handler,
                            .user_ctx = NULL};
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t rssi_uri = {.uri = "/rssi",
                            .method = HTTP_GET,
                            .handler = rssi_get_handler,
                            .user_ctx = NULL};
    httpd_register_uri_handler(server, &rssi_uri);

    return server;
  }

  ESP_LOGI(TAG, "Error starting server!");
  return NULL;
}

// Stop HTTP server
void rssi_web_stop(httpd_handle_t server) {
  if (server != NULL) {
    httpd_stop(server);
  }
}

// WiFi AP event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " joined, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " left, AID=%d", MAC2STR(event->mac),
             event->aid);
  }
}

// Initialize the RSSI web server
esp_err_t rssi_web_init(const char *ap_ssid, const char *ap_pass,
                        uint8_t ap_channel, bool use_existing_wifi) {
  esp_err_t ret = ESP_OK;

  // Create mutex for RSSI data if it doesn't exist
  if (rssi_mutex == NULL) {
    rssi_mutex = xSemaphoreCreateMutex();
    if (rssi_mutex == NULL) {
      return ESP_ERR_NO_MEM;
    }
  }

  // Initialize WiFi in AP mode if it's not already initialized
  if (!use_existing_wifi) {
    ret = esp_netif_init();
    if (ret != ESP_OK)
      return ret;

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK)
      return ret;

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK)
      return ret;

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK)
      return ret;

    wifi_config_t wifi_config = {
        .ap = {.channel = ap_channel,
               .max_connection = 4,
               .authmode = WIFI_AUTH_WPA_WPA2_PSK},
    };

    // Set SSID and password
    strncpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);
    strncpy((char *)wifi_config.ap.password, ap_pass,
            sizeof(wifi_config.ap.password));

    // If password is empty, use open authentication
    if (strlen(ap_pass) == 0) {
      wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ret = esp_wifi_set_mode(
        WIFI_MODE_APSTA); // Use APSTA mode to support both AP and ESP-NOW
    if (ret != ESP_OK)
      return ret;

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK)
      return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK)
      return ret;

    ESP_LOGI(TAG, "WiFi AP started with SSID: %s, channel: %d", ap_ssid,
             ap_channel);
  } else {
    // If WiFi is already initialized, just make sure we're in the right mode
    // for ESP-NOW
    wifi_mode_t mode;
    ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK)
      return ret;

    if (mode == WIFI_MODE_STA) {
      // If in STA mode, switch to APSTA to support the web server
      ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
      if (ret != ESP_OK)
        return ret;

      // Configure the AP
      wifi_config_t wifi_config = {
          .ap = {.channel = ap_channel,
                 .max_connection = 4,
                 .authmode = WIFI_AUTH_WPA_WPA2_PSK},
      };

      // Set SSID and password
      strncpy((char *)wifi_config.ap.ssid, ap_ssid,
              sizeof(wifi_config.ap.ssid));
      wifi_config.ap.ssid_len = strlen(ap_ssid);
      strncpy((char *)wifi_config.ap.password, ap_pass,
              sizeof(wifi_config.ap.password));

      // If password is empty, use open authentication
      if (strlen(ap_pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
      }

      ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
      if (ret != ESP_OK)
        return ret;

      ESP_LOGI(TAG, "Added AP with SSID: %s to existing WiFi", ap_ssid);
    }
  }

  // Start background task to age RSSI readings
  xTaskCreate(age_device_task, "age_device_task", 2048, NULL, 5, NULL);

  return ESP_OK;
}

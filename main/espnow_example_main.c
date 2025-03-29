/* ESPNOW Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
   This example shows how to use ESPNOW.
   Prepare two device, one for sending ESPNOW data and another for receiving
   ESPNOW data.
*/
#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "espnow_example.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "lwip/ip4_addr.h"
#include <stdarg.h>
#include <stdio.h>

#include "nvs_flash.h"
#include "web.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static httpd_handle_t server = NULL;
// Add this global variable to store own MAC address
static uint8_t s_own_mac[ESP_NOW_ETH_ALEN] = {0};
// Add these global variables near the top of the file, after other globals
static int64_t s_last_recv_time = 0;
static bool s_communication_active = true;
#define RESPONSE_TIMEOUT_MS 60000 // 1 minute timeout
#define CONFIG_ESPNOW_MAX_PEERS 20
#define ESPNOW_DISCOVERY_TIMEOUT_MS 30000 // 30 seconds timeout for discovery
//// For peer discovery and management
static bool s_peer_discovery_complete = false;
static uint8_t s_discovered_peers[CONFIG_ESPNOW_MAX_PEERS][ESP_NOW_ETH_ALEN] = {
    0};
static int s_discovered_peer_count = 0;
static char s_pcb_name[32] = {0};
typedef struct {
  uint8_t mac[ESP_NOW_ETH_ALEN];
  char pcb_name[MAX_PCB_NAME_LENGTH];
  bool has_pcb_name;
} peer_info_t;

static peer_info_t s_peer_info[CONFIG_ESPNOW_MAX_PEERS] = {0};

#define ESPNOW_MAXDELAY 512
// Fpor power saving
#define CONFIG_ESPNOW_SEND_DELAY                                               \
  1000 // Increase delay between sends (milliseconds)

static const char *TAG = "espnow_example";

static QueueHandle_t s_example_espnow_queue;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                            0xFF, 0xFF, 0xFF};
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = {0, 0};

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

// Array to hold trusted MAC addresses
static uint8_t s_trusted_mac_list[5][ESP_NOW_ETH_ALEN] = {0};
static int s_trusted_mac_count = 0;

const char *get_pcb_name(void) {
  if (s_pcb_name[0] == '\0') {
    // Initialize with Kconfig value if not set yet
    strncpy(s_pcb_name, CONFIG_ESPNOW_PCB_NAME, sizeof(s_pcb_name) - 1);
    s_pcb_name[sizeof(s_pcb_name) - 1] = '\0'; // Ensure null termination
  }
  return s_pcb_name;
}

// Add two functions for peer PCB name management:
static void store_peer_pcb_name(const uint8_t *mac_addr, const char *pcb_name) {
  for (int i = 0; i < s_discovered_peer_count; i++) {
    if (memcmp(s_peer_info[i].mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
      // Update existing entry
      strncpy(s_peer_info[i].pcb_name, pcb_name, MAX_PCB_NAME_LENGTH - 1);
      s_peer_info[i].pcb_name[MAX_PCB_NAME_LENGTH - 1] = '\0';
      s_peer_info[i].has_pcb_name = true;
      return;
    }
  }

  // Add new entry if we have space
  if (s_discovered_peer_count < CONFIG_ESPNOW_MAX_PEERS) {
    memcpy(s_peer_info[s_discovered_peer_count].mac, mac_addr,
           ESP_NOW_ETH_ALEN);
    strncpy(s_peer_info[s_discovered_peer_count].pcb_name, pcb_name,
            MAX_PCB_NAME_LENGTH - 1);
    s_peer_info[s_discovered_peer_count].pcb_name[MAX_PCB_NAME_LENGTH - 1] =
        '\0';
    s_peer_info[s_discovered_peer_count].has_pcb_name = true;
    // Note: Do not increment s_discovered_peer_count here, as that's managed
    // elsewhere
  }
}

// Modify the example_espnow_data_prepare function to include PCB name:
void example_espnow_data_prepare(example_espnow_send_param_t *send_param) {
  example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

  assert(send_param->len >= sizeof(example_espnow_data_t));

  buf->type = IS_BROADCAST_ADDR(send_param->dest_mac)
                  ? EXAMPLE_ESPNOW_DATA_BROADCAST
                  : EXAMPLE_ESPNOW_DATA_UNICAST;
  buf->state = send_param->state;
  buf->seq_num = s_example_espnow_seq[buf->type]++;
  buf->crc = 0;
  buf->magic = send_param->magic;

  // Add PCB name to the message
  strncpy(buf->pcb_name, get_pcb_name(), MAX_PCB_NAME_LENGTH - 1);
  buf->pcb_name[MAX_PCB_NAME_LENGTH - 1] = '\0';

  /* Fill all remaining bytes after the data with random values */
  esp_fill_random(buf->payload,
                  send_param->len - sizeof(example_espnow_data_t));
  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_send_cb(const uint8_t *mac_addr,
                                   esp_now_send_status_t status) {
  example_espnow_event_t evt;
  example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

  if (mac_addr == NULL) {
    ESP_LOGE(TAG, "Send cb arg error");
    return;
  }

  evt.id = EXAMPLE_ESPNOW_SEND_CB;
  memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  send_cb->status = status;
  if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
    ESP_LOGW(TAG, "Send send queue fail");
  }
}

// Function to initialize own MAC address
static void init_own_mac(void) {
  esp_err_t ret = esp_wifi_get_mac(ESPNOW_WIFI_IF, s_own_mac);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get own MAC address, err: %d", ret);
    // Fall back to efuse MAC if STA MAC is not available
    esp_efuse_mac_get_default(s_own_mac);
  }
  ESP_LOGI(TAG, "Own MAC address: " MACSTR, MAC2STR(s_own_mac));
}

// Function to check if a MAC address is the device's own
static bool is_own_mac(const uint8_t *mac_addr) {
  return (memcmp(mac_addr, s_own_mac, ESP_NOW_ETH_ALEN) == 0);
}

const char *get_peer_pcb_name(const uint8_t *mac_addr) {
  static char unknown_peer[32];

  if (is_own_mac(mac_addr)) {
    return get_pcb_name(); // Return our own PCB name
  }

  for (int i = 0; i < s_discovered_peer_count; i++) {
    if (memcmp(s_peer_info[i].mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
      if (s_peer_info[i].has_pcb_name) {
        return s_peer_info[i].pcb_name;
      }
      break;
    }
  }

  // If we don't have the PCB name, return the MAC
  snprintf(unknown_peer, sizeof(unknown_peer), "Unknown-" MACSTR,
           MAC2STR(mac_addr));
  return unknown_peer;
}

// Function to parse MAC address string to bytes
static bool parse_mac_string(const char *mac_str, uint8_t *mac_addr) {
  if (strlen(mac_str) != 17) { // XX:XX:XX:XX:XX:XX = 17 chars
    return false;
  }

  unsigned int values[6] = {0}; // Change to unsigned int instead of uint32_t
  int ret = sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &values[0], &values[1],
                   &values[2], &values[3], &values[4], &values[5]);

  if (ret != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    if (values[i] > 0xFF) {
      return false;
    }
    mac_addr[i] = (uint8_t)values[i];
  }

  return true;
}

// Function to load trusted MAC addresses from Kconfig
static void load_trusted_mac_addresses(void) {
  s_trusted_mac_count = 0;

#if CONFIG_ESPNOW_USE_TRUSTED_MACS
  // Initialize own MAC
  init_own_mac();

  bool found_own_mac = false;

  // Load MAC address 1
  if (s_trusted_mac_count < CONFIG_ESPNOW_TRUSTED_MAC_COUNT) {
    if (parse_mac_string(CONFIG_ESPNOW_TRUSTED_MAC_1,
                         s_trusted_mac_list[s_trusted_mac_count])) {
      // Check if this is our own MAC
      if (is_own_mac(s_trusted_mac_list[s_trusted_mac_count])) {
        ESP_LOGW(TAG, "Trusted MAC 1 is device's own MAC address - will be "
                      "skipped for sending");
        found_own_mac = true;
      }

      ESP_LOGI(TAG, "Trusted MAC %d: " MACSTR, s_trusted_mac_count + 1,
               MAC2STR(s_trusted_mac_list[s_trusted_mac_count]));
      s_trusted_mac_count++;
    } else {
      ESP_LOGE(TAG, "Invalid MAC address format for Trusted MAC 1");
    }
  }

  // Load MAC address 2
  if (s_trusted_mac_count < CONFIG_ESPNOW_TRUSTED_MAC_COUNT) {
    if (parse_mac_string(CONFIG_ESPNOW_TRUSTED_MAC_2,
                         s_trusted_mac_list[s_trusted_mac_count])) {
      // Check if this is our own MAC
      if (is_own_mac(s_trusted_mac_list[s_trusted_mac_count])) {
        ESP_LOGW(TAG, "Trusted MAC 2 is device's own MAC address - will be "
                      "skipped for sending");
        found_own_mac = true;
      }

      ESP_LOGI(TAG, "Trusted MAC %d: " MACSTR, s_trusted_mac_count + 1,
               MAC2STR(s_trusted_mac_list[s_trusted_mac_count]));
      s_trusted_mac_count++;
    } else {
      ESP_LOGE(TAG, "Invalid MAC address format for Trusted MAC 2");
    }
  }

// Load MAC address 3
#if CONFIG_ESPNOW_TRUSTED_MAC_COUNT >= 3
  if (s_trusted_mac_count < CONFIG_ESPNOW_TRUSTED_MAC_COUNT) {
    if (parse_mac_string(CONFIG_ESPNOW_TRUSTED_MAC_4,
                         s_trusted_mac_list[s_trusted_mac_count])) {
      // Check if this is our own MAC
      if (is_own_mac(s_trusted_mac_list[s_trusted_mac_count])) {
        ESP_LOGW(TAG, "Trusted MAC 3 is device's own MAC address - will be "
                      "skipped for sending");
        found_own_mac = true;
      }

      ESP_LOGI(TAG, "Trusted MAC %d: " MACSTR, s_trusted_mac_count + 1,
               MAC2STR(s_trusted_mac_list[s_trusted_mac_count]));
      s_trusted_mac_count++;
    } else {
      ESP_LOGE(TAG, "Invalid MAC address format for Trusted MAC 3");
    }
  }
#endif

// Load MAC address 4
#if CONFIG_ESPNOW_TRUSTED_MAC_COUNT >= 4
  if (s_trusted_mac_count < CONFIG_ESPNOW_TRUSTED_MAC_COUNT) {
    if (parse_mac_string(CONFIG_ESPNOW_TRUSTED_MAC_4,
                         s_trusted_mac_list[s_trusted_mac_count])) {
      // Check if this is our own MAC
      if (is_own_mac(s_trusted_mac_list[s_trusted_mac_count])) {
        ESP_LOGW(TAG, "Trusted MAC 4 is device's own MAC address - will be "
                      "skipped for sending");
        found_own_mac = true;
      }

      ESP_LOGI(TAG, "Trusted MAC %d: " MACSTR, s_trusted_mac_count + 1,
               MAC2STR(s_trusted_mac_list[s_trusted_mac_count]));
      s_trusted_mac_count++;
    } else {
      ESP_LOGE(TAG, "Invalid MAC address format for Trusted MAC 4");
    }
  }
#endif

// Load MAC address 5
#if CONFIG_ESPNOW_TRUSTED_MAC_COUNT >= 5
  if (s_trusted_mac_count < CONFIG_ESPNOW_TRUSTED_MAC_COUNT) {
    if (parse_mac_string(CONFIG_ESPNOW_TRUSTED_MAC_5,
                         s_trusted_mac_list[s_trusted_mac_count])) {
      // Check if this is our own MAC
      if (is_own_mac(s_trusted_mac_list[s_trusted_mac_count])) {
        ESP_LOGW(TAG, "Trusted MAC 5 is device's own MAC address - will be "
                      "skipped for sending");
        found_own_mac = true;
      }

      ESP_LOGI(TAG, "Trusted MAC %d: " MACSTR, s_trusted_mac_count + 1,
               MAC2STR(s_trusted_mac_list[s_trusted_mac_count]));
      s_trusted_mac_count++;
    } else {
      ESP_LOGE(TAG, "Invalid MAC address format for Trusted MAC 5");
    }
  }
#endif
  if (found_own_mac) {
    ESP_LOGW(TAG,
             "Own MAC found in trusted list. This is allowed but sending to "
             "self will be skipped");
  }

  ESP_LOGI(TAG, "Loaded %d trusted MAC addresses", s_trusted_mac_count);
#else
  ESP_LOGI(TAG, "Trusted MAC addresses feature disabled");
#endif
}

// Function to check if a MAC address is in our trusted list
static bool is_trusted_mac(const uint8_t *mac_addr) {
#if CONFIG_ESPNOW_USE_TRUSTED_MACS
  if (s_trusted_mac_count == 0) {
    return false;
  }

  for (int i = 0; i < s_trusted_mac_count; i++) {
    if (memcmp(mac_addr, s_trusted_mac_list[i], ESP_NOW_ETH_ALEN) == 0) {
      return true;
    }
  }

  return false;
#else
  // If trusted MACs feature is disabled, accept all
  return true;
#endif
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                                   const uint8_t *data, int len) {

  // Update the last receive time whenever we get a message
  s_last_recv_time =
      esp_timer_get_time() / 1000; // Convert microseconds to milliseconds

  example_espnow_event_t evt;
  example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
  uint8_t *mac_addr = recv_info->src_addr;
  uint8_t *des_addr = recv_info->des_addr;

  // Get RSSI if available via the rx_ctrl field
  int rssi = -120; // Default value if RSSI not available
  if (recv_info->rx_ctrl != NULL) {
    rssi = recv_info->rx_ctrl->rssi;
  }

  if (mac_addr == NULL || data == NULL || len <= 0) {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }
  // Extract PCB name from the message if it's long enough
  if (len >= sizeof(example_espnow_data_t)) {
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    // Store the PCB name from the received message
    store_peer_pcb_name(mac_addr, buf->pcb_name);
  }

  // Get PCB name for the sender
  const char *sender_pcb_name = get_peer_pcb_name(mac_addr);

  // Print custom message acknowledging receipt
  ESP_LOGI(TAG, "==================================================");
  ESP_LOGI(TAG, "Message received from: %s", sender_pcb_name);
  ESP_LOGI(TAG, "Message length: %d bytes", len);
  ESP_LOGI(TAG, "Signal strength (RSSI): %d dBm", rssi);

  // If you want to print the actual data content (if it's text)
  // Note: Only do this if your messages contain readable text
  // For binary data, you might want to print a hex dump instead
  if (len > sizeof(example_espnow_data_t)) {
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    // Just print the first 16 bytes of payload or less if shorter
    int payload_len = len - sizeof(example_espnow_data_t);
    int print_len = payload_len > 16 ? 16 : payload_len;

    ESP_LOGI(TAG, "Message type: %s",
             buf->type == EXAMPLE_ESPNOW_DATA_BROADCAST ? "Broadcast"
                                                        : "Unicast");
    ESP_LOGI(TAG, "Sequence number: %d", buf->seq_num);
    ESP_LOGI(TAG, "Sender PCB: %s", buf->pcb_name);

    if (print_len > 0) {
      char preview[17] = {0}; // +1 for null terminator
      memcpy(preview, buf->payload, print_len);

      // If you want to print hex values instead (good for binary data)
      ESP_LOGI(TAG, "Payload hex: ");
      for (int i = 0; i < print_len; i++) {
        printf("%02x ", buf->payload[i]);
        if ((i + 1) % 8 == 0)
          printf("\n");
      }
      printf("\n");
    }
  }
  ESP_LOGI(TAG, "==================================================");

  // Continue with original functionality
  if (IS_BROADCAST_ADDR(des_addr)) {
    ESP_LOGD(TAG, "Receive broadcast ESPNOW data");
  } else {
    ESP_LOGD(TAG, "Receive unicast ESPNOW data");
  }

  evt.id = EXAMPLE_ESPNOW_RECV_CB;
  memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  recv_cb->data = malloc(len);
  if (recv_cb->data == NULL) {
    ESP_LOGE(TAG, "Malloc receive data fail");
    return;
  }
  memcpy(recv_cb->data, data, len);
  recv_cb->data_len = len;
  if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
    ESP_LOGW(TAG, "Send receive queue fail");
    free(recv_cb->data);
  }
  // Forward this data to the web server for RSSI tracking
  rssi_web_espnow_recv_cb(recv_info, data, len);
}

// Modified version of the example_wifi_init function to support APSTA mode
// correctly
static void example_wifi_init(void) {
  // Initialize networking stack
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // Create default AP interface
  esp_netif_create_default_wifi_ap();

  // Get MAC address for unique SSID
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "ESP-NOW-RSSI-%02X%02X", mac[0], mac[1]);

  // Store own MAC address for later use
  init_own_mac();

  // Initialize and configure WiFi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(
      esp_wifi_set_mode(WIFI_MODE_APSTA)); // APSTA mode for both AP and ESP-NOW

  // Set up AP configuration
  wifi_config_t ap_config = {0};
  strcpy((char *)ap_config.ap.ssid, ssid);
  ap_config.ap.ssid_len = strlen(ssid);
  strcpy((char *)ap_config.ap.password, "12345678");
  ap_config.ap.channel = CONFIG_ESPNOW_CHANNEL;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.max_connection = 4;

  // Apply config and start WiFi
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_start());

  // Power saving settings
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
  ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84)); // Maximum transmission power
  ESP_ERROR_CHECK(
      esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

  ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Password=12345678, IP=192.168.4.1",
           ssid);
}

/* Parse received ESPNOW data. */
int example_espnow_data_parse(uint8_t *data, uint16_t data_len, uint8_t *state,
                              uint16_t *seq, uint32_t *magic) {
  example_espnow_data_t *buf = (example_espnow_data_t *)data;
  uint16_t crc, crc_cal = 0;

  if (data_len < sizeof(example_espnow_data_t)) {
    ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
    return -1;
  }

  *state = buf->state;
  *seq = buf->seq_num;
  *magic = buf->magic;
  crc = buf->crc;
  buf->crc = 0;
  crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

  if (crc_cal == crc) {
    return buf->type;
  }

  return -1;
}

static void example_espnow_task(void *pvParameter) {
  example_espnow_event_t evt;
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  bool is_broadcast = false;
  int ret;

  // Initialize the last receive time
  s_last_recv_time = esp_timer_get_time() / 1000; // Convert to milliseconds
  int64_t discovery_start_time = s_last_recv_time;

  // Make sure to properly cast the parameter at the beginning
  example_espnow_send_param_t *send_param =
      (example_espnow_send_param_t *)pvParameter;

  // Wait a moment before starting
  vTaskDelay(3000 / portTICK_PERIOD_MS);

  ESP_LOGI(TAG, "Starting peer discovery with broadcast...");

  // Initialize in broadcast mode for discovery
  send_param->unicast = false;
  send_param->broadcast = true;
  send_param->state = 0;
  memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
  example_espnow_data_prepare(send_param);

  // Send initial broadcast to discover peers
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Initial broadcast send error");
    example_espnow_deinit(send_param);
    vTaskDelete(NULL);
  }

  // Main task loop - handles both discovery and regular communication
  while (s_communication_active) {
    // Get current time and check timeouts
    int64_t current_time = esp_timer_get_time() / 1000;
    int64_t time_since_last_recv = current_time - s_last_recv_time;

    // Check if we're in discovery mode and should timeout
    if (!s_peer_discovery_complete &&
        (current_time - discovery_start_time > ESPNOW_DISCOVERY_TIMEOUT_MS)) {

      ESP_LOGI(TAG, "[%s] Peer discovery completed with %d peers found",
               get_pcb_name(), s_discovered_peer_count);
      s_peer_discovery_complete = true;

      // If we found any peers, switch to unicast with the first one
      if (s_discovered_peer_count > 0) {
        ESP_LOGI(TAG, "[%s] Switching to unicast mode with peer: %s",
                 get_pcb_name(), get_peer_pcb_name(s_discovered_peers[0]));

        send_param->broadcast = false;
        send_param->unicast = true;
        memcpy(send_param->dest_mac, s_discovered_peers[0], ESP_NOW_ETH_ALEN);
        example_espnow_data_prepare(send_param);

        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Failed to send first unicast");
        }
      } else {
        ESP_LOGW(TAG, "No peers discovered during broadcast phase");
      }
    }

    // Check for communication timeout (applies to both discovery and unicast
    // phases)
    if (time_since_last_recv > RESPONSE_TIMEOUT_MS) {
      ESP_LOGW(TAG, "No response received for %lld ms, stopping communication",
               time_since_last_recv);
      s_communication_active = false;
      break;
    }

    // Process incoming queue messages
    if (xQueueReceive(s_example_espnow_queue, &evt, pdMS_TO_TICKS(1000)) ==
        pdTRUE) {
      switch (evt.id) {
      case EXAMPLE_ESPNOW_SEND_CB: {
        example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
        is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

        ESP_LOGD(TAG, "Send data to " MACSTR ", status: %d",
                 MAC2STR(send_cb->mac_addr), send_cb->status);

        if (is_broadcast && (send_param->broadcast == false)) {
          break;
        }

        if (!is_broadcast) {
          send_param->count--;
          if (send_param->count == 0) {
            ESP_LOGI(TAG, "Send count reached, done");
            example_espnow_deinit(send_param);
            vTaskDelete(NULL);
          }
        }

        // Delay before sending next data
        if (send_param->delay > 0) {
          vTaskDelay(send_param->delay / portTICK_PERIOD_MS);
        }

        // For discovery mode, keep broadcasting
        // For unicast mode, keep sending to the same peer
        if (!s_peer_discovery_complete) {
          // In discovery mode - keep broadcasting
          memcpy(send_param->dest_mac, s_example_broadcast_mac,
                 ESP_NOW_ETH_ALEN);
        } else {
          // In unicast mode - reuse existing destination
          // dest_mac is already set correctly
        }

        example_espnow_data_prepare(send_param);

        // Send the next packet
        if (esp_now_send(send_param->dest_mac, send_param->buffer,
                         send_param->len) != ESP_OK) {
          ESP_LOGE(TAG, "Send error");
          example_espnow_deinit(send_param);
          vTaskDelete(NULL);
        }
        break;
      }
      case EXAMPLE_ESPNOW_RECV_CB: {
        example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

        ret = example_espnow_data_parse(recv_cb->data, recv_cb->data_len,
                                        &recv_state, &recv_seq, &recv_magic);

        // Process valid data
        if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST ||
            ret == EXAMPLE_ESPNOW_DATA_UNICAST) {

          // For discovery mode - track unique peers
          if (!s_peer_discovery_complete) {
            bool peer_exists = false;

            // Check if we already know this peer
            for (int i = 0; i < s_discovered_peer_count; i++) {
              if (memcmp(s_discovered_peers[i], recv_cb->mac_addr,
                         ESP_NOW_ETH_ALEN) == 0) {
                peer_exists = true;
                break;
              }
            }

            // If this is a new peer, add it to our list
            if (!peer_exists &&
                s_discovered_peer_count < CONFIG_ESPNOW_MAX_PEERS) {
              // Don't add our own MAC address as a peer
              if (!is_own_mac(recv_cb->mac_addr)) {
                ESP_LOGI(TAG, "New peer discovered: " MACSTR,
                         MAC2STR(recv_cb->mac_addr));

                memcpy(s_discovered_peers[s_discovered_peer_count],
                       recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                s_discovered_peer_count++;

                // Add this peer to ESP-NOW peer list
                if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                  esp_now_peer_info_t *peer =
                      malloc(sizeof(esp_now_peer_info_t));
                  if (peer == NULL) {
                    ESP_LOGE(TAG, "Malloc peer information fail");
                    free(recv_cb->data);
                    break;
                  }

                  memset(peer, 0, sizeof(esp_now_peer_info_t));
                  peer->channel = CONFIG_ESPNOW_CHANNEL;
                  peer->ifidx = ESPNOW_WIFI_IF;
                  peer->encrypt = false;
                  memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                  ESP_ERROR_CHECK(esp_now_add_peer(peer));
                  free(peer);
                }
              }
            }
          }

          // Handle transition from broadcast to unicast
          if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST && recv_state == 1) {
            // If we receive a broadcast with state=1, it means the other device
            // has received our broadcast. Choose who continues sending based on
            // magic.
            if (send_param->unicast == false &&
                send_param->magic >= recv_magic) {
              ESP_LOGI(TAG, "Starting unicast communication with %s",
                       get_peer_pcb_name(recv_cb->mac_addr));

              send_param->broadcast = false;
              send_param->unicast = true;
              memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
              example_espnow_data_prepare(send_param);

              if (esp_now_send(send_param->dest_mac, send_param->buffer,
                               send_param->len) != ESP_OK) {
                ESP_LOGE(TAG, "Send error");
              }
            }
          }
        }

        free(recv_cb->data);
        break;
      }
      default:
        ESP_LOGE(TAG, "Callback type error: %d", evt.id);
        break;
      }
    } else {
      // No queue message received, yield to other tasks
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  // Cleanup when communication is stopped
  ESP_LOGI(TAG, "Communication stopped, cleaning up");
  example_espnow_deinit(send_param);
  vTaskDelete(NULL);
}

static esp_err_t example_espnow_init(void) {
  example_espnow_send_param_t *send_param;

  s_example_espnow_queue =
      xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
  if (s_example_espnow_queue == NULL) {
    ESP_LOGE(TAG, "Create queue fail");
    return ESP_FAIL;
  }

  // Initialize ESPNOW and register callbacks
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(example_espnow_send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(example_espnow_recv_cb));

  // Power saving settings
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
  ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
  ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(
      CONFIG_ESPNOW_WAKE_INTERVAL));
#endif

  // Add broadcast peer
  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL) {
    ESP_LOGE(TAG, "Malloc peer information fail");
    vQueueDelete(s_example_espnow_queue);
    esp_now_deinit();
    return ESP_FAIL;
  }

  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = CONFIG_ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;
  memcpy(peer->peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);

  // Initialize sending parameters for discovery
  send_param = malloc(sizeof(example_espnow_send_param_t));
  if (send_param == NULL) {
    ESP_LOGE(TAG, "Malloc send parameter fail");
    vQueueDelete(s_example_espnow_queue);
    esp_now_deinit();
    return ESP_FAIL;
  }

  memset(send_param, 0, sizeof(example_espnow_send_param_t));
  send_param->unicast = false;
  send_param->broadcast = true;
  send_param->state = 0;
  send_param->magic = esp_random();
  send_param->count = UINT32_MAX; // Run indefinitely
  send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
  send_param->len = CONFIG_ESPNOW_SEND_LEN;
  send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);

  if (send_param->buffer == NULL) {
    ESP_LOGE(TAG, "Malloc send buffer fail");
    free(send_param);
    vQueueDelete(s_example_espnow_queue);
    esp_now_deinit();
    return ESP_FAIL;
  }

  memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
  example_espnow_data_prepare(send_param);

  // Create task for ESP-NOW communication
  xTaskCreate(example_espnow_task, "example_espnow_task", 2048, send_param, 4,
              NULL);

  return ESP_OK;
}

static void example_espnow_deinit(example_espnow_send_param_t *send_param) {
  free(send_param->buffer);
  free(send_param);
  vSemaphoreDelete(s_example_espnow_queue);

  // Instead of esp_now_deinit(), just unregister callbacks to keep WiFi
  // active
  esp_now_unregister_send_cb();
  esp_now_unregister_recv_cb();

  // Put WiFi in power-save mode when not actively communicating
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

  ESP_LOGI(TAG, "ESP-NOW communication task finished, power save enabled");
}

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Get MAC address to create unique SSID
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
  // Get and store own MAC address (if not already done in example_wifi_init)
  init_own_mac();

  // Create unique SSID with MAC address
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "ESP-NOW-RSSI-%02X", mac[0]);
  // Log startup with PCB name
  ESP_LOGI(TAG, "Starting ESP-NOW device [%s] with MAC: " MACSTR,
           get_pcb_name(), MAC2STR(s_own_mac));

  // Initialize WiFi in APSTA mode
  example_wifi_init();

  // Initialize ESP-NOW
  example_espnow_init();

  // Tell the web server to use existing WiFi (true)
  // Pass the same unique SSID we created
  ESP_ERROR_CHECK(rssi_web_init(ssid, "12345678", CONFIG_ESPNOW_CHANNEL, true));

  // Start the web server
  server = rssi_web_start();
  if (server == NULL) {
    ESP_LOGE(TAG, "Failed to start web server!");
  } else {
    ESP_LOGI(TAG, "Web server started successfully!");
    ESP_LOGI(TAG, "Connect to WiFi SSID: %s, Password: 12345678", ssid);
    ESP_LOGI(TAG, "Then navigate to http://192.168.4.1 in your browser");
  }
}

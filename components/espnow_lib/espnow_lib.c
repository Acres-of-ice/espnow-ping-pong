/**
 * @file espnow_lib.c
 * @brief ESP-NOW communication library implementation
 */

#include "espnow_lib.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_mac.h" // Add this include for MACSTR and MAC2STR macros
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <inttypes.h> // Add this at the top of your file
#include <string.h>

static const char *TAG = "espnow_lib";

/* Define broadcast MAC address */
const uint8_t ESPNOW_BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF,
                                                        0xFF, 0xFF, 0xFF};

/* Global variables */
static QueueHandle_t s_espnow_queue = NULL;
static uint8_t s_own_mac[ESP_NOW_ETH_ALEN] = {0};
static char s_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH] = {0};
static uint16_t s_espnow_seq[ESPNOW_DATA_MAX] = {0, 0};
static TaskHandle_t s_espnow_task_handle = NULL;
static int64_t s_last_recv_time = 0;
static bool s_communication_active = true;
static bool s_discovery_complete = false;
static uint32_t s_discovery_timeout_ms = 30000; // Default 30s timeout
static uint8_t s_trusted_peers[ESPNOW_MAX_PEERS][ESP_NOW_ETH_ALEN] = {0};
static int s_trusted_peer_count = 0;

/* Peer information structure */
typedef struct {
  uint8_t mac[ESP_NOW_ETH_ALEN];
  char pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH];
  bool has_pcb_name;
  int64_t last_seen; // Timestamp when last seen
  int8_t rssi;       // Last signal strength
} peer_info_t;

/* For peer discovery and management */
static uint8_t s_discovered_peers[ESPNOW_MAX_PEERS][ESP_NOW_ETH_ALEN] = {0};
static int s_discovered_peer_count = 0;
static peer_info_t s_peer_info[ESPNOW_MAX_PEERS] = {0};

/* User callbacks */
static espnow_recv_cb_t s_user_recv_cb = NULL;
static espnow_send_cb_t s_user_send_cb = NULL;

static void espnow_send_cb(const uint8_t *mac_addr,
                           esp_now_send_status_t status);
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len);
static void init_own_mac(void);
static bool is_own_mac(const uint8_t *mac_addr);
static void store_peer_pcb_name(const uint8_t *mac_addr, const char *pcb_name);
static esp_err_t espnow_send_internal(const uint8_t *mac_addr, const void *data,
                                      size_t len, bool include_pcb_name);
static void prepare_espnow_data(espnow_send_param_t *send_param);
static int parse_espnow_data(uint8_t *data, uint16_t data_len, uint8_t *state,
                             uint16_t *seq, uint32_t *magic, char *pcb_name);

/* ESP-NOW event structure */
typedef struct {
  espnow_event_id_t id;
  union {
    espnow_event_send_cb_t send_cb;
    espnow_event_recv_cb_t recv_cb;
  } info;
} espnow_event_t;

/* Forward declarations of static functions */
static void espnow_task(void *pvParameter) {
  int64_t discovery_start_time = esp_timer_get_time() / 1000;
  uint8_t recv_state = 0;
  uint16_t recv_seq = 0;
  uint32_t recv_magic = 0;
  char peer_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH] = {0};

  ESP_LOGI(TAG, "ESP-NOW task started");

  // Send initial broadcast if in discovery mode
  if (!s_discovery_complete) {
    uint8_t dummy_data = 0;
    espnow_send(ESPNOW_BROADCAST_MAC, &dummy_data, 1);
  }

  // Main task loop
  while (s_communication_active) {
    // Get current time
    int64_t current_time = esp_timer_get_time() / 1000;

    // Check discovery timeout
    if (!s_discovery_complete &&
        (current_time - discovery_start_time > s_discovery_timeout_ms)) {

      ESP_LOGI(TAG, "Peer discovery completed with %d peers found",
               s_discovered_peer_count);
      s_discovery_complete = true;

      // Notify any listeners (could add a callback here)
    }

    // Process queue events
    espnow_event_t evt;
    if (xQueueReceive(s_espnow_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
      switch (evt.id) {
      case ESPNOW_SEND_CB: {
        // Process send callback
        ESP_LOGD(TAG, "Send data to " MACSTR ", status: %d",
                 MAC2STR(evt.info.send_cb.mac_addr), evt.info.send_cb.status);
        break;
      }

      case ESPNOW_RECV_CB: {
        // Process receive callback
        int ret = parse_espnow_data(evt.info.recv_cb.data,
                                    evt.info.recv_cb.data_len, &recv_state,
                                    &recv_seq, &recv_magic, peer_pcb_name);

        if (ret == ESPNOW_DATA_BROADCAST || ret == ESPNOW_DATA_UNICAST) {
          ESP_LOGD(TAG, "Received %s data from %s, seq=%u, magic=%" PRIu32,
                   ret == ESPNOW_DATA_BROADCAST ? "broadcast" : "unicast",
                   peer_pcb_name, recv_seq, recv_magic);
        }

        // Free data when done
        free(evt.info.recv_cb.data);
        break;
      }

      default:
        ESP_LOGE(TAG, "Unknown event type: %d", evt.id);
        break;
      }
    }
  }

  ESP_LOGI(TAG, "ESP-NOW task ended");
  vTaskDelete(NULL);
};

esp_err_t espnow_init(const espnow_config_t *config) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // Copy PCB name
  if (config->pcb_name != NULL) {
    strncpy(s_pcb_name, config->pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH - 1);
    s_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';
  } else {
    // Default PCB name based on MAC
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH, "ESP32-%02X%02X", mac[4],
             mac[5]);
  }

  // Save user callbacks
  s_user_recv_cb = config->recv_cb;
  s_user_send_cb = config->send_cb;

  // Create queue for ESP-NOW events
  s_espnow_queue = xQueueCreate(10, sizeof(espnow_event_t));
  if (s_espnow_queue == NULL) {
    ESP_LOGE(TAG, "Create queue failed");
    return ESP_FAIL;
  }

  // Get own MAC address
  init_own_mac();

  // Initialize ESP-NOW
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

  // Add broadcast peer
  esp_now_peer_info_t peer = {0};
  peer.channel = config->wifi_channel;
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = config->enable_encryption;

  if (config->enable_encryption) {
    if (config->pmk == NULL || config->lmk == NULL) {
      ESP_LOGE(TAG, "PMK and LMK must be provided when encryption is enabled");
      esp_now_deinit();
      vQueueDelete(s_espnow_queue);
      return ESP_ERR_INVALID_ARG;
    }

    memcpy(peer.lmk, config->lmk, 16);
    ESP_ERROR_CHECK(esp_now_set_pmk((const uint8_t *)config->pmk));
  }

  memcpy(peer.peer_addr, ESPNOW_BROADCAST_MAC, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(&peer));

  // Initialize variables
  s_last_recv_time = esp_timer_get_time() / 1000; // Convert to milliseconds
  s_communication_active = true;
  s_discovery_complete = false;

  // Create ESP-NOW task
  xTaskCreate(espnow_task, "espnow_task", 2048, NULL, 4, &s_espnow_task_handle);

  ESP_LOGI(TAG, "ESP-NOW initialized with PCB name: %s", s_pcb_name);

  return ESP_OK;
}

esp_err_t espnow_deinit(void) {
  // Stop the task
  s_communication_active = false;

  // Wait for task to finish
  if (s_espnow_task_handle != NULL) {
    vTaskDelay(100 /
               portTICK_PERIOD_MS); // Give some time for the task to clean up
    vTaskDelete(s_espnow_task_handle);
    s_espnow_task_handle = NULL;
  }

  // Unregister callbacks
  esp_now_unregister_send_cb();
  esp_now_unregister_recv_cb();

  // Deinitialize ESP-NOW
  esp_err_t ret = esp_now_deinit();

  // Delete queue
  if (s_espnow_queue != NULL) {
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
  }

  ESP_LOGI(TAG, "ESP-NOW deinitialized");

  return ret;
}

esp_err_t espnow_send(const uint8_t *mac_addr, const void *data, size_t len) {
  return espnow_send_internal(mac_addr, data, len, true);
}

esp_err_t espnow_get_own_mac(uint8_t *mac_addr) {
  if (mac_addr == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(mac_addr, s_own_mac, ESP_NOW_ETH_ALEN);
  return ESP_OK;
}

const char *espnow_get_peer_name(const uint8_t *mac_addr) {
  static char unknown_peer[32];

  if (mac_addr == NULL) {
    return "Unknown";
  }

  if (is_own_mac(mac_addr)) {
    return s_pcb_name;
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

esp_err_t espnow_set_pcb_name(const char *pcb_name) {
  if (pcb_name == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  strncpy(s_pcb_name, pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH - 1);
  s_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';

  return ESP_OK;
}

int espnow_get_peer_count(void) { return s_discovered_peer_count; }

esp_err_t espnow_get_peer_mac(int index, uint8_t *mac_addr) {
  if (mac_addr == NULL || index < 0 || index >= s_discovered_peer_count) {
    return ESP_ERR_INVALID_ARG;
  }

  memcpy(mac_addr, s_discovered_peers[index], ESP_NOW_ETH_ALEN);
  return ESP_OK;
}

esp_err_t espnow_start_discovery(uint32_t timeout_ms) {
  s_discovery_complete = false;
  s_discovered_peer_count = 0;
  memset(s_discovered_peers, 0, sizeof(s_discovered_peers));
  memset(s_peer_info, 0, sizeof(s_peer_info));

  s_discovery_timeout_ms = timeout_ms;

  // Send broadcast to start discovery
  uint8_t dummy_data = 0;
  return espnow_send(ESPNOW_BROADCAST_MAC, &dummy_data, 1);
}

esp_err_t espnow_add_trusted_peer(const uint8_t *mac_addr) {
  if (mac_addr == NULL || s_trusted_peer_count >= ESPNOW_MAX_PEERS) {
    return ESP_ERR_INVALID_ARG;
  }

  // Check if already in list
  for (int i = 0; i < s_trusted_peer_count; i++) {
    if (memcmp(s_trusted_peers[i], mac_addr, ESP_NOW_ETH_ALEN) == 0) {
      return ESP_OK; // Already in list
    }
  }

  // Add to list
  memcpy(s_trusted_peers[s_trusted_peer_count], mac_addr, ESP_NOW_ETH_ALEN);
  s_trusted_peer_count++;

  // Also add as a peer to ESP-NOW
  if (esp_now_is_peer_exist(mac_addr) == false) {
    esp_now_peer_info_t peer = {
        .channel = 0, // Use current channel
        .ifidx = WIFI_IF_AP,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&peer);
  }

  return ESP_OK;
}

bool espnow_is_trusted_peer(const uint8_t *mac_addr) {
  if (mac_addr == NULL) {
    return false;
  }

  // If no trusted peers configured, trust everyone
  if (s_trusted_peer_count == 0) {
    return true;
  }

  // Check if in trusted list
  for (int i = 0; i < s_trusted_peer_count; i++) {
    if (memcmp(s_trusted_peers[i], mac_addr, ESP_NOW_ETH_ALEN) == 0) {
      return true;
    }
  }

  return false;
}

/* Static function implementations */

static void init_own_mac(void) {
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_AP, s_own_mac);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get own MAC address, err: %d", ret);
    // Fall back to efuse MAC if AP MAC is not available
    esp_efuse_mac_get_default(s_own_mac);
  }
  ESP_LOGI(TAG, "Own MAC address: " MACSTR, MAC2STR(s_own_mac));
}

static bool is_own_mac(const uint8_t *mac_addr) {
  return (memcmp(mac_addr, s_own_mac, ESP_NOW_ETH_ALEN) == 0);
}

static void store_peer_pcb_name(const uint8_t *mac_addr, const char *pcb_name) {
  for (int i = 0; i < s_discovered_peer_count; i++) {
    if (memcmp(s_peer_info[i].mac, mac_addr, ESP_NOW_ETH_ALEN) == 0) {
      // Update existing entry
      strncpy(s_peer_info[i].pcb_name, pcb_name,
              ESPNOW_MAX_PCB_NAME_LENGTH - 1);
      s_peer_info[i].pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';
      s_peer_info[i].has_pcb_name = true;
      return;
    }
  }

  // Add new entry if we have space
  if (s_discovered_peer_count < ESPNOW_MAX_PEERS) {
    memcpy(s_peer_info[s_discovered_peer_count].mac, mac_addr,
           ESP_NOW_ETH_ALEN);
    strncpy(s_peer_info[s_discovered_peer_count].pcb_name, pcb_name,
            ESPNOW_MAX_PCB_NAME_LENGTH - 1);
    s_peer_info[s_discovered_peer_count]
        .pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';
    s_peer_info[s_discovered_peer_count].has_pcb_name = true;
    // Do not increment s_discovered_peer_count here, as that's managed
    // elsewhere
  }
}

static esp_err_t espnow_send_internal(const uint8_t *mac_addr, const void *data,
                                      size_t len, bool include_pcb_name) {
  if (mac_addr == NULL || data == NULL || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  // Skip sending to self
  if (is_own_mac(mac_addr)) {
    return ESP_OK;
  }

  // Check if peer exists and add if not
  if (esp_now_is_peer_exist(mac_addr) == false &&
      memcmp(mac_addr, ESPNOW_BROADCAST_MAC, ESP_NOW_ETH_ALEN) != 0) {

    esp_now_peer_info_t peer = {
        .channel = 0, // Use current channel
        .ifidx = WIFI_IF_AP,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    esp_now_add_peer(&peer);
  }

  if (include_pcb_name) {
    // Create a new buffer with header and data
    size_t total_len = sizeof(espnow_data_t) + len;
    uint8_t *buffer = malloc(total_len);
    if (buffer == NULL) {
      ESP_LOGE(TAG, "Malloc buffer failed");
      return ESP_ERR_NO_MEM;
    }

    // Prepare header
    espnow_data_t *header = (espnow_data_t *)buffer;
    header->type =
        (memcmp(mac_addr, ESPNOW_BROADCAST_MAC, ESP_NOW_ETH_ALEN) == 0)
            ? ESPNOW_DATA_BROADCAST
            : ESPNOW_DATA_UNICAST;
    header->state = 0;
    header->seq_num = s_espnow_seq[header->type]++;
    header->magic = esp_random();

    // Copy PCB name
    strncpy(header->pcb_name, s_pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH - 1);
    header->pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';

    // Copy data to payload
    memcpy(header->payload, data, len);

    // Calculate CRC
    header->crc = 0;
    header->crc = esp_crc16_le(UINT16_MAX, buffer, total_len);

    // Send data
    esp_err_t ret = esp_now_send(mac_addr, buffer, total_len);

    // Free buffer
    free(buffer);

    return ret;
  } else {
    // Send raw data without header
    return esp_now_send(mac_addr, data, len);
  }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int len) {
  if (recv_info == NULL || data == NULL || len <= 0) {
    ESP_LOGE(TAG, "Receive cb arg error");
    return;
  }

  const uint8_t *mac_addr = recv_info->src_addr;

  // Update the last receive time
  s_last_recv_time = esp_timer_get_time() / 1000; // Convert to milliseconds

  // Get RSSI if available via the rx_ctrl field
  int rssi = -120; // Default value if RSSI not available
  if (recv_info->rx_ctrl != NULL) {
    rssi = recv_info->rx_ctrl->rssi;
  }

  // Extract PCB name if message is long enough
  if (len >= sizeof(espnow_data_t)) {
    espnow_data_t *buf = (espnow_data_t *)data;
    // Store the PCB name from the received message
    store_peer_pcb_name(mac_addr, buf->pcb_name);
  }

  // Call user callback if registered
  if (s_user_recv_cb != NULL) {
    // For custom messages, pass the payload part to the user
    if (len > sizeof(espnow_data_t)) {
      espnow_data_t *buf = (espnow_data_t *)data;
      s_user_recv_cb(mac_addr, buf->payload, len - sizeof(espnow_data_t), rssi);
    } else {
      s_user_recv_cb(mac_addr, data, len, rssi);
    }
  }

  // Post event to queue
  espnow_event_t evt;
  evt.id = ESPNOW_RECV_CB;
  memcpy(evt.info.recv_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);

  // Make a copy of the data
  evt.info.recv_cb.data = malloc(len);
  if (evt.info.recv_cb.data == NULL) {
    ESP_LOGE(TAG, "Malloc receive data fail");
    return;
  }

  memcpy(evt.info.recv_cb.data, data, len);
  evt.info.recv_cb.data_len = len;
  evt.info.recv_cb.rssi = rssi;

  // For discovery, check if this is a new peer
  if (!s_discovery_complete) {
    bool peer_exists = false;
    bool is_self = is_own_mac(mac_addr);

    // Don't add ourselves as a peer
    if (!is_self) {
      // Check if we already know this peer
      for (int i = 0; i < s_discovered_peer_count; i++) {
        if (memcmp(s_discovered_peers[i], mac_addr, ESP_NOW_ETH_ALEN) == 0) {
          peer_exists = true;
          break;
        }
      }

      // Add new peer if not already known
      if (!peer_exists && s_discovered_peer_count < ESPNOW_MAX_PEERS) {
        ESP_LOGI(TAG, "New peer discovered: " MACSTR, MAC2STR(mac_addr));

        // Add to discovered peers list
        memcpy(s_discovered_peers[s_discovered_peer_count], mac_addr,
               ESP_NOW_ETH_ALEN);
        s_discovered_peer_count++;

        // Add as ESP-NOW peer
        if (esp_now_is_peer_exist(mac_addr) == false) {
          esp_now_peer_info_t peer = {
              .channel = 0, // Use current channel
              .ifidx = WIFI_IF_AP,
              .encrypt = false,
          };
          memcpy(peer.peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
          esp_now_add_peer(&peer);
        }
      }
    }
  }

  if (xQueueSend(s_espnow_queue, &evt, 10 / portTICK_PERIOD_MS) != pdTRUE) {
    ESP_LOGW(TAG, "Receive queue full");
    free(evt.info.recv_cb.data);
  }
}

static void prepare_espnow_data(espnow_send_param_t *send_param) {
  espnow_data_t *buf = (espnow_data_t *)send_param->buffer;

  buf->type = (memcmp(send_param->dest_mac, ESPNOW_BROADCAST_MAC,
                      ESP_NOW_ETH_ALEN) == 0)
                  ? ESPNOW_DATA_BROADCAST
                  : ESPNOW_DATA_UNICAST;
  buf->state = send_param->state;
  buf->seq_num = s_espnow_seq[buf->type]++;
  buf->crc = 0;
  buf->magic = send_param->magic;

  // Add PCB name to the message
  strncpy(buf->pcb_name, s_pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH - 1);
  buf->pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';

  // Fill remaining bytes with random values
  esp_fill_random(buf->payload, send_param->len - sizeof(espnow_data_t));

  // Calculate CRC
  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static int parse_espnow_data(uint8_t *data, uint16_t data_len, uint8_t *state,
                             uint16_t *seq, uint32_t *magic, char *pcb_name) {
  espnow_data_t *buf = (espnow_data_t *)data;
  uint16_t crc, crc_cal = 0;

  if (data_len < sizeof(espnow_data_t)) {
    ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
    return -1;
  }

  *state = buf->state;
  *seq = buf->seq_num;
  *magic = buf->magic;

  if (pcb_name != NULL) {
    strncpy(pcb_name, buf->pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH - 1);
    pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';
  }

  crc = buf->crc;
  buf->crc = 0;
  crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

  if (crc_cal == crc) {
    return buf->type;
  }

  return -1;
}

static void espnow_send_cb(const uint8_t *mac_addr,
                           esp_now_send_status_t status) {
  if (mac_addr == NULL) {
    ESP_LOGE(TAG, "Send cb arg error");
    return;
  }

  // Call user callback if registered
  if (s_user_send_cb != NULL) {
    s_user_send_cb(mac_addr, status);
  }

  // Post event to queue
  espnow_event_t evt;
  evt.id = ESPNOW_SEND_CB;
  memcpy(evt.info.send_cb.mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
  evt.info.send_cb.status = status;

  if (xQueueSend(s_espnow_queue, &evt, 10 / portTICK_PERIOD_MS) != pdTRUE) {
    ESP_LOGW(TAG, "Send queue full");
  }
}

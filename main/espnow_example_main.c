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

  // Print custom message acknowledging receipt
  ESP_LOGI(TAG, "==================================================");
  ESP_LOGI(TAG, "Message received from: " MACSTR, MAC2STR(mac_addr));
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

    if (print_len > 0) {
      char preview[17] = {0}; // +1 for null terminator
      memcpy(preview, buf->payload, print_len);
      ESP_LOGI(TAG, "Payload preview: %s", preview);

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

/* WiFi should start before using ESPNOW */
static void example_wifi_init(void) {
  // Initialize networking stack
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_ap();

  // Get MAC address for unique SSID
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
  char ssid[32];
  snprintf(ssid, sizeof(ssid), "ESP-NOW-RSSI-%02X", mac[0]);

  // Initialize and configure WiFi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

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
  // Add these lines for power saving:
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
  // Set maximum transmission power
  ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));
  ESP_ERROR_CHECK(
      esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

  // Log connection info
  ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Password=12345678, IP=192.168.4.1",
           ssid);
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

/* Prepare ESPNOW data to be sent. */
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
  /* Fill all remaining bytes after the data with random values */
  esp_fill_random(buf->payload,
                  send_param->len - sizeof(example_espnow_data_t));
  buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
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

  // Make sure to properly cast the parameter at the beginning
  example_espnow_send_param_t *send_param =
      (example_espnow_send_param_t *)pvParameter;

  vTaskDelay(5000 / portTICK_PERIOD_MS);

#if CONFIG_ESPNOW_USE_TRUSTED_MACS
  if (s_trusted_mac_count > 0) {
    ESP_LOGI(TAG, "Start sending unicast data to trusted peers");

    example_espnow_send_param_t *send_param =
        (example_espnow_send_param_t *)pvParameter;

    // Send to each trusted peer
    for (int i = 0; i < s_trusted_mac_count; i++) {
      // Skip sending to our own MAC address
      if (is_own_mac(s_trusted_mac_list[i])) {
        ESP_LOGI(TAG, "Skipping sending to own MAC address: " MACSTR,
                 MAC2STR(s_trusted_mac_list[i]));
        continue;
      }
      // Set destination MAC
      memcpy(send_param->dest_mac, s_trusted_mac_list[i], ESP_NOW_ETH_ALEN);

      // Prepare data
      send_param->unicast = true;
      send_param->broadcast = false;
      example_espnow_data_prepare(send_param);

      // Send data
      ESP_LOGI(TAG, "Sending to trusted peer %d: " MACSTR, i + 1,
               MAC2STR(s_trusted_mac_list[i]));

      if (esp_now_send(send_param->dest_mac, send_param->buffer,
                       send_param->len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error to peer %d", i + 1);
      }

      // Delay between sends
      vTaskDelay(500 / portTICK_PERIOD_MS);
    }
  } else {
    ESP_LOGW(TAG, "No trusted peers configured, not sending data");
    example_espnow_deinit(pvParameter);
    vTaskDelete(NULL);
    return;
  }
#else

  ESP_LOGI(TAG, "Start sending broadcast data");

  /* Start sending broadcast ESPNOW data. */
  example_espnow_send_param_t *send_param =
      (example_espnow_send_param_t *)pvParameter;
  if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) !=
      ESP_OK) {
    ESP_LOGE(TAG, "Send error");
    example_espnow_deinit(send_param);
    vTaskDelete(NULL);
  }
#endif

  // Main task loop - combined timeout checking with queue processing
  while (s_communication_active) {
    // Check if we've exceeded the timeout
    int64_t current_time =
        esp_timer_get_time() / 1000; // Convert to milliseconds
    int64_t time_since_last_recv = current_time - s_last_recv_time;

    // If no messages received for the timeout period
    if (time_since_last_recv > RESPONSE_TIMEOUT_MS) {
      ESP_LOGW(TAG, "No response received for %lld ms, stopping communication",
               time_since_last_recv);
      s_communication_active = false;
      break; // Exit the loop
    }

    // Wait for queue events with a timeout to prevent CPU hogging
    // Using a short timeout allows us to periodically check the communication
    // timeout
    if (xQueueReceive(s_example_espnow_queue, &evt, pdMS_TO_TICKS(1000)) ==
        pdTRUE) {
      switch (evt.id) {
      case EXAMPLE_ESPNOW_SEND_CB: {
        example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
#if CONFIG_ESPNOW_USE_TRUSTED_MACS
        // Only proceed if this is a trusted MAC
        if (!is_trusted_mac(send_cb->mac_addr)) {
          ESP_LOGW(TAG, "Ignoring non-trusted MAC: " MACSTR,
                   MAC2STR(send_cb->mac_addr));
          break;
        }
#else
        is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);
#endif

        ESP_LOGD(TAG, "Send data to " MACSTR ", status1: %d",
                 MAC2STR(send_cb->mac_addr), send_cb->status);

        if (is_broadcast && (send_param->broadcast == false)) {
          break;
        }

        if (!is_broadcast) {
          send_param->count--;
          if (send_param->count == 0) {
            ESP_LOGI(TAG, "Send done");
            example_espnow_deinit(send_param);
            vTaskDelete(NULL);
          }
        }

        /* Delay a while before sending the next data. */
        if (send_param->delay > 0) {
          vTaskDelay(send_param->delay / portTICK_PERIOD_MS);
        }

        ESP_LOGI(TAG, "send data to " MACSTR "", MAC2STR(send_cb->mac_addr));

        memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
        example_espnow_data_prepare(send_param);

        /* Send the next data after the previous data is sent. */
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
        free(recv_cb->data);
        if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST) {
          ESP_LOGI(TAG, "Receive %dth broadcast data from: " MACSTR ", len: %d",
                   recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);
#if CONFIG_ESPNOW_USE_TRUSTED_MACS
          // Only process packets from trusted sources
          if (!is_trusted_mac(recv_cb->mac_addr)) {
            ESP_LOGW(TAG, "Received data from untrusted source: " MACSTR,
                     MAC2STR(recv_cb->mac_addr));
            break;
          }
#endif

          /* If MAC address does not exist in peer list, add it to peer list. */
          if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
            esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
            if (peer == NULL) {
              ESP_LOGE(TAG, "Malloc peer information fail");
              example_espnow_deinit(send_param);
              vTaskDelete(NULL);
            }
            memset(peer, 0, sizeof(esp_now_peer_info_t));
            peer->channel = CONFIG_ESPNOW_CHANNEL;
            peer->ifidx = ESPNOW_WIFI_IF;
#if CONFIG_ESPNOW_ENABLE_ENCRYPTION
            peer->encrypt = true;
            memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
            ESP_LOGI(TAG, "Adding peer with encryption enabled");
#else
            peer->encrypt = false;
            ESP_LOGI(TAG, "Adding peer with encryption disabled");
#endif
            memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
            ESP_ERROR_CHECK(esp_now_add_peer(peer));
            free(peer);
          }

          /* Indicates that the device has received broadcast ESPNOW data. */
          if (send_param->state == 0) {
            send_param->state = 1;
          }

          /* If receive broadcast ESPNOW data which indicates that the other
           * device has received broadcast ESPNOW data and the local magic
           * number is bigger than that in the received broadcast ESPNOW data,
           * stop sending broadcast ESPNOW data and start sending unicast ESPNOW
           * data.
           */
          if (recv_state == 1) {
            /* The device which has the bigger magic number sends ESPNOW data,
             * the other one receives ESPNOW data.
             */
            if (send_param->unicast == false &&
                send_param->magic >= recv_magic) {
              ESP_LOGI(TAG, "Start sending unicast data");
              ESP_LOGI(TAG, "send data to " MACSTR "",
                       MAC2STR(recv_cb->mac_addr));

              /* Start sending unicast ESPNOW data. */
              memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
              example_espnow_data_prepare(send_param);
              if (esp_now_send(send_param->dest_mac, send_param->buffer,
                               send_param->len) != ESP_OK) {
                ESP_LOGE(TAG, "Send error");
                example_espnow_deinit(send_param);
                vTaskDelete(NULL);
              } else {
                send_param->broadcast = false;
                send_param->unicast = true;
              }
            }
          }
        } else if (ret == EXAMPLE_ESPNOW_DATA_UNICAST) {
          ESP_LOGI(TAG, "Receive %dth unicast data from: " MACSTR ", len: %d",
                   recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

          /* If receive unicast ESPNOW data, also stop sending broadcast ESPNOW
           * data. */
          send_param->broadcast = false;
        } else {
          ESP_LOGI(TAG, "Receive error data from: " MACSTR "",
                   MAC2STR(recv_cb->mac_addr));
        }
        break;
      }
      default:
        ESP_LOGE(TAG, "Callback type error: %d", evt.id);
        break;
      }
    }
    // If we didn't get a queue item, yield to other tasks
    else {
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
    ESP_LOGE(TAG, "Create mutex fail");
    return ESP_FAIL;
  }

  /* Initialize ESPNOW and register sending and receiving callback function.
   */
  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(example_espnow_send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(example_espnow_recv_cb));
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
  ESP_ERROR_CHECK(esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW));
  ESP_ERROR_CHECK(esp_wifi_connectionless_module_set_wake_interval(
      CONFIG_ESPNOW_WAKE_INTERVAL));
#endif

#if CONFIG_ESPNOW_ENABLE_ENCRYPTION
  /* Set primary master key only if encryption is enabled */
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK));
#endif

  // Load trusted MAC addresses from Kconfig
  load_trusted_mac_addresses();
#if CONFIG_ESPNOW_USE_TRUSTED_MACS
  // Add each trusted peer
  for (int i = 0; i < s_trusted_mac_count; i++) {
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
      ESP_LOGE(TAG, "Malloc peer information fail");
      vSemaphoreDelete(s_example_espnow_queue);
      esp_now_deinit();
      return ESP_FAIL;
    }

    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    ESP_LOGI(TAG, "Adding trusted peer %d with encryption disabled", i + 1);
    memcpy(peer->peer_addr, s_trusted_mac_list[i], ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(peer));
    free(peer);
  }
#else

  /* Add broadcast peer information to peer list. */
  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL) {
    ESP_LOGE(TAG, "Malloc peer information fail");
    vSemaphoreDelete(s_example_espnow_queue);
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
#endif

  /* Initialize sending parameters. */
  send_param = malloc(sizeof(example_espnow_send_param_t));
  if (send_param == NULL) {
    ESP_LOGE(TAG, "Malloc send parameter fail");
    vSemaphoreDelete(s_example_espnow_queue);
    esp_now_deinit();
    return ESP_FAIL;
  }
  memset(send_param, 0, sizeof(example_espnow_send_param_t));
  send_param->unicast = false;
  send_param->broadcast = true;
  send_param->state = 0;
  send_param->magic = esp_random();
  // send_param->count = CONFIG_ESPNOW_SEND_COUNT;
  send_param->count = UINT32_MAX;
  send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
  send_param->len = CONFIG_ESPNOW_SEND_LEN;
  send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
  if (send_param->buffer == NULL) {
    ESP_LOGE(TAG, "Malloc send buffer fail");
    free(send_param);
    vSemaphoreDelete(s_example_espnow_queue);
    esp_now_deinit();
    return ESP_FAIL;
  }
  memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
  example_espnow_data_prepare(send_param);

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

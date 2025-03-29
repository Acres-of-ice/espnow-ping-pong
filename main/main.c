
/**
 * @file main.c
 * @brief Simple ESP-NOW communication example with PCB name identification
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

// Include the ESP-NOW library
#include "espnow_lib.h"

static const char *TAG = "esp-now-example";

// Flag to indicate when a message is received
static volatile bool message_received = false;
static char last_message[128];
static uint8_t last_sender_mac[ESP_NOW_ETH_ALEN];
static int last_rssi = 0;
static char last_sender_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH];

// Task to send periodic messages
void send_task(void *pvParameter);

// Callback for received ESP-NOW data
static void on_data_received(const uint8_t *mac_addr, const uint8_t *data,
                             int data_len, int rssi) {
  // Copy data to process it safely
  if (data_len > 0 && data_len < sizeof(last_message)) {
    memcpy(last_message, data, data_len);
    last_message[data_len] = '\0'; // Ensure null termination
    memcpy(last_sender_mac, mac_addr, ESP_NOW_ETH_ALEN);
    last_rssi = rssi;
    message_received = true;

    // Get the PCB name of the sender
    const char *pcb_name = espnow_get_peer_name(mac_addr);
    strncpy(last_sender_pcb_name, pcb_name, ESPNOW_MAX_PCB_NAME_LENGTH - 1);
    last_sender_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH - 1] = '\0';

    // Log the received message with PCB name
    ESP_LOGI(TAG, "Message from %s (" MACSTR ", RSSI: %d): %s", pcb_name,
             MAC2STR(mac_addr), rssi, last_message);
  }
}

// Callback for sent ESP-NOW data
static void on_data_sent(const uint8_t *mac_addr,
                         esp_now_send_status_t status) {
  // Get PCB name for logging
  const char *pcb_name = espnow_get_peer_name(mac_addr);

  ESP_LOGI(TAG, "Message to %s (" MACSTR ") sent: %s", pcb_name,
           MAC2STR(mac_addr),
           status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed");
}

// Initialize WiFi for ESP-NOW
static void wifi_init(void) {
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

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // Initialize WiFi
  wifi_init();

  // Set a descriptive PCB name for this device based on its function/location
  // This is much more user-friendly than MAC addresses
  char pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH];

  // You would typically use a fixed name based on the device function
  // For example: "KITCHEN", "GARAGE", "MAIN-DOOR", "TEMP-SENSOR-1"
  // For this example, we'll use a combination of function + MAC for uniqueness
  uint8_t mac[6];
  ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
  snprintf(pcb_name, sizeof(pcb_name), "SENSOR-%02X%02X", mac[4], mac[5]);

  // Configure ESP-NOW
  espnow_config_t config = {
      .pcb_name = pcb_name,        // Set the PCB name
      .wifi_channel = 1,           // WiFi channel (must match WiFi config)
      .send_delay_ms = 1000,       // Delay between sends
      .enable_long_range = true,   // Enable long range mode
      .enable_encryption = false,  // No encryption for simplicity
      .recv_cb = on_data_received, // Callback for received data
      .send_cb = on_data_sent      // Callback for sent data
  };

  // Initialize ESP-NOW
  ESP_ERROR_CHECK(espnow_init(&config));

  // Start peer discovery (30 second timeout)
  ESP_ERROR_CHECK(espnow_start_discovery(30000));

  ESP_LOGI(TAG, "ESP-NOW initialized with PCB name: %s", pcb_name);

  // Create a task for sending messages
  xTaskCreate(send_task, "send_task", 2048, NULL, 5, NULL);

  // Main loop to process received messages
  while (1) {
    if (message_received) {
      // Process the received message here
      ESP_LOGI(TAG, "Processing message from PCB: %s", last_sender_pcb_name);

      // If it's a command-style message, you could handle it here
      if (strcmp(last_message, "GET_STATUS") == 0) {
        // Example of responding to a specific command
        if (espnow_get_peer_count() > 0) {
          // Include our PCB name in the response
          char response[64];
          snprintf(response, sizeof(response), "STATUS:%s,HEAP:%d",
                   espnow_get_peer_name(NULL), (int)esp_get_free_heap_size());
          espnow_send(last_sender_mac, response, strlen(response) + 1);

          ESP_LOGI(TAG, "Sent status response to %s", last_sender_pcb_name);
        }
      }

      // Reset the flag
      message_received = false;
    }

    // A short delay to avoid consuming too much CPU
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Task to periodically send messages
void send_task(void *pvParameter) {
  int message_count = 0;

  // Give time for peer discovery
  vTaskDelay(pdMS_TO_TICKS(5000));

  // Get our own PCB name for message inclusion
  const char *own_pcb_name = espnow_get_peer_name(NULL);

  while (1) {
    int peer_count = espnow_get_peer_count();

    if (peer_count > 0) {
      // Get the first peer and its PCB name
      uint8_t peer_mac[ESP_NOW_ETH_ALEN];
      espnow_get_peer_mac(0, peer_mac);
      const char *peer_pcb_name = espnow_get_peer_name(peer_mac);

      // Prepare message with PCB name identifiers
      char message[64];
      snprintf(message, sizeof(message),
               "Hello from PCB:%s to PCB:%s! Count: %d", own_pcb_name,
               peer_pcb_name, message_count++);

      ESP_LOGI(TAG, "Sending to PCB %s: %s", peer_pcb_name, message);
      espnow_send(peer_mac, message, strlen(message) + 1);

      // Demonstration of changing our PCB name dynamically if needed
      // This is useful for devices that change function or location
      if (message_count % 10 == 0) {
        char new_pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH];
        snprintf(new_pcb_name, sizeof(new_pcb_name), "SENSOR-%d",
                 message_count / 10);

        ESP_LOGI(TAG, "Changing PCB name to: %s", new_pcb_name);
        espnow_set_pcb_name(new_pcb_name);

        // Update our local reference
        own_pcb_name = espnow_get_peer_name(NULL);
      }
    } else {
      // No peers discovered yet, send broadcast
      ESP_LOGI(TAG, "No peers yet, sending broadcast from PCB: %s",
               own_pcb_name);

      char message[64];
      snprintf(message, sizeof(message),
               "Broadcast from PCB:%s, looking for peers", own_pcb_name);

      espnow_send(ESPNOW_BROADCAST_MAC, message, strlen(message) + 1);
    }

    // Wait before sending next message
    vTaskDelay(pdMS_TO_TICKS(10000)); // Send every 10 seconds
  }
}

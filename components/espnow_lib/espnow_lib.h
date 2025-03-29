/**
 * @file espnow_lib.h
 * @brief ESP-NOW communication library header
 *
 * A lightweight, reusable ESP-NOW communication library for ESP32 projects with
 * peer discovery, data transmission, and message handling capabilities.
 */

#ifndef ESPNOW_LIB_H
#define ESPNOW_LIB_H

#include "esp_now.h"
#include "esp_wifi.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of peers that can be managed */
#ifndef ESPNOW_MAX_PEERS
#define ESPNOW_MAX_PEERS 20
#endif

/** Maximum PCB name length */
#define ESPNOW_MAX_PCB_NAME_LENGTH 20

/** Broadcast MAC address */
extern const uint8_t ESPNOW_BROADCAST_MAC[ESP_NOW_ETH_ALEN];

/** Message types */
typedef enum {
  ESPNOW_DATA_BROADCAST,
  ESPNOW_DATA_UNICAST,
  ESPNOW_DATA_MAX,
} espnow_data_type_t;

/** Callback types */
typedef enum {
  ESPNOW_SEND_CB,
  ESPNOW_RECV_CB,
} espnow_event_id_t;

/** ESP-NOW message structure */
typedef struct {
  uint8_t type;     // Broadcast or unicast ESPNOW data
  uint8_t state;    // Indicate if broadcast data was received
  uint16_t seq_num; // Sequence number of ESPNOW data
  uint16_t crc;     // CRC16 value of ESPNOW data
  uint32_t magic;   // Magic number for device identification
  char pcb_name[ESPNOW_MAX_PCB_NAME_LENGTH]; // PCB name for identification
  uint8_t payload[0]; // Flexible array member for actual data
} __attribute__((packed)) espnow_data_t;

/** Send event callback structure */
typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  esp_now_send_status_t status;
} espnow_event_send_cb_t;

/** Receive event callback structure */
typedef struct {
  uint8_t mac_addr[ESP_NOW_ETH_ALEN];
  uint8_t *data;
  int data_len;
  int rssi; // RSSI value if available
} espnow_event_recv_cb_t;

/** User callback for data reception */
typedef void (*espnow_recv_cb_t)(const uint8_t *mac_addr, const uint8_t *data,
                                 int data_len, int rssi);

/** User callback for send status */
typedef void (*espnow_send_cb_t)(const uint8_t *mac_addr,
                                 esp_now_send_status_t status);

/** Send parameters structure */
typedef struct {
  bool unicast;    // Send unicast ESPNOW data
  bool broadcast;  // Send broadcast ESPNOW data
  uint8_t state;   // Indicate that if has received broadcast ESPNOW data or not
  uint32_t magic;  // Magic number which is used to determine which device to
                   // send unicast ESPNOW data
  uint16_t count;  // Total count of unicast ESPNOW data to be sent
  uint16_t delay;  // Delay between sending two ESPNOW data, unit: ms
  int len;         // Length of ESPNOW data to be sent, unit: byte
  uint8_t *buffer; // Buffer pointing to ESPNOW data
  uint8_t dest_mac[ESP_NOW_ETH_ALEN]; // MAC address of destination device
} espnow_send_param_t;

/** Configuration structure for the ESP-NOW library */
typedef struct {
  const char *pcb_name;     // PCB name for this device (max
                            // ESPNOW_MAX_PCB_NAME_LENGTH chars)
  uint8_t wifi_channel;     // WiFi channel to use (usually 1-13)
  uint16_t send_delay_ms;   // Delay between sending packets (ms)
  bool enable_long_range;   // Enable long range mode
  bool enable_encryption;   // Enable encryption
  const char *pmk;          // Primary Master Key (if encryption enabled)
  const char *lmk;          // Local Master Key (if encryption enabled)
  espnow_recv_cb_t recv_cb; // User callback for received data
  espnow_send_cb_t send_cb; // User callback for send status
} espnow_config_t;

/**
 * @brief Initialize the ESP-NOW library with the given configuration
 *
 * @param config Configuration structure
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_init(const espnow_config_t *config);

/**
 * @brief Deinitialize the ESP-NOW library
 *
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_deinit(void);

/**
 * @brief Send data to a specific peer
 *
 * @param mac_addr MAC address of the peer, or ESPNOW_BROADCAST_MAC for
 * broadcast
 * @param data Pointer to the data to send
 * @param len Length of the data
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_send(const uint8_t *mac_addr, const void *data, size_t len);

/**
 * @brief Get the device's own MAC address
 *
 * @param mac_addr Pointer to store the MAC address (must be at least
 * ESP_NOW_ETH_ALEN bytes)
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_get_own_mac(uint8_t *mac_addr);

/**
 * @brief Get the PCB name for a peer
 *
 * @param mac_addr MAC address of the peer, or NULL for own PCB name
 * @return const char* PCB name if available, or a string with the MAC address
 */
const char *espnow_get_peer_name(const uint8_t *mac_addr);

/**
 * @brief Set this device's PCB name
 *
 * @param pcb_name New PCB name (max ESPNOW_MAX_PCB_NAME_LENGTH chars)
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_set_pcb_name(const char *pcb_name);

/**
 * @brief Get the number of discovered peers
 *
 * @return int Number of peers
 */
int espnow_get_peer_count(void);

/**
 * @brief Get the MAC address of a peer by index
 *
 * @param index Index of the peer (0 to espnow_get_peer_count()-1)
 * @param mac_addr Pointer to store the MAC address (must be at least
 * ESP_NOW_ETH_ALEN bytes)
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_get_peer_mac(int index, uint8_t *mac_addr);

/**
 * @brief Start peer discovery process
 *
 * @param timeout_ms Maximum time to wait for peer discovery (ms)
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_start_discovery(uint32_t timeout_ms);

/**
 * @brief Add a MAC address to the trusted peers list
 *
 * @param mac_addr MAC address to add to trusted list
 * @return esp_err_t ESP_OK on success, or an error code
 */
esp_err_t espnow_add_trusted_peer(const uint8_t *mac_addr);

/**
 * @brief Check if a MAC address is in the trusted peers list
 *
 * @param mac_addr MAC address to check
 * @return bool true if trusted, false otherwise
 */
bool espnow_is_trusted_peer(const uint8_t *mac_addr);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_LIB_H */

#ifndef _ESP_NOW_WEB_H_
#define _ESP_NOW_WEB_H_

#include <esp_http_server.h>
#include <esp_now.h>
#include <stdint.h>

/**
 * @brief Initialize the RSSI web server
 *
 * @param ap_ssid SSID for the WiFi access point
 * @param ap_pass Password for the WiFi access point
 * @param ap_channel WiFi channel to use
 * @param use_existing_wifi Set to true if WiFi is already initialized
 * @return ESP_OK on success, other values on failure
 */
esp_err_t rssi_web_init(const char *ap_ssid, const char *ap_pass,
                        uint8_t ap_channel, bool use_existing_wifi);

/**
 * @brief Start the RSSI web server
 *
 * @return httpd_handle_t Handle to the server, or NULL on failure
 */
httpd_handle_t rssi_web_start(void);

/**
 * @brief Stop the RSSI web server
 *
 * @param server Server handle returned by rssi_web_start
 */
void rssi_web_stop(httpd_handle_t server);

/**
 * @brief Update RSSI data for a device
 *
 * @param mac_addr MAC address of the device
 * @param rssi RSSI value
 * @return ESP_OK on success, other values on failure
 */
esp_err_t rssi_web_update_device(const uint8_t *mac_addr, int rssi);

/**
 * @brief ESP-NOW receive callback to use with the RSSI web server
 *
 * @param recv_info ESP-NOW receive info structure
 * @param data Received data
 * @param len Length of the received data
 */
void rssi_web_espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                             const uint8_t *data, int len);

#endif /* _ESP_NOW_WEB_H_ */

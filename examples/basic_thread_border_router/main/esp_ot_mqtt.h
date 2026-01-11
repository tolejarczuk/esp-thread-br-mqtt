/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT configuration structure
 */
typedef struct {
    const char *broker_uri;      /*!< MQTT broker URI (e.g., mqtt://broker.example.com:1883) */
    const char *client_id;       /*!< MQTT client ID */
    const char *username;        /*!< MQTT username (optional, can be NULL) */
    const char *password;        /*!< MQTT password (optional, can be NULL) */
    const char *base_topic;      /*!< Base topic prefix for publishing (e.g., "esp-ot-br") */
    uint16_t udp_port;           /*!< UDP port for sending messages to Thread devices (default: 12345) */
} esp_ot_mqtt_config_t;

/**
 * @brief Device message structure for routing to Thread end devices
 */
typedef struct {
    char ext_mac[17];            /*!< Extended MAC address (e.g., "001122334455aabb") */
    char *payload;               /*!< Message payload to send to device */
    size_t payload_len;          /*!< Length of payload */
} esp_ot_device_msg_t;

/**
 * @brief Initialize and start MQTT client
 *
 * @param config MQTT configuration
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t esp_ot_mqtt_init(const esp_ot_mqtt_config_t *config);

/**
 * @brief Stop and deinitialize MQTT client
 *
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t esp_ot_mqtt_deinit(void);

/**
 * @brief Publish a message to an MQTT topic
 *
 * @param topic Topic to publish to (will be appended to base_topic)
 * @param data Message data
 * @param len Length of message data
 * @param qos Quality of Service (0, 1, or 2)
 * @param retain Retain flag
 * @return Message ID on success, -1 on failure
 */
int esp_ot_mqtt_publish(const char *topic, const char *data, int len, int qos, int retain);

/**
 * @brief Subscribe to an MQTT topic
 *
 * @param topic Topic to subscribe to (will be appended to base_topic)
 * @param qos Quality of Service (0, 1, or 2)
 * @return Message ID on success, -1 on failure
 */
int esp_ot_mqtt_subscribe(const char *topic, int qos);

/**
 * @brief Unsubscribe from an MQTT topic
 *
 * @param topic Topic to unsubscribe from
 * @return Message ID on success, -1 on failure
 */
int esp_ot_mqtt_unsubscribe(const char *topic);

/**
 * @brief Check if MQTT client is connected
 *
 * @return true if connected, false otherwise
 */
bool esp_ot_mqtt_is_connected(void);

/**
 * @brief Publish OpenThread border router status
 *
 * This function publishes the current status of the border router
 * including network information, device role, etc.
 *
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t esp_ot_mqtt_publish_br_status(void);

/**
 * @brief Send UDP message to a Thread device by extended MAC address
 *
 * This function looks up the device in the neighbor table by extended MAC,
 * resolves its IPv6 address, and sends the message via UDP.
 *
 * @param ext_mac Extended MAC address as hex string (e.g., "001122334455aabb")
 * @param payload Message payload to send
 * @param payload_len Length of payload
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t esp_ot_mqtt_send_to_device(const char *ext_mac, const char *payload, size_t payload_len);

/**
 * @brief Get list of neighbor devices and publish to MQTT
 *
 * Queries the Thread neighbor table and publishes the list of devices
 * with their extended MAC addresses and IPv6 addresses.
 *
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t esp_ot_mqtt_publish_neighbor_table(void);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_ot_mqtt.h"
#include "mqtt_credentials.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_netif_glue.h"
#include "openthread/instance.h"
#include "openthread/thread.h"
#include "openthread/dataset.h"
#include "openthread/thread_ftd.h"
#include "openthread/ip6.h"
#include "openthread/udp.h"
#include "openthread/message.h"
#include "openthread/link.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_ot_mqtt";

// Device registry for Extended MAC to ML-EID mapping
#define MAX_REGISTERED_DEVICES MQTT_MAX_REGISTERED_DEVICES

typedef struct {
    uint8_t ext_mac[8];           // Extended MAC address
    struct in6_addr ml_eid;       // ML-EID IPv6 address
    bool in_use;                  // Is this entry active?
    uint32_t last_seen;           // Timestamp of last registration/activity
} device_registry_entry_t;

static device_registry_entry_t s_device_registry[MAX_REGISTERED_DEVICES];

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static char s_base_topic[128] = {0};
static bool s_mqtt_connected = false;
static uint16_t s_udp_port = 12345;  // Default UDP port for device communication

// ML-EID request state
static SemaphoreHandle_t s_ml_eid_response_sem = NULL;
static char s_pending_mac_request[17] = {0};
static bool s_ml_eid_request_pending = false;

/**
 * @brief MQTT event handler
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        s_mqtt_connected = true;
        
        // Auto-subscribe to command topics
        char cmd_topic[256];
        snprintf(cmd_topic, sizeof(cmd_topic), "%s/cmd/#", s_base_topic);
        esp_mqtt_client_subscribe(client, cmd_topic, 1);
        ESP_LOGI(TAG, "Subscribed to %s", cmd_topic);
        
        // Publish online status
        char status_topic[256];
        snprintf(status_topic, sizeof(status_topic), "%s/status", s_base_topic);
        esp_mqtt_client_publish(client, status_topic, "online", 0, 1, 1);
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        s_mqtt_connected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
        
        // Check if this is a device message command
        char topic_str[256] = {0};
        snprintf(topic_str, sizeof(topic_str), "%.*s", event->topic_len, event->topic);
        
        // Check for device registration messages
        char register_topic[256];
        snprintf(register_topic, sizeof(register_topic), "%s/cmd/register", s_base_topic);
        
        if (strstr(topic_str, register_topic) != NULL) {
            // Parse JSON: {"mac":"ca7bf088c6e9bb2a", "ml_eid":"fd64:f0dd:8948:b4b1:4d0e:fbfa:552f:b1cd"}
            char *data_copy = strndup(event->data, event->data_len);
            if (data_copy) {
                cJSON *json = cJSON_Parse(data_copy);
                if (json) {
                    cJSON *mac = cJSON_GetObjectItem(json, "mac");
                    cJSON *ml_eid = cJSON_GetObjectItem(json, "ml_eid");
                    
                    if (cJSON_IsString(mac) && cJSON_IsString(ml_eid)) {
                        ESP_LOGI(TAG, "Device registration: MAC=%s, ML-EID=%s", 
                                mac->valuestring, ml_eid->valuestring);
                        esp_err_t ret = esp_ot_mqtt_register_device(mac->valuestring, ml_eid->valuestring);
                        if (ret == ESP_OK) {
                            ESP_LOGI(TAG, "Device registered successfully");
                            
                            // Check if this is a response to our ML-EID request
                            if (s_ml_eid_request_pending && 
                                strcmp(s_pending_mac_request, mac->valuestring) == 0) {
                                // Signal that we received the ML-EID response
                                s_ml_eid_request_pending = false;
                                if (s_ml_eid_response_sem) {
                                    xSemaphoreGive(s_ml_eid_response_sem);
                                }
                            }
                        } else {
                            ESP_LOGW(TAG, "Failed to register device");
                        }
                    }
                    cJSON_Delete(json);
                }
                free(data_copy);
            }
        }
        
        char device_cmd_topic[256];
        snprintf(device_cmd_topic, sizeof(device_cmd_topic), "%s/cmd/device", s_base_topic);
        
        if (strstr(topic_str, device_cmd_topic) != NULL) {
            // Parse JSON message: {"mac":"001122334455aabb", "payload":"data"}
            char *data_copy = strndup(event->data, event->data_len);
            if (data_copy) {
                cJSON *json = cJSON_Parse(data_copy);
                if (json) {
                    cJSON *mac = cJSON_GetObjectItem(json, "mac");
                    cJSON *payload = cJSON_GetObjectItem(json, "payload");
                    
                    if (cJSON_IsString(mac) && cJSON_IsString(payload)) {
                        ESP_LOGI(TAG, "Routing message to device MAC: %s", mac->valuestring);
                        esp_err_t ret = esp_ot_mqtt_send_to_device(mac->valuestring, 
                                                                   payload->valuestring, 
                                                                   strlen(payload->valuestring));
                        if (ret == ESP_OK) {
                            ESP_LOGI(TAG, "Message sent to device successfully");
                        } else {
                            ESP_LOGW(TAG, "Failed to send message to device");
                        }
                    } else {
                        ESP_LOGW(TAG, "Invalid JSON format, expected 'mac' and 'payload' fields");
                    }
                    cJSON_Delete(json);
                } else {
                    ESP_LOGW(TAG, "Failed to parse JSON");
                }
                free(data_copy);
            }
        }
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGE(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGE(TAG, "Last captured errno : %d (%s)",  event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
        
    default:
        ESP_LOGD(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

esp_err_t esp_ot_mqtt_init(const esp_ot_mqtt_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid MQTT configuration");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mqtt_client != NULL) {
        ESP_LOGW(TAG, "MQTT client already initialized");
        return ESP_OK;
    }

    // Create semaphore for ML-EID request synchronization
    if (s_ml_eid_response_sem == NULL) {
        s_ml_eid_response_sem = xSemaphoreCreateBinary();
        if (s_ml_eid_response_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create ML-EID response semaphore");
            return ESP_FAIL;
        }
    }

    // Store base topic and UDP port
    snprintf(s_base_topic, sizeof(s_base_topic), "%s", config->base_topic ? config->base_topic : "esp-ot-br");
    s_udp_port = config->udp_port ? config->udp_port : 12345;

    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->broker_uri,
        .credentials.client_id = config->client_id,
        .credentials.username = config->username,
        .credentials.authentication.password = config->password,
        .session.last_will.topic = s_base_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };

    // Configure TLS if using mqtts://
    if (config->use_global_ca_store) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
        ESP_LOGI(TAG, "TLS enabled with certificate bundle verification");
    }

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    // Register event handler
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // Start MQTT client
    esp_err_t ret = esp_mqtt_client_start(s_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "MQTT client initialized and started");
    return ESP_OK;
}

esp_err_t esp_ot_mqtt_deinit(void)
{
    if (s_mqtt_client == NULL) {
        return ESP_OK;
    }

    // Publish offline status before disconnecting
    if (s_mqtt_connected) {
        char status_topic[256];
        snprintf(status_topic, sizeof(status_topic), "%s/status", s_base_topic);
        esp_mqtt_client_publish(s_mqtt_client, status_topic, "offline", 0, 1, 1);
    }

    esp_mqtt_client_stop(s_mqtt_client);
    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_mqtt_connected = false;

    ESP_LOGI(TAG, "MQTT client deinitialized");
    return ESP_OK;
}

int esp_ot_mqtt_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }

    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT client not connected");
        return -1;
    }

    char full_topic[256];
    snprintf(full_topic, sizeof(full_topic), "%s/%s", s_base_topic, topic);

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, full_topic, data, len, qos, retain);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", full_topic);
    } else {
        ESP_LOGD(TAG, "Published to %s, msg_id=%d", full_topic, msg_id);
    }

    return msg_id;
}

int esp_ot_mqtt_subscribe(const char *topic, int qos)
{
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }

    char full_topic[256];
    snprintf(full_topic, sizeof(full_topic), "%s/%s", s_base_topic, topic);

    int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, full_topic, qos);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to subscribe to %s", full_topic);
    } else {
        ESP_LOGI(TAG, "Subscribed to %s, msg_id=%d", full_topic, msg_id);
    }

    return msg_id;
}

int esp_ot_mqtt_unsubscribe(const char *topic)
{
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return -1;
    }

    char full_topic[256];
    snprintf(full_topic, sizeof(full_topic), "%s/%s", s_base_topic, topic);

    int msg_id = esp_mqtt_client_unsubscribe(s_mqtt_client, full_topic);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to unsubscribe from %s", full_topic);
    } else {
        ESP_LOGI(TAG, "Unsubscribed from %s, msg_id=%d", full_topic, msg_id);
    }

    return msg_id;
}

bool esp_ot_mqtt_is_connected(void)
{
    return s_mqtt_connected;
}

esp_err_t esp_ot_mqtt_publish_br_status(void)
{
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skipping status publish");
        return ESP_ERR_INVALID_STATE;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        ESP_LOGE(TAG, "OpenThread instance not available");
        return ESP_FAIL;
    }

    // Get Thread network state
    otDeviceRole role = otThreadGetDeviceRole(instance);
    const char *role_str = "unknown";
    switch (role) {
        case OT_DEVICE_ROLE_DISABLED: role_str = "disabled"; break;
        case OT_DEVICE_ROLE_DETACHED: role_str = "detached"; break;
        case OT_DEVICE_ROLE_CHILD: role_str = "child"; break;
        case OT_DEVICE_ROLE_ROUTER: role_str = "router"; break;
        case OT_DEVICE_ROLE_LEADER: role_str = "leader"; break;
        default: break;
    }

    // Build JSON status message
    char status_msg[512];
    int len = snprintf(status_msg, sizeof(status_msg),
                      "{\"role\":\"%s\",\"rloc16\":\"0x%04x\"}",
                      role_str,
                      otThreadGetRloc16(instance));

    // Publish status
    esp_ot_mqtt_publish("thread/status", status_msg, len, 1, 0);

    // Publish network name if available
    if (role != OT_DEVICE_ROLE_DISABLED && role != OT_DEVICE_ROLE_DETACHED) {
        const char *network_name = otThreadGetNetworkName(instance);
        if (network_name) {
            esp_ot_mqtt_publish("thread/network_name", network_name, strlen(network_name), 1, 1);
        }
    }

    ESP_LOGI(TAG, "Published border router status");
    return ESP_OK;
}

/**
 * @brief Convert hex string to byte array
 */
static esp_err_t hex_string_to_bytes(const char *hex_str, uint8_t *bytes, size_t bytes_len)
{
    if (strlen(hex_str) != bytes_len * 2) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < bytes_len; i++) {
        char byte_str[3] = {hex_str[i * 2], hex_str[i * 2 + 1], '\0'};
        bytes[i] = (uint8_t)strtol(byte_str, NULL, 16);
    }

    return ESP_OK;
}

/**
 * @brief Register a Thread device's ML-EID address
 */
esp_err_t esp_ot_mqtt_register_device(const char *ext_mac, const char *ml_eid_str)
{
    if (!ext_mac || !ml_eid_str) {
        return ESP_ERR_INVALID_ARG;
    }

    // Convert MAC string to bytes
    uint8_t mac_bytes[8];
    if (hex_string_to_bytes(ext_mac, mac_bytes, 8) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid MAC address format: %s", ext_mac);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse ML-EID IPv6 address
    struct in6_addr ml_eid;
    if (inet_pton(AF_INET6, ml_eid_str, &ml_eid) != 1) {
        ESP_LOGE(TAG, "Invalid ML-EID address format: %s", ml_eid_str);
        return ESP_ERR_INVALID_ARG;
    }

    // Check if device already registered - update if found
    for (int i = 0; i < MAX_REGISTERED_DEVICES; i++) {
        if (s_device_registry[i].in_use && 
            memcmp(s_device_registry[i].ext_mac, mac_bytes, 8) == 0) {
            // Update existing entry
            memcpy(&s_device_registry[i].ml_eid, &ml_eid, sizeof(struct in6_addr));
            s_device_registry[i].last_seen = xTaskGetTickCount();
            ESP_LOGI(TAG, "Updated device registry: MAC=%s, ML-EID=%s", ext_mac, ml_eid_str);
            return ESP_OK;
        }
    }

    // Find empty slot for new registration
    for (int i = 0; i < MAX_REGISTERED_DEVICES; i++) {
        if (!s_device_registry[i].in_use) {
            memcpy(s_device_registry[i].ext_mac, mac_bytes, 8);
            memcpy(&s_device_registry[i].ml_eid, &ml_eid, sizeof(struct in6_addr));
            s_device_registry[i].in_use = true;
            s_device_registry[i].last_seen = xTaskGetTickCount();
            ESP_LOGI(TAG, "Registered new device: MAC=%s, ML-EID=%s", ext_mac, ml_eid_str);
            
            // Publish device registration notification
            if (s_mqtt_client && s_mqtt_connected) {
                char notify_topic[256];
                char notify_msg[512];
                snprintf(notify_topic, sizeof(notify_topic), "%s/notify/device_registered", s_base_topic);
                snprintf(notify_msg, sizeof(notify_msg), 
                        "{\"mac\":\"%s\",\"ml_eid\":\"%s\",\"timestamp\":%lu}",
                        ext_mac, ml_eid_str, (unsigned long)xTaskGetTickCount());
                esp_mqtt_client_publish(s_mqtt_client, notify_topic, notify_msg, 0, 1, 0);
                ESP_LOGI(TAG, "Published device registration notification");
            }
            
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Device registry full, cannot register MAC=%s", ext_mac);
    return ESP_ERR_NO_MEM;
}

/**
 * @brief Look up ML-EID for a device in the registry
 */
static esp_err_t lookup_device_ml_eid(const uint8_t *ext_mac, struct in6_addr *ml_eid)
{
    for (int i = 0; i < MAX_REGISTERED_DEVICES; i++) {
        if (s_device_registry[i].in_use && 
            memcmp(s_device_registry[i].ext_mac, ext_mac, 8) == 0) {
            memcpy(ml_eid, &s_device_registry[i].ml_eid, sizeof(struct in6_addr));
            s_device_registry[i].last_seen = xTaskGetTickCount();
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/**
 * @brief Find IPv6 address of device by extended MAC in neighbor table
 */
static esp_err_t find_device_ipv6_by_mac(const char *ext_mac, otIp6Address *ipv6_addr)
{
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        ESP_LOGE(TAG, "OpenThread instance not available");
        return ESP_FAIL;
    }

    // Convert MAC string to bytes
    uint8_t target_mac[8];
    if (hex_string_to_bytes(ext_mac, target_mac, 8) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid MAC address format: %s", ext_mac);
        return ESP_ERR_INVALID_ARG;
    }

    // First check device registry for ML-EID
    struct in6_addr ml_eid;
    if (lookup_device_ml_eid(target_mac, &ml_eid) == ESP_OK) {
        // Use registered ML-EID (stable address)
        memcpy(ipv6_addr->mFields.m8, &ml_eid, sizeof(struct in6_addr));
        
        char ipv6_str[OT_IP6_ADDRESS_STRING_SIZE];
        otIp6AddressToString(ipv6_addr, ipv6_str, sizeof(ipv6_str));
        ESP_LOGI(TAG, "Using registered ML-EID for device %s: %s", ext_mac, ipv6_str);
        return ESP_OK;
    }

    // Device not in registry - request ML-EID from backend
    if (s_mqtt_client && s_mqtt_connected && !s_ml_eid_request_pending) {
        ESP_LOGI(TAG, "Device %s not in registry, requesting ML-EID from backend", ext_mac);
        
        // Mark request as pending
        s_ml_eid_request_pending = true;
        snprintf(s_pending_mac_request, sizeof(s_pending_mac_request), "%s", ext_mac);
        
        // Publish ML-EID request
        char request_topic[256];
        char request_msg[256];
        snprintf(request_topic, sizeof(request_topic), "%s/request/ml_eid", s_base_topic);
        snprintf(request_msg, sizeof(request_msg), "{\"mac\":\"%s\"}", ext_mac);
        esp_mqtt_client_publish(s_mqtt_client, request_topic, request_msg, 0, 1, 0);
        ESP_LOGI(TAG, "Published ML-EID request for device %s, waiting for response...", ext_mac);
        
        // Wait for response (timeout from config)
        if (s_ml_eid_response_sem) {
            if (xSemaphoreTake(s_ml_eid_response_sem, pdMS_TO_TICKS(MQTT_ML_EID_REQUEST_TIMEOUT_MS)) == pdTRUE) {
                ESP_LOGI(TAG, "Received ML-EID response, retrying lookup");
                // Retry lookup after receiving response
                if (lookup_device_ml_eid(target_mac, &ml_eid) == ESP_OK) {
                    memcpy(ipv6_addr->mFields.m8, &ml_eid, sizeof(struct in6_addr));
                    char ipv6_str[OT_IP6_ADDRESS_STRING_SIZE];
                    otIp6AddressToString(ipv6_addr, ipv6_str, sizeof(ipv6_str));
                    ESP_LOGI(TAG, "Using ML-EID from backend response: %s", ipv6_str);
                    return ESP_OK;
                }
            } else {
                ESP_LOGW(TAG, "ML-EID request timeout, falling back to RLOC16");
                s_ml_eid_request_pending = false;
            }
        }
    }

    ESP_LOGI(TAG, "Device %s not in registry, checking neighbor table for RLOC16", ext_mac);

    // Fallback: Iterate through neighbor table for RLOC16-based address
    otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor_info;

    while (otThreadGetNextNeighborInfo(instance, &iterator, &neighbor_info) == OT_ERROR_NONE) {
        // Compare extended MAC addresses
        if (memcmp(neighbor_info.mExtAddress.m8, target_mac, 8) == 0) {
            // Found the device - use RLOC16-based mesh-local address
            // Note: This is less stable than ML-EID and may change
            uint16_t rloc16 = neighbor_info.mRloc16;
            
            // Get mesh-local prefix
            const otMeshLocalPrefix *ml_prefix = otThreadGetMeshLocalPrefix(instance);
            if (ml_prefix == NULL) {
                ESP_LOGE(TAG, "Failed to get mesh-local prefix");
                return ESP_FAIL;
            }

            // Construct RLOC16-based mesh-local IPv6 address
            memcpy(ipv6_addr->mFields.m8, ml_prefix->m8, 8);
            ipv6_addr->mFields.m8[8] = 0x00;
            ipv6_addr->mFields.m8[9] = 0x00;
            ipv6_addr->mFields.m8[10] = 0x00;
            ipv6_addr->mFields.m8[11] = 0xff;
            ipv6_addr->mFields.m8[12] = 0xfe;
            ipv6_addr->mFields.m8[13] = 0x00;
            ipv6_addr->mFields.m8[14] = (uint8_t)(rloc16 >> 8);
            ipv6_addr->mFields.m8[15] = (uint8_t)(rloc16 & 0xff);

            char ipv6_str[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(ipv6_addr, ipv6_str, sizeof(ipv6_str));
            ESP_LOGW(TAG, "Device %s not registered - using unstable RLOC16 address: %s", ext_mac, ipv6_str);
            ESP_LOGW(TAG, "Please register device to use stable ML-EID addressing");
            
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "Device with MAC %s not found in neighbor table", ext_mac);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_ot_mqtt_send_to_device(const char *ext_mac, const char *payload, size_t payload_len)
{
    if (ext_mac == NULL || payload == NULL || payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    otIp6Address device_ipv6;
    esp_err_t ret = find_device_ipv6_by_mac(ext_mac, &device_ipv6);
    if (ret != ESP_OK) {
        return ret;
    }

    // Convert OpenThread IPv6 address to string for socket API
    char ipv6_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&device_ipv6, ipv6_str, sizeof(ipv6_str));

    // Create UDP socket
    int sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return ESP_FAIL;
    }

    // Setup destination address
    struct sockaddr_in6 dest_addr = {0};
    dest_addr.sin6_family = AF_INET6;
    dest_addr.sin6_port = htons(s_udp_port);
    inet_pton(AF_INET6, ipv6_str, &dest_addr.sin6_addr);

    // Send UDP packet
    int sent_bytes = sendto(sock, payload, payload_len, 0, 
                           (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    
    close(sock);

    if (sent_bytes < 0) {
        ESP_LOGE(TAG, "Failed to send UDP packet: %d", errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sent %d bytes to device %s at %s:%d", sent_bytes, ext_mac, ipv6_str, s_udp_port);
    
    // Publish confirmation to MQTT
    if (s_mqtt_connected) {
        char response[256];
        int resp_len = snprintf(response, sizeof(response),
                               "{\"mac\":\"%s\",\"status\":\"sent\",\"bytes\":%d}",
                               ext_mac, sent_bytes);
        esp_ot_mqtt_publish("device/response", response, resp_len, 1, 0);
    }

    return ESP_OK;
}

esp_err_t esp_ot_mqtt_publish_neighbor_table(void)
{
    if (!s_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT not connected, skipping neighbor table publish");
        return ESP_ERR_INVALID_STATE;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        ESP_LOGE(TAG, "OpenThread instance not available");
        return ESP_FAIL;
    }

    cJSON *neighbors_array = cJSON_CreateArray();
    if (neighbors_array == NULL) {
        return ESP_ERR_NO_MEM;
    }

    otNeighborInfoIterator iterator = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor_info;
    int count = 0;

    while (otThreadGetNextNeighborInfo(instance, &iterator, &neighbor_info) == OT_ERROR_NONE) {
        cJSON *neighbor_obj = cJSON_CreateObject();
        if (neighbor_obj == NULL) {
            continue;
        }

        // Format extended MAC address
        char mac_str[17];
        snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x%02x%02x",
                neighbor_info.mExtAddress.m8[0], neighbor_info.mExtAddress.m8[1],
                neighbor_info.mExtAddress.m8[2], neighbor_info.mExtAddress.m8[3],
                neighbor_info.mExtAddress.m8[4], neighbor_info.mExtAddress.m8[5],
                neighbor_info.mExtAddress.m8[6], neighbor_info.mExtAddress.m8[7]);

        // Construct RLOC16-based mesh-local address
        // Note: Devices also have ML-EID addresses, but we can't derive them
        // from Extended MAC (OpenThread uses hash-based IID for ML-EID)
        otIp6Address rloc_addr;
        const otMeshLocalPrefix *ml_prefix = otThreadGetMeshLocalPrefix(instance);
        if (ml_prefix) {
            memcpy(rloc_addr.mFields.m8, ml_prefix->m8, 8);
            rloc_addr.mFields.m8[8] = 0x00;
            rloc_addr.mFields.m8[9] = 0x00;
            rloc_addr.mFields.m8[10] = 0x00;
            rloc_addr.mFields.m8[11] = 0xff;
            rloc_addr.mFields.m8[12] = 0xfe;
            rloc_addr.mFields.m8[13] = 0x00;
            rloc_addr.mFields.m8[14] = (uint8_t)(neighbor_info.mRloc16 >> 8);
            rloc_addr.mFields.m8[15] = (uint8_t)(neighbor_info.mRloc16 & 0xff);

            char rloc_str[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(&rloc_addr, rloc_str, sizeof(rloc_str));

            cJSON_AddStringToObject(neighbor_obj, "mac", mac_str);
            cJSON_AddStringToObject(neighbor_obj, "ipv6", rloc_str);
            cJSON_AddNumberToObject(neighbor_obj, "rloc16", neighbor_info.mRloc16);
            cJSON_AddNumberToObject(neighbor_obj, "age", neighbor_info.mAge);
            cJSON_AddNumberToObject(neighbor_obj, "linkQuality", neighbor_info.mLinkQualityIn);

            cJSON_AddItemToArray(neighbors_array, neighbor_obj);
            count++;
        } else {
            cJSON_Delete(neighbor_obj);
        }
    }

    // Create root object
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddItemToObject(root, "neighbors", neighbors_array);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        esp_ot_mqtt_publish("neighbors", json_str, strlen(json_str), 1, 0);
        ESP_LOGI(TAG, "Published neighbor table with %d devices", count);
        cJSON_free(json_str);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

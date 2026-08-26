/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <protocomm_ext.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Endpoint names matching espressif/network_provisioning (device side). */
#define NETWORK_PROVISIONER_EP_PROTO_VER   "proto-ver"
#define NETWORK_PROVISIONER_EP_SESSION     "prov-session"
#define NETWORK_PROVISIONER_EP_CONFIG      "prov-config"
#define NETWORK_PROVISIONER_EP_SCAN        "prov-scan"
#define NETWORK_PROVISIONER_EP_CTRL        "prov-ctrl"

/**
 * BLE characteristic UUID16 values used by network_provisioning / wifi_provisioning
 * (see wifi_prov manager set_config_endpoint). Register via
 * protocomm_ext_set_config_endpoint() before BLE sessions.
 */
#define NETWORK_PROVISIONER_EP_CTRL_UUID       0xFF4F
#define NETWORK_PROVISIONER_EP_SCAN_UUID       0xFF50
#define NETWORK_PROVISIONER_EP_SESSION_UUID    0xFF51
#define NETWORK_PROVISIONER_EP_CONFIG_UUID     0xFF52
#define NETWORK_PROVISIONER_EP_PROTO_VER_UUID  0xFF53

/**
 * Upper bound on Wi-Fi / Thread scan result entries accepted from a peer
 * (request `count` and reported `result_count`).
 */
#define NETWORK_PROVISIONER_MAX_SCAN_RESULTS  64

/**
 * @brief Opaque network provisioner instance returned by network_provisioner_create()
 */
typedef struct network_provisioner network_provisioner_t;

/**
 * @brief Capabilities parsed from the peer proto-ver JSON
 */
typedef struct {
    bool wifi_prov;     /**< Peer supports Wi-Fi provisioning */
    bool wifi_scan;     /**< Peer supports Wi-Fi scan */
    bool thread_prov;   /**< Peer supports Thread provisioning */
    bool thread_scan;   /**< Peer supports Thread scan */
    bool no_sec;        /**< Peer advertises no-security (Sec0) */
    bool no_pop;        /**< Peer does not require Proof-of-Possession */
    /**
     * Security scheme from proto-ver JSON (`prov.sec_ver`, 0 / 1 / 2).
     * Required when a `prov` object is present; bare version strings fall back to 1.
     */
    int sec_ver;
} network_provisioner_capabilities_t;

typedef enum {
    NETWORK_PROVISIONER_WIFI_STATE_CONNECTED = 0,       /*!< STA connected */
    NETWORK_PROVISIONER_WIFI_STATE_CONNECTING,          /*!< STA connecting */
    NETWORK_PROVISIONER_WIFI_STATE_DISCONNECTED,        /*!< STA disconnected */
    NETWORK_PROVISIONER_WIFI_STATE_CONNECTION_FAILED,   /*!< STA connection failed */
} network_provisioner_wifi_state_t;

typedef enum {
    NETWORK_PROVISIONER_WIFI_FAIL_AUTH_ERROR = 0,       /*!< Authentication failed */
    NETWORK_PROVISIONER_WIFI_FAIL_NETWORK_NOT_FOUND,    /*!< AP not found */
    NETWORK_PROVISIONER_WIFI_FAIL_UNKNOWN = -1,         /*!< Unspecified failure */
} network_provisioner_wifi_fail_reason_t;

typedef enum {
    NETWORK_PROVISIONER_THREAD_STATE_ATTACHED = 0,          /*!< Attached to Thread network */
    NETWORK_PROVISIONER_THREAD_STATE_ATTACHING,             /*!< Attaching */
    NETWORK_PROVISIONER_THREAD_STATE_DETACHED,              /*!< Detached */
    NETWORK_PROVISIONER_THREAD_STATE_ATTACHING_FAILED,      /*!< Attach failed */
} network_provisioner_thread_state_t;

typedef enum {
    NETWORK_PROVISIONER_THREAD_FAIL_DATASET_INVALID = 0,    /*!< Dataset invalid */
    NETWORK_PROVISIONER_THREAD_FAIL_NETWORK_NOT_FOUND,      /*!< Network not found */
    NETWORK_PROVISIONER_THREAD_FAIL_UNKNOWN = -1,           /*!< Unspecified failure */
} network_provisioner_thread_fail_reason_t;

/**
 * @brief Wi-Fi STA status reported by the peer
 */
typedef struct {
    network_provisioner_wifi_state_t state;             /**< Connection state */
    network_provisioner_wifi_fail_reason_t fail_reason; /**< Failure reason when disconnected/failed */
    char ip4_addr[16];                                  /**< IPv4 address string, or empty */
    char ssid[33];                                      /**< Connected SSID */
    int32_t channel;                                    /**< Channel, or 0 if unknown */
} network_provisioner_wifi_status_t;

/**
 * @brief Thread network status reported by the peer
 */
typedef struct {
    network_provisioner_thread_state_t state;               /**< Attach state */
    network_provisioner_thread_fail_reason_t fail_reason;   /**< Failure reason when attach failed */
    uint32_t pan_id;                                        /**< PAN ID */
    uint32_t channel;                                       /**< Channel */
    char name[33];                                          /**< Network name */
} network_provisioner_thread_status_t;

/**
 * @brief One Wi-Fi AP entry from a peer scan
 */
typedef struct {
    uint8_t ssid[33];       /**< SSID bytes */
    uint8_t ssid_len;       /**< SSID length */
    uint8_t bssid[6];       /**< BSSID */
    uint32_t channel;       /**< Channel */
    int32_t rssi;           /**< RSSI in dBm */
    int auth_mode;          /**< Wi-Fi auth mode (wifi_auth_mode_t) */
} network_provisioner_wifi_ap_t;

/**
 * @brief One Thread network entry from a peer scan
 */
typedef struct {
    uint32_t pan_id;            /**< PAN ID */
    uint32_t channel;           /**< Channel */
    int32_t rssi;               /**< RSSI in dBm */
    uint32_t lqi;               /**< Link quality indicator */
    char network_name[33];      /**< Network name */
    uint8_t ext_pan_id[8];      /**< Extended PAN ID */
    uint8_t ext_addr[8];        /**< Extended address */
} network_provisioner_thread_network_t;

/**
 * @brief Wi-Fi credentials for network_provisioner_provision_wifi()
 */
typedef struct {
    const uint8_t *ssid;            /**< SSID bytes */
    size_t ssid_len;                /**< SSID length */
    const uint8_t *passphrase;      /**< Passphrase bytes; may be NULL for open AP */
    size_t passphrase_len;          /**< Passphrase length */
    const uint8_t *bssid;           /**< Optional BSSID, 6 bytes or NULL */
    int32_t channel;                /**< 0 = any channel */
    uint32_t poll_timeout_ms;       /**< Status poll timeout; 0 uses the default */
    uint32_t poll_interval_ms;      /**< Status poll interval; 0 uses the default */
} network_provisioner_wifi_creds_t;

/**
 * @brief Thread credentials for network_provisioner_provision_thread()
 */
typedef struct {
    const uint8_t *dataset;         /**< Active Operational Dataset bytes */
    size_t dataset_len;             /**< Dataset length */
    uint32_t poll_timeout_ms;       /**< Status poll timeout; 0 uses the default */
    uint32_t poll_interval_ms;      /**< Status poll interval; 0 uses the default */
} network_provisioner_thread_creds_t;

/**
 * @brief Wrap an existing protocomm_ext instance (session may not be open yet)
 *
 * Ownership of @p pc remains with the caller; call network_provisioner_delete()
 * before protocomm_ext_delete().
 *
 * Registers BLE endpoint UUID mappings (`NETWORK_PROVISIONER_EP_*_UUID`) via
 * `protocomm_ext_set_config_endpoint()` (no-op for HTTP / Console).
 *
 * @note APIs on a given instance are not thread-safe. Use one task, or
 *       serialize externally. Do not call network_provisioner_delete() or
 *       network_provisioner_stop_session() while another call is in progress.
 */
network_provisioner_t *network_provisioner_create(protocomm_ext_t *pc);

/**
 * @brief Free a network_provisioner instance
 *
 * Does not delete the wrapped protocomm_ext instance.
 */
void network_provisioner_delete(network_provisioner_t *np);

/**
 * @brief open_session → get_version(proto-ver) → establish_security(prov-session)
 *
 * Requires the protocomm_ext instance to already use the peer's security scheme
 * (Sec0/1/2) and credentials.
 *
 * For runtime discovery from proto-ver (recommended):
 *   1. network_provisioner_fetch_capabilities()
 *   2. protocomm_ext_set_security() with peer sec_ver / PoP or Sec2 creds
 *   3. network_provisioner_establish_security()
 *
 * @param np Network provisioner instance
 * @param transport_connect_cfg HTTP: NULL; BLE: ble_addr_t *; Console: NULL
 */
esp_err_t network_provisioner_start_session(network_provisioner_t *np,
                                            const void *transport_connect_cfg);

/**
 * @brief open_session + read proto-ver (plaintext); does not run the security handshake
 *
 * Stores capabilities for network_provisioner_get_capabilities().
 */
esp_err_t network_provisioner_fetch_capabilities(network_provisioner_t *np,
                                                 const void *transport_connect_cfg);

/**
 * @brief security_init + establish_security after credentials are configured
 */
esp_err_t network_provisioner_establish_security(network_provisioner_t *np);

/**
 * @brief Close the transport session
 */
esp_err_t network_provisioner_stop_session(network_provisioner_t *np);

/**
 * @brief Copy capabilities stored by fetch_capabilities()
 */
esp_err_t network_provisioner_get_capabilities(network_provisioner_t *np,
                                               network_provisioner_capabilities_t *caps);

/* ---- low-level config ---- */
/** @brief Send Wi-Fi SetConfig on `prov-config` */
esp_err_t network_provisioner_wifi_set_config(network_provisioner_t *np,
                                              const uint8_t *ssid, size_t ssid_len,
                                              const uint8_t *passphrase, size_t passphrase_len,
                                              const uint8_t *bssid, int32_t channel);
/** @brief Send Wi-Fi ApplyConfig on `prov-config` */
esp_err_t network_provisioner_wifi_apply_config(network_provisioner_t *np);
/** @brief Query Wi-Fi STA status from the peer */
esp_err_t network_provisioner_wifi_get_status(network_provisioner_t *np,
                                              network_provisioner_wifi_status_t *status);

/** @brief Send Thread SetConfig (Active Operational Dataset) */
esp_err_t network_provisioner_thread_set_config(network_provisioner_t *np,
                                                const uint8_t *dataset, size_t dataset_len);
/** @brief Send Thread ApplyConfig */
esp_err_t network_provisioner_thread_apply_config(network_provisioner_t *np);
/** @brief Query Thread attach status from the peer */
esp_err_t network_provisioner_thread_get_status(network_provisioner_t *np,
                                                network_provisioner_thread_status_t *status);

/* ---- scan ---- */
/** @brief Start a Wi-Fi scan on the peer */
esp_err_t network_provisioner_wifi_scan_start(network_provisioner_t *np, bool blocking,
                                              bool passive, uint32_t group_channels,
                                              uint32_t period_ms);
/** @brief Query whether the peer Wi-Fi scan has finished */
esp_err_t network_provisioner_wifi_scan_status(network_provisioner_t *np,
                                               bool *finished, uint32_t *result_count);
/** @brief Fetch Wi-Fi scan results from the peer */
esp_err_t network_provisioner_wifi_scan_result(network_provisioner_t *np,
                                               uint32_t start_index, uint32_t count,
                                               network_provisioner_wifi_ap_t *out,
                                               uint32_t *out_count);

/** @brief Start a Thread scan on the peer */
esp_err_t network_provisioner_thread_scan_start(network_provisioner_t *np, bool blocking,
                                                uint32_t channel_mask);
/** @brief Query whether the peer Thread scan has finished */
esp_err_t network_provisioner_thread_scan_status(network_provisioner_t *np,
                                                 bool *finished, uint32_t *result_count);
/** @brief Fetch Thread scan results from the peer */
esp_err_t network_provisioner_thread_scan_result(network_provisioner_t *np,
                                                 uint32_t start_index, uint32_t count,
                                                 network_provisioner_thread_network_t *out,
                                                 uint32_t *out_count);

/* ---- ctrl ---- */
/** @brief Reset Wi-Fi provisioning state on the peer */
esp_err_t network_provisioner_wifi_reset(network_provisioner_t *np);
/** @brief Re-enable Wi-Fi provisioning on the peer */
esp_err_t network_provisioner_wifi_reprov(network_provisioner_t *np);
/** @brief Reset Thread provisioning state on the peer */
esp_err_t network_provisioner_thread_reset(network_provisioner_t *np);
/** @brief Re-enable Thread provisioning on the peer */
esp_err_t network_provisioner_thread_reprov(network_provisioner_t *np);

/* ---- high-level ---- */
/**
 * @brief Set + Apply + poll until Connected or fail/timeout
 */
esp_err_t network_provisioner_provision_wifi(network_provisioner_t *np,
                                             const network_provisioner_wifi_creds_t *creds,
                                             network_provisioner_wifi_status_t *out_status);

/**
 * @brief Set + Apply Thread dataset and poll until attached or fail/timeout
 */
esp_err_t network_provisioner_provision_thread(network_provisioner_t *np,
                                               const network_provisioner_thread_creds_t *creds,
                                               network_provisioner_thread_status_t *out_status);

/**
 * @brief Return the wrapped protocomm_ext instance (caller still owns it)
 */
protocomm_ext_t *network_provisioner_get_protocomm(network_provisioner_t *np);

#ifdef __cplusplus
}
#endif

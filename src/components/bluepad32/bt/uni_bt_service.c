// SPDX-License-Identifier: Apache-2.0
// Copyright 2023 Ricardo Quesada
// http://retro.moe/unijoysticle2

#include "bt/uni_bt_service.h"

#include <btstack.h>

#include "bt/uni_bt.h"
#include "bt/uni_bt_le.h"
#include "bt/uni_bt_service.gatt.h"
#include "uni_common.h"
#include "uni_log.h"
#include "uni_version.h"

// General Discoverable = 0x02
// BR/EDR Not supported = 0x04
#define APP_AD_FLAGS 0x06

// HID name, truncated to smaller value.
#define HID_NAME_COMPACT_LEN 16
_Static_assert(HID_NAME_COMPACT_LEN <= HID_MAX_NAME_LEN, "Truncated name is bigger than original name");
// Max number of clients that can connect to the service at the same time.
#define MAX_NR_CLIENT_CONNECTIONS 2

// Struct sent to the BLE client
// A compact version of uni_hid_device_t.
typedef struct __attribute((packed)) {
    bd_addr_t addr;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t state;
    uint8_t incoming;
    uint16_t controller_type;
    uni_controller_subtype_t controller_subtype;
    char name[HID_NAME_COMPACT_LEN];  // Name, truncated
} compact_device_t;

// client connection
typedef struct {
    bool notification_enabled;
    uint16_t value_handle;
    hci_con_handle_t connection_handle;
} client_connection_t;
static client_connection_t client_connections[MAX_NR_CLIENT_CONNECTIONS];
// round robin sending
static int connection_index;

static compact_device_t compact_devices[CONFIG_BLUEPAD32_MAX_DEVICES];
static bool service_enabled = true;

// clang-format off
static const uint8_t adv_data[] = {
    // Flags general discoverable
    2, BLUETOOTH_DATA_TYPE_FLAGS, APP_AD_FLAGS,
    // Name
    5, BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,'B', 'P', '3', '2',
    // 4627C4A4-AC00-46B9-B688-AFC5C1BF7F63
    17, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_128_BIT_SERVICE_CLASS_UUIDS,
    0x63, 0x7F, 0xBF, 0xC1, 0xC5, 0xAF, 0x88, 0xB6, 0xB9, 0x46, 0x00, 0xAC, 0xA4, 0xC4, 0x27, 0x46,
};
// clang-format on
static int adv_data_len = sizeof(adv_data);

static void att_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size);
static int att_write_callback(hci_con_handle_t con_handle,
                              uint16_t att_handle,
                              uint16_t transaction_mode,
                              uint16_t offset,
                              uint8_t* buffer,
                              uint16_t buffer_size);
static uint16_t att_read_callback(hci_con_handle_t connection_handle,
                                  uint16_t att_handle,
                                  uint16_t offset,
                                  uint8_t* buffer,
                                  uint16_t buffer_size);
static client_connection_t* connection_for_conn_handle(hci_con_handle_t conn_handle);
static void notify_client(void);
static void maybe_notify_client();

static void notify_client(void) {
    logi("**** notify_client\n");
    // find next active streaming connection
    int idx = -1;
    for (int i = connection_index; i < MAX_NR_CLIENT_CONNECTIONS; i++) {
        // active found?
        if ((client_connections[i].connection_handle != HCI_CON_HANDLE_INVALID) &&
            (client_connections[i].notification_enabled)) {
            idx = i;
            break;
        }
    }

    // Already iterated all clients, stop
    if (idx == -1)
        return;

    client_connection_t* ctx = &client_connections[idx];

    // send
    logi("***** notifying client with handle = %#x\n", ctx->connection_handle);
    att_server_notify(ctx->connection_handle, ctx->value_handle, (const uint8_t*)compact_devices,
                      sizeof(compact_devices));

    // Next client
    connection_index++;
    if (connection_index == MAX_NR_CLIENT_CONNECTIONS)
        connection_index = 0;

    // request next send event
    att_server_request_can_send_now_event(ctx->connection_handle);
}

static void maybe_notify_client(void) {
    logi("**** maybe_notify_client\n");
    client_connection_t* ctx = NULL;

    for (int i = 0; i < MAX_NR_CLIENT_CONNECTIONS; i++) {
        if (client_connections[i].connection_handle != HCI_CON_HANDLE_INVALID &&
            client_connections[i].notification_enabled) {
            ctx = &client_connections[i];
            break;
        }
    }
    if (ctx)
        att_server_request_can_send_now_event(ctx->connection_handle);
}

static int att_write_callback(hci_con_handle_t con_handle,
                              uint16_t att_handle,
                              uint16_t transaction_mode,
                              uint16_t offset,
                              uint8_t* buffer,
                              uint16_t buffer_size) {
    ARG_UNUSED(transaction_mode);

    client_connection_t* ctx;

    switch (att_handle) {
        case ATT_CHARACTERISTIC_4627C4A4_AC05_46B9_B688_AFC5C1BF7F63_01_CLIENT_CONFIGURATION_HANDLE: {
            logi("Client configuration for notify\n");
            ctx = connection_for_conn_handle(con_handle);
            if (!ctx)
                break;
            ctx->notification_enabled =
                little_endian_read_16(buffer, 0) == GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION;
            ctx->value_handle = ATT_CHARACTERISTIC_4627C4A4_AC05_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE;
            logi("notification enabled = %d\n", ctx->notification_enabled);
            break;
        }
        case ATT_CHARACTERISTIC_4627C4A4_AC03_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE: {
            // Whether to enable BLE connections
            if (buffer_size != 1 || offset != 0)
                return 0;
            bool enabled = buffer[0];
            uni_bt_le_set_enabled(enabled);
            return 1;
        }
        case ATT_CHARACTERISTIC_4627C4A4_AC04_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE: {
            // Scan for new connections
            if (buffer_size != 1 || offset != 0)
                return 0;
            bool enabled = buffer[0];
            uni_bt_enable_new_connections_unsafe(enabled);
            return 1;
        }
        default:
            logi("Default Write to 0x%04x, len %u\n", att_handle, buffer_size);
            break;
    }
    return 0;
}

static uint16_t att_read_callback(hci_con_handle_t connection_handle,
                                  uint16_t att_handle,
                                  uint16_t offset,
                                  uint8_t* buffer,
                                  uint16_t buffer_size) {
    ARG_UNUSED(connection_handle);

    switch (att_handle) {
        case ATT_CHARACTERISTIC_4627C4A4_AC01_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            // version
            return att_read_callback_handle_blob((const uint8_t*)uni_version, (uint16_t)strlen(uni_version), offset,
                                                 buffer, buffer_size);
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC02_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE: {
            // Max supported connections
            const uint8_t max = CONFIG_BLUEPAD32_MAX_DEVICES;
            return att_read_callback_handle_blob(&max, (uint16_t)1, offset, buffer, buffer_size);
        }
        case ATT_CHARACTERISTIC_4627C4A4_AC03_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE: {
            // Whether to enable BLE connections
            const uint8_t enabled = uni_bt_le_is_enabled();
            return att_read_callback_handle_blob(&enabled, (uint16_t)1, offset, buffer, buffer_size);
        }
        case ATT_CHARACTERISTIC_4627C4A4_AC04_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE: {
            // Scan for new connections
            const uint8_t scanning = uni_bt_enable_new_connections_is_enabled();
            return att_read_callback_handle_blob(&scanning, (uint16_t)1, offset, buffer, buffer_size);
        }
        case ATT_CHARACTERISTIC_4627C4A4_AC05_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            // Connected devices
            return att_read_callback_handle_blob((const void*)compact_devices, (uint16_t)sizeof(compact_devices),
                                                 offset, buffer, buffer_size);
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC06_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC07_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC08_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC09_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC0A_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC0B_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC0C_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_4627C4A4_AC0D_46B9_B688_AFC5C1BF7F63_01_VALUE_HANDLE:
            break;

        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_BATTERY_LEVEL_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_BATTERY_LEVEL_01_CLIENT_CONFIGURATION_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_MANUFACTURER_NAME_STRING_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_MODEL_NUMBER_STRING_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_SERIAL_NUMBER_STRING_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_HARDWARE_REVISION_STRING_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_FIRMWARE_REVISION_STRING_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_SOFTWARE_REVISION_STRING_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_SYSTEM_ID_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_IEEE_11073_20601_REGULATORY_CERTIFICATION_DATA_LIST_01_VALUE_HANDLE:
            break;
        case ATT_CHARACTERISTIC_ORG_BLUETOOTH_CHARACTERISTIC_PNP_ID_01_VALUE_HANDLE:
            break;
        default:
            break;
    }
    return 0;
}

static client_connection_t* connection_for_conn_handle(hci_con_handle_t conn_handle) {
    int i;
    for (i = 0; i < MAX_NR_CLIENT_CONNECTIONS; i++) {
        if (client_connections[i].connection_handle == conn_handle)
            return &client_connections[i];
    }
    return NULL;
}

static void att_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    client_connection_t* ctx;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    switch (hci_event_packet_get_type(packet)) {
        case ATT_EVENT_CONNECTED:
            // setup new
            logi("New device connected\n");
            ctx = connection_for_conn_handle(HCI_CON_HANDLE_INVALID);
            if (!ctx)
                break;
            ctx->connection_handle = att_event_connected_get_handle(packet);
            logi("New device connected handle = %#x\n", ctx->connection_handle);
            break;
        case ATT_EVENT_CAN_SEND_NOW:
            notify_client();
            break;
        case ATT_EVENT_DISCONNECTED:
            ctx = connection_for_conn_handle(att_event_disconnected_get_handle(packet));
            if (!ctx)
                break;
            memset(ctx, 0, sizeof(*ctx));
            ctx->connection_handle = HCI_CON_HANDLE_INVALID;
            break;
        default:
            break;
    }
}

/*
 * Configures the ATT Server with the pre-compiled ATT Database generated from the .gatt file.
 * Finally, it configures the advertisements.
 */
void uni_bt_service_init(void) {
    logi("Starting Bluepad32 BLE service UUID: 4627C4A4-AC00-46B9-B688-AFC5C1BF7F63\n");

    // Setup ATT server.
    att_server_init(profile_data, att_read_callback, att_write_callback);

    // setup advertisements
    uint16_t adv_int_min = 0x0030;
    uint16_t adv_int_max = 0x0030;
    uint8_t adv_type = 0;
    bd_addr_t null_addr;

    memset(null_addr, 0, 6);
    memset(compact_devices, 0, sizeof(compact_devices));
    memset(&client_connections, 0, sizeof(client_connections));
    for (int i = 0; i < MAX_NR_CLIENT_CONNECTIONS; i++)
        client_connections[i].connection_handle = HCI_CON_HANDLE_INVALID;

    // register for ATT events
    att_server_register_packet_handler(att_packet_handler);

    gap_advertisements_set_params(adv_int_min, adv_int_max, adv_type, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(adv_data_len, (uint8_t*)adv_data);
    gap_advertisements_enable(true);
}

bool uni_bt_service_is_enabled() {
    return service_enabled;
}
void uni_bt_service_set_enabled(bool enabled) {
    service_enabled = enabled;
}

void uni_bt_service_on_device_ready(const uni_hid_device_t* d) {
    logi("**** device_ready\n");
    // Must be called from BTstack task
    if (!d)
        return;
    if (!service_enabled)
        return;

    int idx = uni_hid_device_get_idx_for_instance(d);
    if (idx < 0)
        return;

    // Update the things that could have changed from "on_device_connected" callback.
    compact_devices[idx].controller_subtype = d->controller_subtype;
    compact_devices[idx].state = d->conn.connected;
    // No need to end the name with 0
    memcpy(compact_devices[idx].name, d->name, HID_NAME_COMPACT_LEN - 1);

    maybe_notify_client();
}

void uni_bt_service_on_device_connected(const uni_hid_device_t* d) {
    logi("**** device_connected\n");
    // Must be called from BTstack task
    if (!d)
        return;
    if (!service_enabled)
        return;

    int idx = uni_hid_device_get_idx_for_instance(d);
    if (idx < 0)
        return;
    compact_devices[idx].vendor_id = d->vendor_id;
    compact_devices[idx].product_id = d->product_id;
    compact_devices[idx].controller_type = d->controller_type;
    compact_devices[idx].controller_subtype = d->controller_subtype;
    memcpy(compact_devices[idx].addr, d->conn.btaddr, 6);
    compact_devices[idx].state = d->conn.state;
    compact_devices[idx].incoming = d->conn.incoming;
    // No need to end the name with 0
    memcpy(compact_devices[idx].name, d->name, HID_NAME_COMPACT_LEN - 1);

    maybe_notify_client();
}

void uni_bt_service_on_device_disconnected(const uni_hid_device_t* d) {
    logi("**** device_disconnected\n");
    // Must be called from BTstack task
    if (!d)
        return;
    if (!service_enabled)
        return;

    int idx = uni_hid_device_get_idx_for_instance(d);
    if (idx < 0)
        return;
    memset(&compact_devices[idx], 0, sizeof(compact_device_t));

    maybe_notify_client();
}
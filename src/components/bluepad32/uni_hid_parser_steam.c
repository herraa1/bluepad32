/****************************************************************************
http://retro.moe/unijoysticle2

Copyright 2023 Ricardo Quesada

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
****************************************************************************/

// Info from:
// https://github.com/rodrigorc/steamctrl/blob/master/src/steamctrl.c
// https://elixir.bootlin.com/linux/latest/source/drivers/hid/hid-steam.c
// https://github.com/haxpor/sdl2-samples/blob/master/android-project/app/src/main/java/org/libsdl/app/HIDDeviceBLESteamController.java

#include "uni_hid_parser_steam.h"

#include "hid_usage.h"
#include "uni_common.h"
#include "uni_controller.h"
#include "uni_hid_device.h"
#include "uni_hid_parser.h"
#include "uni_log.h"

typedef enum {
    STATE_QUERY_SERVICE,
    STATE_QUERY_CHARACTERISTIC_REPORT,
    STATE_QUERY_CLEAR_MAPPINGS,
    STATE_QUERY_DISABLE_LIZARD,
    STATE_QUERY_FORCEFEEDBACK,
    STATE_QUERY_END,
} steam_query_state_t;

// "100F6C32-1735-4313-B402-38567131E5F3"
static uint8_t le_steam_service_uuid[16] = {0x10, 0x0f, 0x6c, 0x32, 0x17, 0x35, 0x43, 0x13,
                                            0xb4, 0x02, 0x38, 0x56, 0x71, 0x31, 0xe5, 0xf3};

// "100F6C34-1735-4313-B402-38567131E5F3"
static uint8_t le_steam_characteristic_report_uuid[16] = {0x10, 0x0f, 0x6c, 0x34, 0x17, 0x35, 0x43, 0x13,
                                                          0xb4, 0x02, 0x38, 0x56, 0x71, 0x31, 0xe5, 0xf3};

// Commands that can be sent in a feature report.
#define STEAM_CMD_SET_MAPPINGS 0x80
#define STEAM_CMD_CLEAR_MAPPINGS 0x81
#define STEAM_CMD_GET_MAPPINGS 0x82
#define STEAM_CMD_GET_ATTRIB 0x83
#define STEAM_CMD_GET_ATTRIB_LABEL 0x84
#define STEAM_CMD_DEFAULT_MAPPINGS 0x85
#define STEAM_CMD_FACTORY_RESET 0x86
#define STEAM_CMD_WRITE_REGISTER 0x87
#define STEAM_CMD_CLEAR_REGISTER 0x88
#define STEAM_CMD_READ_REGISTER 0x89
#define STEAM_CMD_GET_REGISTER_LABEL 0x8a
#define STEAM_CMD_GET_REGISTER_MAX 0x8b
#define STEAM_CMD_GET_REGISTER_DEFAULT 0x8c
#define STEAM_CMD_SET_MODE 0x8d
#define STEAM_CMD_DEFAULT_MOUSE 0x8e
#define STEAM_CMD_FORCEFEEDBAK 0x8f
#define STEAM_CMD_REQUEST_COMM_STATUS 0xb4
#define STEAM_CMD_GET_SERIAL 0xae
#define STEAM_CMD_HAPTIC_RUMBLE 0xeb

// Some useful register ids
#define STEAM_REG_LPAD_MODE 0x07
#define STEAM_REG_RPAD_MODE 0x08
#define STEAM_REG_RPAD_MARGIN 0x18
#define STEAM_REG_LED 0x2d
#define STEAM_REG_GYRO_MODE 0x30
#define STEAM_REG_LPAD_CLICK_PRESSURE 0x34
#define STEAM_REG_RPAD_CLICK_PRESSURE 0x35

static uint8_t cmd_clear_mappings[] = {
    0xc0, STEAM_CMD_CLEAR_MAPPINGS,  // Command
    0x01                             // Command Len
};

// clang-format off
static uint8_t cmd_disable_lizard[] = {
    0xc0, STEAM_CMD_WRITE_REGISTER,    // Command
    0x0f,                              // Command Len
    STEAM_REG_GYRO_MODE,   0x00, 0x00, // Disable gyro/accel
    STEAM_REG_LPAD_MODE,   0x07, 0x00, // Disable cursor
    STEAM_REG_RPAD_MODE,   0x07, 0x00, // Disable mouse
    STEAM_REG_RPAD_MARGIN, 0x00, 0x00, // No margin
    STEAM_REG_LED,         0x64, 0x00  // LED bright, max value
};
// clang-format on

static gatt_client_service_t le_steam_service;
static gatt_client_characteristic_t le_steam_characteristic_report;
static hci_con_handle_t connection_handle;
static uni_hid_device_t* device;
static steam_query_state_t query_state;

// TODO: Make it easier for "parsers" to write/read/get notified from characteristics
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t* packet, uint16_t size) {
    uint8_t att_status;

    if (packet_type != HCI_EVENT_PACKET)
        return;

    uint8_t event = hci_event_packet_get_type(packet);
    switch (query_state) {
        case STATE_QUERY_SERVICE:
            switch (event) {
                case GATT_EVENT_SERVICE_QUERY_RESULT:
                    logi("gatt_event_service_query_result\n");
                    // store service (we expect only one)
                    gatt_event_service_query_result_get_service(packet, &le_steam_service);
                    break;
                case GATT_EVENT_QUERY_COMPLETE:
                    logi("gatt_event_query_complete\n");
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        loge("SERVICE_QUERY_RESULT - Error status %x.\n", att_status);
                        // Should disconnect (?)
                        // gap_disconnect(connection_handle);
                        break;
                    }
                    // service query complete, look for characteristic report
                    logi("Search for LE Steam characteristic report.\n");
                    query_state = STATE_QUERY_CHARACTERISTIC_REPORT;
                    gatt_client_discover_characteristics_for_service_by_uuid128(handle_gatt_client_event,
                                                                                connection_handle, &le_steam_service,
                                                                                le_steam_characteristic_report_uuid);
                    break;
                default:
                    loge("Steam: Unknown event: %#x\n", event);
            }
            break;
        case STATE_QUERY_CHARACTERISTIC_REPORT:
            switch (event) {
                case GATT_EVENT_QUERY_COMPLETE:
                    logi("gatt_event_query_complete\n");
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        loge("SERVICE_QUERY_RESULT - Error status %x.\n", att_status);
                        // Should disconnect (?)
                        // gap_disconnect(connection_handle);
                        break;
                    }
                    gatt_client_write_value_of_characteristic(handle_gatt_client_event, connection_handle,
                                                              le_steam_characteristic_report.value_handle,
                                                              sizeof(cmd_clear_mappings), cmd_clear_mappings);
                    query_state = STATE_QUERY_CLEAR_MAPPINGS;
                    break;
                case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT:
                    logi("gatt_event_characteristic_query_result\n");
                    gatt_event_characteristic_query_result_get_characteristic(packet, &le_steam_characteristic_report);
                    break;
                default:
                    loge("Unknown event: %#x\n", event);
            }
            break;
        case STATE_QUERY_CLEAR_MAPPINGS:
            switch (event) {
                case GATT_EVENT_QUERY_COMPLETE:
                    logi("gatt_event_query_complete\n");
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        loge("SERVICE_QUERY_RESULT - Error status %x.\n", att_status);
                        // Should disconnect (?)
                        // gap_disconnect(connection_handle);
                        break;
                    }
                    gatt_client_write_value_of_characteristic(handle_gatt_client_event, connection_handle,
                                                              le_steam_characteristic_report.value_handle,
                                                              sizeof(cmd_disable_lizard), cmd_disable_lizard);
                    query_state = STATE_QUERY_DISABLE_LIZARD;
                    break;
                default:
                    loge("Unknown event: %#x\n", event);
            }
            break;
        case STATE_QUERY_DISABLE_LIZARD:
            switch (event) {
                case GATT_EVENT_QUERY_COMPLETE:
                    logi("gatt_event_query_complete\n");
                    att_status = gatt_event_query_complete_get_att_status(packet);
                    if (att_status != ATT_ERROR_SUCCESS) {
                        loge("SERVICE_QUERY_RESULT - Error status %x.\n", att_status);
                        // Should disconnect (?)
                        // gap_disconnect(connection_handle);
                        break;
                    }
                    uni_hid_device_set_ready_complete(device);
                    query_state = STATE_QUERY_END;
                    break;
                default:
                    loge("Unknown event: %#x\n", event);
            }
            break;
        case STATE_QUERY_END:
            // pass-through
        default:
            loge("Steam: Unknown query state: %#x\n", query_state);
            break;
    }
}

void uni_hid_parser_steam_setup(struct uni_hid_device_s* d) {
    device = d;
    connection_handle = d->conn.handle;
    query_state = STATE_QUERY_SERVICE;
    gatt_client_discover_primary_services_by_uuid128(handle_gatt_client_event, d->conn.handle, le_steam_service_uuid);
}

void uni_hid_parser_steam_init_report(uni_hid_device_t* d) {
    // Reset old state. Each report contains a full-state.
    uni_controller_t* ctl = &d->controller;
    memset(ctl, 0, sizeof(*ctl));
    ctl->klass = UNI_CONTROLLER_CLASS_GAMEPAD;
}

void uni_hid_parser_steam_parse_input_report(struct uni_hid_device_s* d, const uint8_t* report, uint16_t len) {
    printf_hexdump(report, len);
}
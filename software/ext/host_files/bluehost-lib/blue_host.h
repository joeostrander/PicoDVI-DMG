#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_HOST_CONTROLLER_GENERIC = 0,
    BT_HOST_CONTROLLER_PS4 = 2,
    BT_HOST_CONTROLLER_EIGHT_BITDO = 5,
} bt_host_controller_type_t;

typedef enum {
    BT_HOST_START_MODE_NORMAL = 0,
    BT_HOST_START_MODE_PAIRING = 1,
} bt_host_start_mode_t;

typedef struct {
    uint8_t remote_addr[6];
    bt_host_controller_type_t controller_type;
    bt_host_start_mode_t start_mode;
    uint32_t pairing_timeout_ms;
    bool clear_bonding_on_start;
    uint8_t pair_button_gpio;
    bool pair_button_active_low;
    uint32_t pair_button_long_press_ms;
} bt_host_config_t;

typedef struct {
    bt_host_controller_type_t controller_type;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t lt;
    uint8_t rt;
    uint8_t dpad;
    uint8_t buttons_primary;
    uint8_t buttons_secondary;
    uint8_t buttons_tertiary;
} bt_host_gamepad_report_t;

typedef void (*bt_host_report_callback_t)(const bt_host_gamepad_report_t *report);
typedef void (*bt_host_pairing_complete_callback_t)(const uint8_t remote_addr[6]);
typedef void (*bt_host_pairing_reset_callback_t)(void);

void btstack_host_start(
    const bt_host_config_t *config,
    bt_host_report_callback_t report_callback,
    bt_host_pairing_complete_callback_t pairing_callback,
    bt_host_pairing_reset_callback_t pairing_reset_callback);

void btstack_host_start_non_blocking(
    const bt_host_config_t *config,
    bt_host_report_callback_t report_callback,
    bt_host_pairing_complete_callback_t pairing_callback,
    bt_host_pairing_reset_callback_t pairing_reset_callback);

void btstack_host_poll(void);

#ifdef __cplusplus
}
#endif

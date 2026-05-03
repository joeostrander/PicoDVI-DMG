#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack_config.h"
#include "btstack.h"

#include "blue_host.h"
#include "gamepad_parser.h"

#define MAX_ATTRIBUTE_VALUE_SIZE 512
#define LED_BLINK_SLOW_MS 500
#define LED_BLINK_FAST_MS 125
#define PAIRING_INQUIRY_DURATION_UNITS 4
#define PAIR_BUTTON_POLL_MS 50

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_SOLID,
    LED_MODE_BLINK_SLOW,
    LED_MODE_BLINK_FAST,
} led_mode_t;

static bd_addr_t remote_addr;
static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint8_t hid_descriptor_storage[MAX_ATTRIBUTE_VALUE_SIZE];
static bool hid_host_descriptor_available = false;
static uint16_t hid_host_cid = 0;
static bool first_time = false;
static bool remote_addr_valid = false;

static btstack_timer_source_t led_timer;
static btstack_timer_source_t pairing_timer;
static btstack_timer_source_t pair_button_timer;
static bool led_timer_active = false;
static bool pairing_timer_active = false;
static bool pair_button_timer_active = false;
static bool pairing_discovery_active = false;
static bool pairing_connect_in_progress = false;
static bool pair_button_pressed = false;
static bool pair_button_long_press_fired = false;
static uint32_t pair_button_press_start_ms = 0;
static led_mode_t led_mode = LED_MODE_OFF;
static int led_state = 0;

static bt_host_config_t host_config;
static bt_host_report_callback_t report_callback = NULL;
static bt_host_pairing_complete_callback_t pairing_complete_callback = NULL;
static bt_host_pairing_reset_callback_t pairing_reset_callback = NULL;
static bool host_started = false;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void process_hid_event(uint8_t *packet);
static void led_timer_handler(btstack_timer_source_t *ts);
static void pairing_timeout_handler(btstack_timer_source_t *ts);
static void pair_button_timer_handler(btstack_timer_source_t *ts);
static void set_led_mode(led_mode_t mode);
static void try_connect_saved_controller(void);
static void enter_pairing_mode(void);
static void leave_pairing_mode(void);
static void start_pairing_discovery(void);
static void stop_pairing_discovery(void);
static void pairing_try_connect_candidate(const bd_addr_t addr, uint32_t class_of_device);
static bool address_is_unset(const bd_addr_t addr);
static uint8_t dpad_hat_from_axes(uint8_t lx, uint8_t ly);
static bool class_of_device_is_likely_hid(uint32_t class_of_device);
static bool parse_with_generic_mapper(
    const uint8_t *report,
    uint16_t report_len,
    bt_host_gamepad_report_t *parsed_report);
static bool pair_button_is_pressed(void);
static void request_pairing_reset_and_start(void);

static void connected_led(int state) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, state);
}

static bool address_is_unset(const bd_addr_t addr) {
    for (size_t i = 0; i < sizeof(bd_addr_t); ++i) {
        if (addr[i] != 0) {
            return false;
        }
    }

    return true;
}

static bool pair_button_is_pressed(void) {
    int raw_state;

    if (host_config.pair_button_long_press_ms == 0) {
        return false;
    }

    raw_state = gpio_get(host_config.pair_button_gpio);
    if (host_config.pair_button_active_low) {
        return raw_state == 0;
    }

    return raw_state != 0;
}

static uint8_t dpad_hat_from_axes(uint8_t lx, uint8_t ly) {
    bool left = lx < 64;
    bool right = lx > 192;
    bool up = ly < 64;
    bool down = ly > 192;

    if (up && right) {
        return 1;
    }
    if (right && down) {
        return 3;
    }
    if (down && left) {
        return 5;
    }
    if (left && up) {
        return 7;
    }
    if (up) {
        return 0;
    }
    if (right) {
        return 2;
    }
    if (down) {
        return 4;
    }
    if (left) {
        return 6;
    }

    return 8;
}

static bool class_of_device_is_likely_hid(uint32_t class_of_device) {
    uint8_t major_device_class = (uint8_t) ((class_of_device >> 8) & 0x1f);

    // Major class 0x05 = Peripheral, which includes gamepads/joysticks.
    return major_device_class == 0x05;
}

static bool parse_with_generic_mapper(
    const uint8_t *report,
    uint16_t report_len,
    bt_host_gamepad_report_t *parsed_report) {
    uint8_t generic_report[7] = {0};
    const uint8_t *hid_report;
    uint16_t hid_report_len;

    if (report_len < 2 || parsed_report == NULL) {
        return false;
    }

    hid_report = report + 1;
    hid_report_len = report_len - 1;

    if (!first_time) {
        uint8_t ret = generate_HID_report_info(
            hid_descriptor_storage_get_descriptor_data(hid_host_cid),
            hid_descriptor_storage_get_descriptor_len(hid_host_cid));
        if (ret != 0) {
            printf("Error: USB_ProcessHIDReport failed: %d\r\n", ret);
            return false;
        }
        first_time = true;
    }

    parse_report(hid_report, hid_report_len, generic_report);
    parsed_report->lx = generic_report[1];
    parsed_report->ly = generic_report[2];
    parsed_report->rx = generic_report[3];
    parsed_report->ry = generic_report[4];
    parsed_report->dpad = generic_report[5] & 0x0f;
    parsed_report->buttons_primary = generic_report[5];
    parsed_report->buttons_secondary = generic_report[6];

    return true;
}

static uint16_t led_blink_interval_ms(void) {
    switch (led_mode) {
        case LED_MODE_BLINK_SLOW:
            return LED_BLINK_SLOW_MS;
        case LED_MODE_BLINK_FAST:
            return LED_BLINK_FAST_MS;
        default:
            return 0;
    }
}

static void set_led_mode(led_mode_t mode) {
    if (led_mode == mode) {
        return;
    }

    led_mode = mode;
    if (led_timer_active) {
        btstack_run_loop_remove_timer(&led_timer);
        led_timer_active = false;
    }

    switch (led_mode) {
        case LED_MODE_OFF:
            led_state = 0;
            connected_led(0);
            break;

        case LED_MODE_SOLID:
            led_state = 1;
            connected_led(1);
            break;

        case LED_MODE_BLINK_SLOW:
        case LED_MODE_BLINK_FAST: {
            uint16_t interval_ms = led_blink_interval_ms();
            led_state = 0;
            connected_led(0);
            btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
            btstack_run_loop_set_timer(&led_timer, interval_ms);
            btstack_run_loop_add_timer(&led_timer);
            led_timer_active = true;
            break;
        }

        default:
            break;
    }
}

static void led_timer_handler(btstack_timer_source_t *ts) {
    (void) ts;

    if (led_mode != LED_MODE_BLINK_SLOW && led_mode != LED_MODE_BLINK_FAST) {
        led_timer_active = false;
        return;
    }

    led_state = !led_state;
    connected_led(led_state);

    btstack_run_loop_set_timer(&led_timer, led_blink_interval_ms());
    btstack_run_loop_add_timer(&led_timer);
    led_timer_active = true;
}

static void pairing_timeout_handler(btstack_timer_source_t *ts) {
    (void) ts;

    pairing_timer_active = false;
    printf("Pairing timeout\n");
    leave_pairing_mode();

    if (remote_addr_valid) {
        try_connect_saved_controller();
    } else {
        set_led_mode(LED_MODE_OFF);
    }
}

static void request_pairing_reset_and_start(void) {
    printf("Pair button long press detected, clearing pairing data and entering pairing mode\n");

    if (pairing_reset_callback != NULL) {
        pairing_reset_callback();
    }

    memset(remote_addr, 0, sizeof(remote_addr));
    remote_addr_valid = false;
    host_config.clear_bonding_on_start = false;
    gap_delete_all_link_keys();

    leave_pairing_mode();
    enter_pairing_mode();

    if (hid_host_cid != 0) {
        hid_host_disconnect(hid_host_cid);
    }
}

static void pair_button_timer_handler(btstack_timer_source_t *ts) {
    uint32_t now_ms;
    bool pressed;

    (void) ts;

    if (host_config.pair_button_long_press_ms == 0) {
        pair_button_timer_active = false;
        return;
    }

    pressed = pair_button_is_pressed();
    now_ms = to_ms_since_boot(get_absolute_time());

    if (pressed) {
        if (!pair_button_pressed) {
            pair_button_pressed = true;
            pair_button_long_press_fired = false;
            pair_button_press_start_ms = now_ms;
        } else if (!pair_button_long_press_fired &&
                   (now_ms - pair_button_press_start_ms) >= host_config.pair_button_long_press_ms) {
            pair_button_long_press_fired = true;
            request_pairing_reset_and_start();
        }
    } else {
        pair_button_pressed = false;
        pair_button_long_press_fired = false;
    }

    btstack_run_loop_set_timer(&pair_button_timer, PAIR_BUTTON_POLL_MS);
    btstack_run_loop_add_timer(&pair_button_timer);
    pair_button_timer_active = true;
}

static void leave_pairing_mode(void) {
    if (host_config.start_mode != BT_HOST_START_MODE_PAIRING) {
        return;
    }

    host_config.start_mode = BT_HOST_START_MODE_NORMAL;

    if (pairing_timer_active) {
        btstack_run_loop_remove_timer(&pairing_timer);
        pairing_timer_active = false;
    }

    stop_pairing_discovery();
    pairing_connect_in_progress = false;

    gap_discoverable_control(0);
    gap_connectable_control(0);
    printf("Pairing mode stopped\n");
}

static void start_pairing_discovery(void) {
    int status;

    if (pairing_discovery_active || pairing_connect_in_progress || hid_host_cid != 0) {
        return;
    }

    status = gap_inquiry_start(PAIRING_INQUIRY_DURATION_UNITS);
    if (status == ERROR_CODE_SUCCESS) {
        pairing_discovery_active = true;
        printf("Pairing inquiry started\n");
    } else {
        printf("Pairing inquiry start failed, status 0x%02x\n", (uint8_t) status);
    }
}

static void stop_pairing_discovery(void) {
    if (!pairing_discovery_active) {
        return;
    }

    gap_inquiry_stop();
    pairing_discovery_active = false;
}

static void pairing_try_connect_candidate(const bd_addr_t addr, uint32_t class_of_device) {
    uint8_t status;
    bd_addr_t target_addr;

    if (pairing_connect_in_progress || hid_host_cid != 0) {
        return;
    }

    if (!class_of_device_is_likely_hid(class_of_device)) {
        return;
    }

    stop_pairing_discovery();
    set_led_mode(LED_MODE_BLINK_SLOW);

    printf("Pairing candidate found: %s cod=0x%06" PRIX32 "\n", bd_addr_to_str(addr), class_of_device);
    memcpy(target_addr, addr, sizeof(target_addr));
    status = hid_host_connect(target_addr, HID_PROTOCOL_MODE_REPORT, &hid_host_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("Pairing connect failed, status 0x%02x\n", status);
        set_led_mode(LED_MODE_BLINK_FAST);
        start_pairing_discovery();
        return;
    }

    pairing_connect_in_progress = true;
}

static void enter_pairing_mode(void) {
    host_config.start_mode = BT_HOST_START_MODE_PAIRING;
    set_led_mode(LED_MODE_BLINK_FAST);
    gap_discoverable_control(1);
    gap_connectable_control(1);
    printf("Pairing mode active, scanning for discoverable HID controllers\n");
    start_pairing_discovery();

    if (host_config.pairing_timeout_ms > 0) {
        btstack_run_loop_set_timer_handler(&pairing_timer, &pairing_timeout_handler);
        btstack_run_loop_set_timer(&pairing_timer, host_config.pairing_timeout_ms);
        btstack_run_loop_add_timer(&pairing_timer);
        pairing_timer_active = true;
    }
}

static void try_connect_saved_controller(void) {
    uint8_t status;

    if (!remote_addr_valid) {
        printf("No saved controller address to connect\n");
        return;
    }

    set_led_mode(LED_MODE_BLINK_SLOW);
    status = hid_host_connect(remote_addr, HID_PROTOCOL_MODE_REPORT, &hid_host_cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("HID host connect failed, status 0x%02x.\n", status);
    }
}

static void hid_host_setup(void) {
    l2cap_init();
    hid_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));
    hid_host_register_packet_handler(packet_handler);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
}

static void hid_host_handle_interrupt_report(const uint8_t *report, uint16_t report_len) {
    if (report_len < 1 || *report != 0xa1) {
        return;
    }

    bt_host_gamepad_report_t parsed_report = {
        .controller_type = host_config.controller_type,
    };

    switch (host_config.controller_type) {
        case BT_HOST_CONTROLLER_EIGHT_BITDO:
            // 8BitDo Micro in S mode often presents Switch-style reports.
            // Use descriptor-based generic parsing for that mode and keep
            // the direct 8BitDo parsing path for D mode.
            if (report[1] == 0x30 || report_len != 11) {
                if (parse_with_generic_mapper(report, report_len, &parsed_report)) {
                    break;
                }
            }

            if (report_len < 11) {
                return;
            }

            parsed_report.lx = report[3];
            parsed_report.ly = report[4];
            parsed_report.rx = report[5];
            parsed_report.ry = report[6];
            parsed_report.rt = report[7];
            parsed_report.lt = report[8];
            parsed_report.buttons_primary = report[9];
            parsed_report.buttons_secondary = report[10];

            // 8BitDo Micro commonly uses report id 0x3F where d-pad behaves like axis motion.
            // For other report ids, keep hat-byte parsing and fall back to axis decode if needed.
            if (report[1] == 0x3f) {
                parsed_report.dpad = dpad_hat_from_axes(parsed_report.lx, parsed_report.ly);
            } else if ((report[2] & 0x0f) <= 8) {
                parsed_report.dpad = report[2] & 0x0f;
            } else {
                parsed_report.dpad = dpad_hat_from_axes(parsed_report.lx, parsed_report.ly);
            }
            break;

        case BT_HOST_CONTROLLER_PS4:
            if (report_len < 11) {
                return;
            }

            parsed_report.lx = report[2];
            parsed_report.ly = report[3];
            parsed_report.rx = report[4];
            parsed_report.ry = report[5];
            parsed_report.dpad = report[6] & 0x0f;
            parsed_report.buttons_primary = report[6];
            parsed_report.buttons_secondary = report[7];
            parsed_report.buttons_tertiary = report[8];
            parsed_report.lt = report[9];
            parsed_report.rt = report[10];
            break;

        case BT_HOST_CONTROLLER_GENERIC: {
            if (!parse_with_generic_mapper(report, report_len, &parsed_report)) {
                return;
            }
            break;
        }

        default:
            return;
    }

    if (report_callback != NULL) {
        report_callback(&parsed_report);
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    uint8_t event;
    bd_addr_t event_addr;

    (void) channel;
    (void) size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                if (host_config.clear_bonding_on_start) {
                    printf("Clearing stored Bluetooth link keys\n");
                    gap_delete_all_link_keys();
                    host_config.clear_bonding_on_start = false;
                }

                if (!pair_button_timer_active && host_config.pair_button_long_press_ms > 0) {
                    btstack_run_loop_set_timer_handler(&pair_button_timer, &pair_button_timer_handler);
                    btstack_run_loop_set_timer(&pair_button_timer, PAIR_BUTTON_POLL_MS);
                    btstack_run_loop_add_timer(&pair_button_timer);
                    pair_button_timer_active = true;
                }

                if (host_config.start_mode == BT_HOST_START_MODE_PAIRING) {
                    enter_pairing_mode();
                } else {
                    try_connect_saved_controller();
                }
            }
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, event_addr);
            gap_pin_code_response(event_addr, "0000");
            break;

        case GAP_EVENT_INQUIRY_RESULT:
            if (host_config.start_mode == BT_HOST_START_MODE_PAIRING &&
                !pairing_connect_in_progress &&
                hid_host_cid == 0) {
                uint32_t class_of_device = gap_event_inquiry_result_get_class_of_device(packet);
                gap_event_inquiry_result_get_bd_addr(packet, event_addr);
                pairing_try_connect_candidate(event_addr, class_of_device);
            }
            break;

        case GAP_EVENT_INQUIRY_COMPLETE:
            pairing_discovery_active = false;
            if (host_config.start_mode == BT_HOST_START_MODE_PAIRING &&
                !pairing_connect_in_progress &&
                hid_host_cid == 0) {
                start_pairing_discovery();
            }
            break;

        case HCI_EVENT_HID_META:
            process_hid_event(packet);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("Controller disconnected\n");
            pairing_connect_in_progress = false;
            if (host_config.start_mode == BT_HOST_START_MODE_NORMAL && remote_addr_valid) {
                try_connect_saved_controller();
            } else if (host_config.start_mode == BT_HOST_START_MODE_PAIRING) {
                set_led_mode(LED_MODE_BLINK_FAST);
                start_pairing_discovery();
            } else {
                set_led_mode(LED_MODE_OFF);
            }
            break;

        default:
            break;
    }
}

static void process_hid_event(uint8_t *packet) {
    int8_t hid_event = hci_event_hid_meta_get_subevent_code(packet);
    bd_addr_t event_addr;

    switch (hid_event) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            hid_subevent_incoming_connection_get_address(packet, event_addr);
            if (host_config.start_mode == BT_HOST_START_MODE_PAIRING ||
                (remote_addr_valid && memcmp(event_addr, remote_addr, sizeof(remote_addr)) == 0)) {
                hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet), HID_PROTOCOL_MODE_REPORT);
            } else {
                printf("Ignoring incoming connection from %s\n", bd_addr_to_str(event_addr));
            }
            break;

        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);

            hid_subevent_connection_opened_get_bd_addr(packet, event_addr);
            if (status != ERROR_CODE_SUCCESS) {
                printf("Connection failed, status 0x%02x\n", status);
                hid_host_cid = 0;
                pairing_connect_in_progress = false;
                if (host_config.start_mode == BT_HOST_START_MODE_PAIRING) {
                    set_led_mode(LED_MODE_BLINK_FAST);
                    start_pairing_discovery();
                }
                return;
            }

            hid_host_descriptor_available = false;
            pairing_connect_in_progress = false;
            hid_host_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            memcpy(remote_addr, event_addr, sizeof(remote_addr));
            remote_addr_valid = !address_is_unset(remote_addr);
            printf("Connection opened for %s\n", bd_addr_to_str(event_addr));

            if (host_config.start_mode == BT_HOST_START_MODE_PAIRING) {
                leave_pairing_mode();
                if (pairing_complete_callback != NULL) {
                    pairing_complete_callback(remote_addr);
                }
            }

            set_led_mode(LED_MODE_BLINK_SLOW);
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            uint8_t status = hid_subevent_descriptor_available_get_status(packet);

            if (status == ERROR_CODE_SUCCESS) {
                hid_host_descriptor_available = true;
                set_led_mode(LED_MODE_SOLID);
                printf("Controller ready\n");
            }
            break;
        }

        case HID_SUBEVENT_REPORT:
            if (hid_host_descriptor_available) {
                hid_host_handle_interrupt_report(
                    hid_subevent_report_get_report(packet),
                    hid_subevent_report_get_report_len(packet));
            }
            break;

        case HID_SUBEVENT_CONNECTION_CLOSED:
            hid_host_cid = 0;
            hid_host_descriptor_available = false;
            pairing_connect_in_progress = false;
            free_HID_report_info();
            first_time = false;
            if (host_config.start_mode == BT_HOST_START_MODE_PAIRING) {
                set_led_mode(LED_MODE_BLINK_FAST);
                start_pairing_discovery();
            } else {
                set_led_mode(LED_MODE_OFF);
            }
            printf("Connection closed\n");
            break;

        default:
            break;
    }
}

static bool btstack_host_init_common(
    const bt_host_config_t *config,
    bt_host_report_callback_t callback,
    bt_host_pairing_complete_callback_t pairing_callback,
    bt_host_pairing_reset_callback_t reset_callback) {
    if (config == NULL || callback == NULL) {
        printf("Bluetooth host start requires a config and callback\n");
        return false;
    }

    memcpy(host_config.remote_addr, config->remote_addr, sizeof(host_config.remote_addr));
    host_config.controller_type = config->controller_type;
    host_config.start_mode = config->start_mode;
    host_config.pairing_timeout_ms = config->pairing_timeout_ms;
    host_config.clear_bonding_on_start = config->clear_bonding_on_start;
    host_config.pair_button_gpio = config->pair_button_gpio;
    host_config.pair_button_active_low = config->pair_button_active_low;
    host_config.pair_button_long_press_ms = config->pair_button_long_press_ms;
    memcpy(remote_addr, host_config.remote_addr, sizeof(remote_addr));
    remote_addr_valid = !address_is_unset(remote_addr);
    report_callback = callback;
    pairing_complete_callback = pairing_callback;
    pairing_reset_callback = reset_callback;
    set_led_mode(LED_MODE_OFF);

    hid_host_setup();

    if (remote_addr_valid) {
        printf("Saved MAC address -> %s\n", bd_addr_to_str(remote_addr));
    } else {
        printf("Saved MAC address -> <none>\n");
    }
    printf("HID type -> %d\n", host_config.controller_type);
    printf("Start mode -> %s\n", host_config.start_mode == BT_HOST_START_MODE_PAIRING ? "pairing" : "normal");
    printf("Long-press pairing reset -> %s\n", host_config.pair_button_long_press_ms > 0 ? "enabled" : "disabled");

    hci_power_control(HCI_POWER_ON);
    host_started = true;
    return true;
}

void btstack_host_start_non_blocking(
    const bt_host_config_t *config,
    bt_host_report_callback_t callback,
    bt_host_pairing_complete_callback_t pairing_callback,
    bt_host_pairing_reset_callback_t reset_callback) {
    (void)btstack_host_init_common(config, callback, pairing_callback, reset_callback);
}

void btstack_host_poll(void) {
    if (!host_started) {
        return;
    }

    async_context_t *context = cyw43_arch_async_context();
    if (context != NULL) {
        async_context_poll(context);
    }
}

void btstack_host_start(
    const bt_host_config_t *config,
    bt_host_report_callback_t callback,
    bt_host_pairing_complete_callback_t pairing_callback,
    bt_host_pairing_reset_callback_t reset_callback) {
    if (!btstack_host_init_common(config, callback, pairing_callback, reset_callback)) {
        return;
    }

    btstack_run_loop_execute();
}
// Joe Ostrander
// 2023.11.19
// PicoDVI-DMG
//
// This is an early version of what might eventually be integrated into:
// https://github.com/joeostrander/consolized-game-boy

// Thanks to PicoDVI and PicoDVI-N64 for getting me started :)

//TODO:
// Audio over HDMI?
// Get HDMI (DVI) working on real TV?
// Possibly pull in some of the OSD features from PicoDVI-N64
// cleanup unused headers
// Add my version of OSD back in
// 

// REMINDER: Always use cmake with:  -DPICO_COPY_TO_RAM=1

// #pragma GCC optimize("Os")
// #pragma GCC optimize("O2")
#pragma GCC optimize("O3")


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "hardware/vreg.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "dmg.pio.h"
#include "tmds_encode.h"

#include "mario.h"

#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "analog_microphone.h"
#include "emusound.h"

static const int hdmi_n[3] = {4096, 6272, 6144};
static uint16_t  rate = SAMPLE_FREQ;
// #define AUDIO_BUFFER_SIZE   (0x1<<8) // Must be power of 2
audio_sample_t      audio_buffer[AUDIO_BUFFER_SIZE];

#define DEBUG_BUTTON_PRESS   // Illuminate LED on button presses

#define ONBOARD_LED_PIN             25

// GAMEBOY VIDEO INPUT (From level shifter)

#define HSYNC_PIN                   0
#define DATA_1_PIN                  1
#define DATA_0_PIN                  2
#define PIXEL_CLOCK_PIN             3
#define VSYNC_PIN                   4
#define DMG_READING_BUTTONS_PIN     5       // P15
#define DMG_READING_DPAD_PIN        6       // P14
#define DMG_OUTPUT_RIGHT_A_PIN      7       // P10
#define DMG_OUTPUT_UP_SELECT_PIN    8       // P12
#define DMG_OUTPUT_DOWN_START_PIN   9       // P13
#define DMG_OUTPUT_LEFT_B_PIN       10      // P11

// the bit order coincides with the pin order
#define BIT_RIGHT_A                 (1<<0)  // P10
#define BIT_UP_SELECT               (1<<1)  // P12
#define BIT_DOWN_START              (1<<2)  // P13
#define BIT_LEFT_B                  (1<<3)  // P11

// I2C Pins, etc. -- for I2C controller
#define SDA_PIN                     26
#define SCL_PIN                     27
#define I2C_ADDRESS                 0x52
i2c_inst_t* i2cHandle = i2c1;

// at 4x Game area will be 640x576 
#define DMG_PIXELS_X                160
#define DMG_PIXELS_Y                144
#define DMG_PIXEL_COUNT             (DMG_PIXELS_X*DMG_PIXELS_Y)

static uint8_t framebuffer_active[DMG_PIXEL_COUNT] = {0};
static uint8_t framebuffer_previous[DMG_PIXEL_COUNT] = {0};
static uint8_t* pixel_active;
static uint8_t* pixel_old;
static bool frameblending_enabled = true;



typedef enum
{
    BUTTON_A = 0,
    BUTTON_B,
    BUTTON_SELECT,
    BUTTON_START,
    BUTTON_UP,
    BUTTON_DOWN,
    BUTTON_LEFT,
    BUTTON_RIGHT,
    BUTTON_HOME,
    BUTTON_COUNT
} controller_button_t;

typedef enum
{
    BUTTON_STATE_PRESSED = 0,
    BUTTON_STATE_UNPRESSED
} button_state_t;

static PIO pio_dmg = pio1;
static uint state_machine = 0;
static uint pio_out_value = 0;

static uint8_t button_states[BUTTON_COUNT];
static uint8_t button_states_previous[BUTTON_COUNT];

// The frame is basically full width
// - since horizontal is already doubled, I manually repeat each x2 to get 4x scale
// - using DVI_VERTICAL_REPEAT to repeat vertically by 4
#define FRAME_WIDTH 800
#define FRAME_HEIGHT 150    // (x4 via DVI_VERTICAL_REPEAT)

const struct dvi_timing __not_in_flash_func(dvi_timing_800x600p_60hz_280K) = {
	.h_sync_polarity   = false,
	.h_front_porch     = 44,
	.h_sync_width      = 128,
	.h_back_porch      = 88,
	.h_active_pixels   = 800,

	.v_sync_polarity   = false,
	.v_front_porch     = 2,        // increased from 1
	.v_sync_width      = 4,
	.v_back_porch      = 22,
	.v_active_lines    = 600,

	.bit_clk_khz       = 280000     // 400K .... toooo much!
};

#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_800x600p_60hz_280K


// // UART config on the last GPIOs
// #define UART_TX_PIN (28)
// #define UART_RX_PIN (29) /* not available on the pico */
// #define UART_ID     uart0
// #define BAUD_RATE   115200

#define RGB888_TO_RGB332(_r, _g, _b) \
    (                                \
        ((_r) & 0xE0)         |      \
        (((_g) & 0xE0) >>  3) |      \
        (((_b) & 0xC0) >>  6)        \
    )

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

struct dvi_inst dvi0;

uint8_t line_buffer[FRAME_WIDTH/DVI_SYMBOLS_PER_WORD] = {0};

uint8_t colors__dmg_nso[] = {
    RGB888_TO_RGB332(0x8c, 0xad, 0x28), 
    RGB888_TO_RGB332(0x6c, 0x94, 0x21), 
    RGB888_TO_RGB332(0x42, 0x6b, 0x29), 
    RGB888_TO_RGB332(0x21, 0x42, 0x31)
};
uint8_t colors__gbp_nso[] = {
    RGB888_TO_RGB332(0xb5, 0xc6, 0x9c), 
    RGB888_TO_RGB332(0x8d, 0x9c, 0x7b), 
    RGB888_TO_RGB332(0x6c, 0x72, 0x51), 
    RGB888_TO_RGB332(0x30, 0x38, 0x20)
};

//0xFF, 0xB6, 0x6D, 0x00
uint8_t* game_palette = colors__gbp_nso;

uint8_t background_color = RGB888_TO_RGB332(0x00, 0x00, 0xFF);


// configuration
const struct analog_microphone_config mic_config = {
    // GPIO to use for input, must be ADC compatible (GPIO 26 - 28)
    .gpio = 28,

    // bias voltage of microphone in volts
    // .bias_voltage = 1.25,
	// .bias_voltage = 6.6,
	.bias_voltage = 0,


    // sample rate in Hz
    // .sample_rate = 44100,
    .sample_rate = SAMPLE_FREQ,

    // number of samples to buffer
    .sample_buffer_size = AUDIO_BUFFER_SIZE,
};
// variables
int16_t sample_buffer[AUDIO_BUFFER_SIZE];
volatile int samples_read = 0;
static void on_analog_samples_ready(void);
// static void __not_in_flash_func(on_analog_samples_ready)(void);
static long map(long x, long in_min, long in_max, long out_min, long out_max);

static void initialize_gpio(void);
static void initialize_pio_program(void);
static void __no_inline_not_in_flash_func(gpio_callback_VIDEO)(uint gpio, uint32_t events);
// static void read_pixel(void);
static void __not_in_flash_func(read_pixel)(void);
static bool __no_inline_not_in_flash_func(nes_classic_controller)(void);
// static bool nes_classic_controller(void);
static void __no_inline_not_in_flash_func(core1_scanline_callback)(void);

void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    dvi_scanbuf_main_8bpp(&dvi0);

	// uint y = 0;
	// while (1) 
    // {
	// 	uint32_t *scanbuf;
	// 	queue_remove_blocking_u32(&dvi0.q_colour_valid, &scanbuf);
		
    //     //_dvi_prepare_scanline_8bpp(&dvi0, scanbuf);
    //     uint32_t *tmdsbuf;
    //     queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
    //     uint pixwidth = dvi0.timing->h_active_pixels;
    //     uint words_per_channel = pixwidth / DVI_SYMBOLS_PER_WORD;
    //     // Scanline buffers are half-resolution; the functions take the number of *input* pixels as parameter.
    //     tmds_encode_data_channel_8bpp(scanbuf, tmdsbuf + 0 * words_per_channel, pixwidth / 2, DVI_8BPP_BLUE_MSB,  DVI_8BPP_BLUE_LSB );
    //     tmds_encode_data_channel_8bpp(scanbuf, tmdsbuf + 1 * words_per_channel, pixwidth / 2, DVI_8BPP_GREEN_MSB, DVI_8BPP_GREEN_LSB);
    //     tmds_encode_data_channel_8bpp(scanbuf, tmdsbuf + 2 * words_per_channel, pixwidth / 2, DVI_8BPP_RED_MSB,   DVI_8BPP_RED_LSB  );
    //     queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);

	// 	queue_add_blocking_u32(&dvi0.q_colour_free, &scanbuf);
	// 	++y;
	// 	if (y == dvi0.timing->v_active_lines) {
	// 		y = 0;
	// 	}
	// }

    __builtin_unreachable();
}

// void core1_scanline_callback(void)
static void __no_inline_not_in_flash_func(core1_scanline_callback)(void)
{
    // Note first two scanlines are pushed before DVI start
    static uint scanline = 2;
    uint idx = 0;
    uint border_horz = 40;
    uint border_vert = 3;
    static uint frame_idx = 0;
    static uint dmg_line_idx = 0;
    // TODO: calc array start from scanline...
    // scanlines are 0 to 149 cuz frame height 150
    if (scanline < border_vert || scanline >= FRAME_HEIGHT-border_vert)
    {
        for (uint i = 0; i < sizeof(line_buffer); i++)
        {
            line_buffer[idx++] = background_color;
        }
        dmg_line_idx = 0;
    }
    else
    {
        dmg_line_idx = scanline - border_vert;
        for (uint i = 0; i < border_horz; i++)
            line_buffer[idx++] = background_color;

        for (uint i = 0; i < DMG_PIXELS_X; i++)
        {
            frame_idx = dmg_line_idx * DMG_PIXELS_X + i;
            line_buffer[idx++] = game_palette[framebuffer_active[frame_idx]];
            line_buffer[idx++] = game_palette[framebuffer_active[frame_idx]];
            
        }

        for (uint i = 0; i < border_horz; i++)
            line_buffer[idx++] = background_color;
    }

    const uint32_t *bufptr = (uint32_t*)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
    ;
    // scanline = (scanline + 1) % FRAME_HEIGHT;
    if (++scanline >= FRAME_HEIGHT) {
    	scanline = 0;
	}
}

int main(void)
{
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);

    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    // preload an image (160x144) into the framebuffer
    memcpy(framebuffer_active, mario_lut_160x144, sizeof(framebuffer_active));

    // setup_default_uart();
    // stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = (dvi_callback_t*)core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());


        // HDMI Audio related
    int offset = rate == 48000 ? 2 : (rate == 44100) ? 1 : 0;
    int cts = dvi0.timing->bit_clk_khz*hdmi_n[offset]/(rate/100)/128;
    dvi_get_blank_settings(&dvi0)->top    = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, rate, cts, hdmi_n[offset]);
    increase_write_pointer(&dvi0.audio_ring,AUDIO_BUFFER_SIZE -1);

	emu_sndInit(false, false, &dvi0.audio_ring, sample_buffer); // dunno


// ------------------------------------------------------------------------------
 // initialize the analog microphone
    if (analog_microphone_init(&mic_config) < 0) {
        printf("analog microphone initialization failed!\n");
        while (1) { tight_loop_contents(); }
    }

    // set callback that is called when all the samples in the library
    // internal sample buffer are ready for reading
    analog_microphone_set_samples_ready_handler(on_analog_samples_ready);

	    // start capturing data from the analog microphone
    if (analog_microphone_start() < 0) 
	{
        printf("PDM microphone start failed!\n");
        while (1) { tight_loop_contents();  }
    }
	// ------------------------------------------------------------------------------

    uint32_t *bufptr = (uint32_t*)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    //printf("Core 1 start\n");
    multicore_launch_core1(core1_main);

    //printf("Start rendering\n");

    initialize_gpio();
    initialize_pio_program();

    gpio_set_irq_enabled_with_callback(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true, &gpio_callback_VIDEO);



    while (true) 
    {
        // for (uint y = 0; y < FRAME_HEIGHT; ++y) {
		// 	uint16_t *pixbuf;
		// 	queue_remove_blocking(&dvi0.q_colour_free, &pixbuf);
		// 	// // sprite_blit16(pixbuf, (const uint16_t *)testcard_320x240 + (y + frame_ctr / 2) % 240 * FRAME_WIDTH, 320);
		// 	// sprite_fill16(pixbuf, 0x07ff, FRAME_WIDTH);
		// 	// for (int i = 0; i < N_BERRIES; ++i)
		// 	// 	// sprite_asprite16(pixbuf, &berry[i], atrans[i], y, FRAME_WIDTH);
		// 	// 	sprite_sprite16(pixbuf, &berry[i], y, FRAME_WIDTH);
		// 	queue_add_blocking(&dvi0.q_colour_valid, &pixbuf);
		// }
        nes_classic_controller();
        pio_sm_put(pio_dmg, state_machine, pio_out_value);
    }
    __builtin_unreachable();
}

// static void __not_in_flash_func(on_analog_samples_ready)(void)
static void on_analog_samples_ready(void)
{
    // callback from library when all the samples in the library
    // internal sample buffer are ready for reading 

    samples_read = analog_microphone_read(sample_buffer, AUDIO_BUFFER_SIZE);
    
	
}

static long map(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

static void initialize_pio_program(void)
{
    static const uint start_in_pin = DMG_READING_BUTTONS_PIN;
    static const uint start_out_pin = DMG_OUTPUT_RIGHT_A_PIN;

    // Get first free state machine in PIO 0
    state_machine = pio_claim_unused_sm(pio_dmg, true);

    // Add PIO program to PIO instruction memory. SDK will find location and
    // return with the memory offset of the program.
    uint offset = pio_add_program(pio_dmg, &dmg_program);

    // Calculate the PIO clock divider
    // float div = (float)clock_get_hz(clk_sys) / pio_freq;
    float div = (float)2;

    // Initialize the program using the helper function in our .pio file
    dmg_program_init(pio_dmg, state_machine, offset, start_in_pin, start_out_pin, div);

    // Start running our PIO program in the state machine
    pio_sm_set_enabled(pio_dmg, state_machine, true);
}

static void initialize_gpio(void)
{    
    //Onboard LED
    gpio_init(ONBOARD_LED_PIN);
    gpio_set_dir(ONBOARD_LED_PIN, GPIO_OUT);
    gpio_put(ONBOARD_LED_PIN, 0);

    // Gameboy video signal inputs
    gpio_init(VSYNC_PIN);
    gpio_init(PIXEL_CLOCK_PIN);
    gpio_init(DATA_0_PIN);
    gpio_init(DATA_1_PIN);
    gpio_init(HSYNC_PIN);

    //Initialize I2C port at 400 kHz
    i2c_init(i2cHandle, 400 * 1000);

    // Initialize I2C pins
    gpio_set_function(SCL_PIN, GPIO_FUNC_I2C);
    gpio_set_function(SDA_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(SCL_PIN);
    gpio_pull_up(SDA_PIN);
}

static void __no_inline_not_in_flash_func(gpio_callback_VIDEO)(uint gpio, uint32_t events)
{
//                  ┌─────────────────────────────────────────┐     
// VSYNC ───────────┘                                         └───────────────────────
//                    ┌──────┐                                     ┌──────┐
// HSYNC ─────────────┘      └─────────────────────────────────────┘      └───────────
//                      ┌─┐     ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐      ┌─┐     ┌─┐ ┌─┐ ┌
// CLOCK ───────────────┘ └─────┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └──────┘ └─────┘ └─┘ └─┘
//       ─────────────┐   ┌──┐  ┌┐ ┌┐ ┌──┐ ┌┐ ┌┐ ┌───────────────┐   ┌──┐  ┌┐ ┌┐ ┌──┐ 
// DATA 0/1           └───┘  └──┘└─┘└─┘  └─┘└─┘└─┘               └───┘  └──┘└─┘└─┘  └─

    uint16_t x = 0;
    uint16_t y = 0;
    pixel_active = framebuffer_active;
    pixel_old = framebuffer_previous;
    uint8_t state_clk;
    uint8_t state_clk_last;

    if (events & GPIO_IRQ_EDGE_FALL)
        return;
    

    for (y = 0; y < DMG_PIXELS_Y; y++)
    {
        // wait for HSYNC edge to rise & fall
        while (gpio_get(HSYNC_PIN) == 0);
        while (gpio_get(HSYNC_PIN) == 1);
        // Grab first pixel just after HYSNC goes low
        read_pixel();
        
        // Get remaining pixels on each falling edge of the clock
        x = 1;
        state_clk = gpio_get(PIXEL_CLOCK_PIN);
        state_clk_last = state_clk;

        while (x < DMG_PIXELS_X)
        {
            state_clk = gpio_get(PIXEL_CLOCK_PIN);
            if (state_clk == 0 && state_clk_last == 1)
            {
                read_pixel();
                x++;
            }
            state_clk_last = state_clk;
        }
    }
}

// static void read_pixel(void)
static void __not_in_flash_func(read_pixel)(void)
{
    uint8_t new_value = (gpio_get(DATA_0_PIN) << 1) + gpio_get(DATA_1_PIN);
    *pixel_active++ = frameblending_enabled ? (new_value == 0 ? new_value|*pixel_old : new_value) : new_value;
    *pixel_old++ = new_value > 0 ? 2 : 0;   // To brighten up the previous frame
}

// static bool nes_classic_controller(void)
static bool __no_inline_not_in_flash_func(nes_classic_controller)(void)
{
    static uint32_t last_micros = 0;
    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 20000)   // probably longer than it needs to be, NES Classic queries about every 5ms
        return false;

    gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, false);
    
    static bool initialized = false;
    static uint8_t i2c_buffer[16] = {0};

    if (!initialized)
    {
        sleep_ms(2000);

        i2c_buffer[0] = 0xF0;
        i2c_buffer[1] = 0x55;
        (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 2, false);
        sleep_ms(10);

        i2c_buffer[0] = 0xFB;
        i2c_buffer[1] = 0x00;
        (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 2, false);
        sleep_ms(20);

        initialized = true;
    }

    last_micros = current_micros;

    i2c_buffer[0] = 0x00;
    (void)i2c_write_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 1, false);   // false - finished with bus
    sleep_us(300);  // NES Classic uses about 330uS
    int ret = i2c_read_blocking(i2cHandle, I2C_ADDRESS, i2c_buffer, 8, false);
    if (ret < 0)
    {
        last_micros = time_us_32();
        gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true);
        return false;
    }
        
    bool valid = false;
    uint8_t i;
    for (i = 0; i < 8; i++)
    {
        if ((i < 4) && (i2c_buffer[i] != 0xFF))
            valid = true;

        if (valid)
        {
            if (i == 4)
            {
                button_states[BUTTON_START] = (~i2c_buffer[i] & (1<<2)) > 0 ? 0 : 1;
                button_states[BUTTON_SELECT] = (~i2c_buffer[i] & (1<<4)) > 0 ? 0 : 1;
                button_states[BUTTON_DOWN] = (~i2c_buffer[i] & (1<<6)) > 0 ? 0 : 1;
                button_states[BUTTON_RIGHT] = (~i2c_buffer[i] & (1<<7)) > 0 ? 0 : 1;

                button_states[BUTTON_HOME] = (~i2c_buffer[i] & (1<<3)) > 0 ? 0 : 1;
            }
            else if (i == 5)
            {
                button_states[BUTTON_UP] = (~i2c_buffer[i] & (1<<0)) > 0 ? 0 : 1;
                button_states[BUTTON_LEFT] = (~i2c_buffer[i] & (1<<1)) > 0 ? 0 : 1;
                button_states[BUTTON_A] = (~i2c_buffer[i] & (1<<4)) > 0 ? 0 : 1;
                button_states[BUTTON_B] = (~i2c_buffer[i] & (1<<6)) > 0 ? 0 : 1;
            }
        }
    }

    if (!valid )
    {
        initialized = false;
        sleep_ms(1000);
        last_micros = time_us_32();
    }

#ifdef DEBUG_BUTTON_PRESS
    uint8_t buttondown = 0;
    for (i = 0; i < BUTTON_COUNT; i++)
    {
        if (button_states[i] == BUTTON_STATE_PRESSED)
        {
            buttondown = 1;
        }
    }
    gpio_put(ONBOARD_LED_PIN, buttondown);
#endif

    uint8_t pins_dpad = 0;
    uint8_t pins_other = 0;
    if (button_states[BUTTON_A] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_RIGHT_A;

    if (button_states[BUTTON_B] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_LEFT_B;
    
    if (button_states[BUTTON_SELECT] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_UP_SELECT;
    
    if (button_states[BUTTON_START] == BUTTON_STATE_PRESSED)
        pins_other |= BIT_DOWN_START;

    if (button_states[BUTTON_UP] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_UP_SELECT;

    if (button_states[BUTTON_DOWN] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_DOWN_START;
    
    if (button_states[BUTTON_LEFT] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_LEFT_B;
    
    if (button_states[BUTTON_RIGHT] == BUTTON_STATE_PRESSED)
        pins_dpad |= BIT_RIGHT_A;

    uint8_t pio_report = ~((pins_dpad << 4) | (pins_other&0xF));
    pio_out_value = (uint32_t)pio_report;

    gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true);

    return true;
}



// TODO:
// get audio working again
// get controls working again

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
// #include "dmg_simple.pio.h"
#include "tmds_encode.h"
#include "audio_ring.h"  // For get_write_size and get_read_size

#include "mario.h"

#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "analog_microphone.h"
#include "emusound.h"
#include "video_capture.pio.h"  // PIO-based video capture - COMMENTED OUT FOR TESTING

#define ENABLE_AUDIO 0  // Set to 1 to enable audio, 0 to disable all audio code
#define ENABLE_PIO_DMG 0  // Set to 1 to enable DMG PIO controller, 0 to disable
#define ENABLE_VIDEO_CAPTURE 1

static const int hdmi_n[6] = {4096, 6272, 6144, 3136, 4096, 6144};  // 32k, 44.1k, 48k, 22.05k, 16k, 24k
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

// Double buffering: one for capture, one for display
static uint8_t framebuffer_0[DMG_PIXEL_COUNT] = {0};
static uint8_t framebuffer_1[DMG_PIXEL_COUNT] = {0};
static uint8_t framebuffer_previous[DMG_PIXEL_COUNT] = {0};

// Packed DMA buffers - 4 pixels per byte (2 bits each)
#define PACKED_FRAME_SIZE (DMG_PIXEL_COUNT / 4)  // 5760 bytes
static uint8_t packed_buffer_0[PACKED_FRAME_SIZE] = {0};
static uint8_t packed_buffer_1[PACKED_FRAME_SIZE] = {0};

// Pointers - swapped atomically between capture and display
static volatile uint8_t* framebuffer_display = framebuffer_0;  // Core 1 reads this
static uint8_t* framebuffer_capture = framebuffer_1;           // Core 0 writes this

static uint8_t* pixel_active;
static uint8_t* pixel_old;
// Set to false to disable frame blending for better performance
// Trade-off: slightly sharper image vs more CPU time for audio
static bool frameblending_enabled = false;  // Try false for smoother audio

// Flag to signal frame capture needed
static volatile bool frame_capture_needed = false;



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

#if ENABLE_PIO_DMG
static PIO pio_dmg = pio1;
// static PIO pio_dmg_simple = pio1;
static uint state_machine = 0;
static uint pio_out_value = 0;
#endif

// PIO video capture  
// CANNOT use PIO0 - it's used by DVI (SM0, SM1, SM2)
// CANNOT use PIO1 SM0 - it's used by DMG controller
// CANNOT use PIO1 SM1 - it's used by audio timer
// CAN use PIO1 SM2 or SM3 for video capture!
static PIO pio_video = pio1;  // Share PIO1 with DMG controller and audio
static uint video_sm = 3;      // Use SM3 (DMG uses SM0, audio uses SM1)
// static uint video_sm = 2;      // Use SM3 (DMG uses SM0)
// static uint video_sm = 1;
static uint video_offset = 0;

static uint8_t button_states[BUTTON_COUNT];
static uint8_t button_states_previous[BUTTON_COUNT];

// Video capture state variables (used by video_capture.pio functions)
int video_dma_chan = -1;
volatile bool video_frame_ready = false;
volatile uint8_t* video_completed_frame = NULL;

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

// #define VREG_VSEL VREG_VOLTAGE_1_25  // Increased from 1.20V for stable 280 MHz clocks
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
    .bias_voltage = 0,

    // sample rate in Hz - match HDMI audio
    .sample_rate = SAMPLE_FREQ,

    // CRITICAL: Small buffer synchronized with PIO chunk size!
    // ADC fills 64 samples every 2ms, PIO reads 64 samples every 2ms
    // This eliminates the race condition
    .sample_buffer_size = ADC_CHUNK_SIZE,
};
// variables - use double buffering to avoid race conditions
int16_t sample_buffer_a[ADC_CHUNK_SIZE];
int16_t sample_buffer_b[ADC_CHUNK_SIZE];
volatile int16_t* adc_write_buffer = sample_buffer_a;  // ADC writes here
volatile int16_t* adc_read_buffer = sample_buffer_b;   // PIO reads from here

// Fixed buffer that PIO always reads from (ADC copies to this)
int16_t sample_buffer_for_pio[ADC_CHUNK_SIZE];
volatile bool adc_buffer_ready = false;                // Signal from ADC to PIO
volatile int samples_read = 0;
static void on_analog_samples_ready(void);
// static void __not_in_flash_func(on_analog_samples_ready)(void);
static long map(long x, long in_min, long in_max, long out_min, long out_max);

static void initialize_gpio(void);
#if ENABLE_PIO_DMG
static void initialize_pio_program(void);
#endif
static void __no_inline_not_in_flash_func(gpio_callback)(uint gpio, uint32_t events);
static void __no_inline_not_in_flash_func(gpio_callback_VIDEO)(uint gpio, uint32_t events);

static bool __no_inline_not_in_flash_func(nes_classic_controller)(void);

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
            line_buffer[idx++] = game_palette[framebuffer_display[frame_idx]];
            line_buffer[idx++] = game_palette[framebuffer_display[frame_idx]];
            
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
    // Initialize stdio for USB serial debugging
    stdio_init_all();
    sleep_ms(3000);  // Give USB more time to enumerate
    
    // Force flush and try multiple times
    for (int i = 0; i < 5; i++) {
        printf("\n\n=== PicoDVI-DMG Starting (attempt %d) ===\n", i+1);
        stdio_flush();
        sleep_ms(100);
    }
    
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);

    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    // preload an image (160x144) into both framebuffers
    memcpy((void*)framebuffer_display, mario_lut_160x144, DMG_PIXEL_COUNT);
    memcpy(framebuffer_capture, mario_lut_160x144, DMG_PIXEL_COUNT);

    // setup_default_uart();
    // stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);

    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    dvi0.scanline_callback = (dvi_callback_t*)core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    uint32_t *bufptr = (uint32_t*)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);

    // HDMI Audio related
    // Support 16000, 22050, 24000, 32000, 44100, and 48000 Hz sample rates
#if ENABLE_AUDIO
    int offset;
    if (rate == 48000) {
        offset = 2;
    } else if (rate == 44100) {
        offset = 1;
    } else if (rate == 24000) {
        offset = 5;  // 24kHz - well supported
    } else if (rate == 22050) {
        offset = 3;  // Half of 44.1k
    } else if (rate == 16000) {
        offset = 4;  // Half of 32k
    } else {
        offset = 0;  // Default for other rates (32000)
    }
    int cts = dvi0.timing->bit_clk_khz*hdmi_n[offset]/(rate/100)/128;
    dvi_get_blank_settings(&dvi0)->top    = 0;
    dvi_get_blank_settings(&dvi0)->bottom = 0;
    dvi_audio_sample_buffer_set(&dvi0, audio_buffer, AUDIO_BUFFER_SIZE);
    dvi_set_audio_freq(&dvi0, rate, cts, hdmi_n[offset]);
    // Note: dvi_set_audio_freq() automatically calls dvi_enable_data_island()
    
    // Pre-fill buffer to 25% to allow for bursty DVI consumption patterns
    increase_write_pointer(&dvi0.audio_ring, AUDIO_BUFFER_SIZE / 4);
    printf("Audio buffer pre-filled to %d samples (25%%)\n", AUDIO_BUFFER_SIZE / 4);
#endif

    // OPTIMIZED ORDER for audio + video:
    // 1. Start PIO audio timer (500Hz chunk processing)
    // 2. Start Core1 (DVI output starts consuming audio)
    // 3. Initialize ADC microphone
    // 4. Initialize GPIO/PIO for DMG controller
    // 5. Register VSYNC interrupt (video capture begins)

#if ENABLE_AUDIO
    printf("Starting PIO audio timer (500Hz chunk rate)...\n");
	emu_sndInit(false, false, &dvi0.audio_ring, sample_buffer_for_pio);
    printf("Audio system initialized\n");
#endif

    // Always start DVI output on Core 1, regardless of audio
    printf("Starting Core 1 (DVI output)...\n");
    multicore_launch_core1(core1_main);
    printf("Core 1 running - now consuming video!\n");

#if ENABLE_AUDIO
    printf("Initializing analog microphone on GPIO %d at %d Hz...\n", mic_config.gpio, mic_config.sample_rate);
    if (analog_microphone_init(&mic_config) < 0) {
        printf("ERROR: analog microphone initialization failed!\n");
        while (1) { tight_loop_contents(); }
    }
    printf("Analog microphone initialized successfully\n");

    analog_microphone_set_samples_ready_handler(on_analog_samples_ready);
    printf("ADC callback handler registered\n");

    printf("Starting analog microphone DMA capture...\n");
    if (analog_microphone_start() < 0) {
        printf("ERROR: PDM microphone start failed!\n");
        while (1) { tight_loop_contents(); }
    }
    printf("Analog microphone started successfully - DMA should be running\n");
#endif

    printf("Initializing GPIO for DMG video capture...\n");
    initialize_gpio();
#if ENABLE_PIO_DMG
    printf("Initializing PIO programs for DMG controller...\n");
    initialize_pio_program();
#endif

    printf("=== SYSTEM READY - DELAYING BEFORE VIDEO INIT ===\n");
    printf("Waiting 7 seconds...\n");
    stdio_flush();
    
    // Long delay - let everything stabilize
    for (int i = 7; i > 0; i--) {
        printf("%d...\n", i);
        stdio_flush();
        sleep_ms(1000);
    }
    
    printf("\n*** PAST COUNTDOWN ***\n");
    stdio_flush();
    sleep_ms(500);
    
#if ENABLE_VIDEO_CAPTURE
    printf("Skipping video initialization (commented out for testing)\n");
    stdio_flush();
    
    printf("Starting video initialization NOW...\n");
    stdio_flush();
    sleep_ms(100);
    
    printf("Step 1: About to call pio_add_program()...\n");
    stdio_flush();
    sleep_ms(100);
    
    video_offset = pio_add_program(pio_video, &video_capture_program);
    
    printf("  -> Loaded at offset %d\n", video_offset);
    stdio_flush();
    
    printf("Step 2: About to initialize PIO state machine...\n");
    stdio_flush();
    
    video_capture_program_init(pio_video, video_sm, video_offset);
    
    printf("  -> State machine initialized\n");
    stdio_flush();
    
    printf("Step 3: About to initialize DMA...\n");
    stdio_flush();
    
    video_capture_dma_init(pio_video, video_sm, packed_buffer_0, PACKED_FRAME_SIZE);
    
    printf("  -> DMA initialized (packed format: %d bytes)\n", PACKED_FRAME_SIZE);
    stdio_flush();
    
    printf("If you see this, the countdown works fine\n");
    stdio_flush();
#endif

    // Track which packed buffer is being captured
    uint8_t* packed_capture = packed_buffer_0;
    uint8_t* packed_next = packed_buffer_1;
    bool video_capture_active = false;

    printf("Entering main loop...\n");
    
    // Main loop - PIO video + perfect audio
    while (true) 
    {
        static uint32_t loop_counter = 0;
        static uint32_t frames_captured = 0;
#if ENABLE_VIDEO_CAPTURE
        // Start video capture after another 3 seconds in the loop (total ~10 sec from boot)
        if (!video_capture_active && loop_counter > 3000000) {
            printf("\n*** STARTING VIDEO CAPTURE NOW (after %lu loops) ***\n", loop_counter);
            printf("Calling video_capture_start_frame()...\n");
            video_capture_start_frame(pio_video, video_sm, packed_capture, PACKED_FRAME_SIZE);
            printf("Returned from video_capture_start_frame()\n");
            printf("Waiting for VSYNC from Game Boy...\n");
            video_capture_active = true;
        }
        // Check if video frame is ready (non-blocking)
        if (video_capture_active && video_capture_frame_ready()) {
            // Frame complete! Get the packed buffer
            uint8_t* completed_packed = video_capture_get_frame();
            // Debug: print first 16 bytes of packed buffer
            // printf("Packed[0..15]: ");
            // for (int dbg = 0; dbg < 16; dbg++) printf("%02X ", completed_packed[dbg]);
            // printf("\n");
            // Quickly unpack: 4 pixels per byte -> 1 pixel per byte
            uint8_t* dest = framebuffer_capture;
            for (int i = 0; i < PACKED_FRAME_SIZE; i++) {
                uint8_t packed = completed_packed[i];
                dest[3] = (packed >> 0) & 0x03;
                dest[2] = (packed >> 2) & 0x03;
                dest[1] = (packed >> 4) & 0x03;
                dest[0] = (packed >> 6) & 0x03;
                dest += 4;
            }
            // Debug: print first 16 pixels of unpacked buffer
            // printf("Unpacked[0..15]: ");
            // for (int dbg = 0; dbg < 16; dbg++) printf("%02X ", framebuffer_capture[dbg]);
            // printf("\n");
            // Atomic swap: make unpacked frame visible to display core
            framebuffer_display = framebuffer_capture;
            // Ping-pong framebuffers
            framebuffer_capture = (framebuffer_capture == framebuffer_0) ? framebuffer_1 : framebuffer_0;
            // Swap packed buffers for next DMA capture
            uint8_t* temp = packed_capture;
            packed_capture = packed_next;
            packed_next = temp;
            // Start capturing next frame immediately
            video_capture_start_frame(pio_video, video_sm, packed_capture, PACKED_FRAME_SIZE);
            frames_captured++;
        }
#endif
        loop_counter++;
        // Poll controller occasionally
#if ENABLE_PIO_DMG
        if (loop_counter % 100000 == 0) {
            nes_classic_controller();
            // pio_sm_put(pio_dmg_simple, state_machine, pio_out_value);
            pio_sm_put(pio_dmg, state_machine, pio_out_value);
        }
#endif
        
        // Blink LED and print stats
        if (loop_counter % 1000000 == 0) {
            static bool led_state = false;
            led_state = !led_state;
            gpio_put(ONBOARD_LED_PIN, led_state);
#if ENABLE_AUDIO
            int write_size = get_write_size(&dvi0.audio_ring, false);
            int read_size = get_read_size(&dvi0.audio_ring, false);
            int fill_level = AUDIO_BUFFER_SIZE - write_size;
            // if (video_capture_active) {
            //     printf("Frames: %lu | Audio: %d/%d (w=%d r=%d)\n",
            //            frames_captured, fill_level, AUDIO_BUFFER_SIZE, write_size, read_size);
            // } else {
            //     printf("Loop %lu - waiting to start video | Audio: %d/%d\n", 
            //            loop_counter, fill_level, AUDIO_BUFFER_SIZE);
            // }
#endif
        }
    }
    __builtin_unreachable();
}

// static void __not_in_flash_func(on_analog_samples_ready)(void)
static void on_analog_samples_ready(void)
{
    static uint32_t callback_count = 0;
    
    // ADC has filled the write buffer with fresh samples
    // Read them into the current write buffer
    int samples_read = analog_microphone_read((int16_t*)adc_write_buffer, ADC_CHUNK_SIZE);
    
    // Copy the JUST-READ samples (from write buffer) to the fixed buffer that PIO reads from
    // We copy from adc_write_buffer because that's what we just filled above!
    memcpy(sample_buffer_for_pio, (void*)adc_write_buffer, ADC_CHUNK_SIZE * sizeof(int16_t));
    
    // Atomically swap buffers - get ready for next ADC capture
    volatile int16_t* temp = adc_write_buffer;
    adc_write_buffer = adc_read_buffer;
    adc_read_buffer = temp;
    
    // Signal that fresh samples are ready
    adc_buffer_ready = true;
    
    // Debug output every 1000 callbacks (every ~2 seconds at 64-sample chunks)
    if (++callback_count % 1000 == 0) {
        printf("ADC callback #%lu: read %d samples, sample[0]=%d, sample_buffer_for_pio[32]=%d\n", 
               callback_count, samples_read, sample_buffer_for_pio[0], sample_buffer_for_pio[32]);
    }
}

static long map(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

#if ENABLE_PIO_DMG
static void initialize_pio_program(void)
{
    static const uint start_in_pin = DMG_READING_BUTTONS_PIN;
    static const uint start_out_pin = DMG_OUTPUT_RIGHT_A_PIN;

    // Get first free state machine in PIO 0
    // state_machine = pio_claim_unused_sm(pio_dmg_simple, true);
    state_machine = pio_claim_unused_sm(pio_dmg, true);

    // Add PIO program to PIO instruction memory. SDK will find location and
    // return with the memory offset of the program.
    // uint offset = pio_add_program(pio_dmg_simple, &dmg_simple_program);
    uint offset = pio_add_program(pio_dmg, &dmg_program);

    // Calculate the PIO clock divider
    // float div = (float)clock_get_hz(clk_sys) / pio_freq;
    float div = (float)2;

    // Initialize the program using the helper function in our .pio file
    // dmg_simple_program_init(pio_dmg_simple, state_machine, offset, start_in_pin, start_out_pin, div);
    dmg_program_init(pio_dmg, state_machine, offset, start_in_pin, start_out_pin, div);

    // Start running our PIO program in the state machine
    // pio_sm_set_enabled(pio_dmg_simple, state_machine, true);
    pio_sm_set_enabled(pio_dmg, state_machine, true);
}
#endif

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

// VSYNC interrupt handler - OPTIMIZED FOR SPEED
// Captures full 160x144 frame in interrupt context (~16ms @ 280MHz)
// Audio PIO timer (priority 0x00) can still preempt this for smooth audio
// Minor pixel artifacts acceptable - Game Boy stays stable!
static void __no_inline_not_in_flash_func(gpio_callback_VIDEO)(uint gpio, uint32_t events)
{
    // Fast inline pixel read - no function call overhead
    #define read_pixel_fast() ({ \
        while (gpio_get(PIXEL_CLOCK_PIN)); \
        while (!gpio_get(PIXEL_CLOCK_PIN)); \
        ((gpio_get(DATA_1_PIN) << 1) | gpio_get(DATA_0_PIN)); \
    })
    
    // Capture entire 160x144 frame into capture buffer
    // Audio interrupts CAN happen here (causes minor artifacts but keeps audio smooth)
    uint8_t* fb = framebuffer_capture;
    
    for (int y = 0; y < 144; y++) {
        // Wait for HSYNC
        while (!gpio_get(HSYNC_PIN));
        while (gpio_get(HSYNC_PIN));
        
        // Capture 160 pixels for this scanline
        // UNROLLED for speed - process 4 pixels at a time
        for (int x = 0; x < 160; x += 4) {
            fb[y * 160 + x + 0] = read_pixel_fast();
            fb[y * 160 + x + 1] = read_pixel_fast();
            fb[y * 160 + x + 2] = read_pixel_fast();
            fb[y * 160 + x + 3] = read_pixel_fast();
        }
    }
    
    // Atomic swap: make captured frame visible to display core
    framebuffer_display = fb;
    framebuffer_capture = (fb == framebuffer_0) ? framebuffer_1 : framebuffer_0;
    
    #undef read_pixel_fast
}

// static bool nes_classic_controller(void)
static bool __no_inline_not_in_flash_func(nes_classic_controller)(void)
{
    static uint32_t last_micros = 0;
    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 20000)   // probably longer than it needs to be, NES Classic queries about every 5ms
        return false;

    // No longer need to disable VSYNC IRQ since handler is lightweight now
    // gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, false);
    
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
        // gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true);
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

#if ENABLE_PIO_DMG
    uint8_t pio_report = ~((pins_dpad << 4) | (pins_other&0xF));
    pio_out_value = (uint32_t)pio_report;
#endif

    // gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true);

    return true;
}

static void __no_inline_not_in_flash_func(gpio_callback)(uint gpio, uint32_t events)
{
    // Prevent controller input to game if OSD is visible
    if (OSD_is_enabled())
        return;

    if(gpio==DMG_READING_DPAD_PIN)
    {
        if (events & GPIO_IRQ_EDGE_FALL)   // Send DPAD states on low
        {
            gpio_put(DMG_OUTPUT_RIGHT_A_PIN, button_states[BUTTON_RIGHT]);
            gpio_put(DMG_OUTPUT_LEFT_B_PIN, button_states[BUTTON_LEFT]);
            gpio_put(DMG_OUTPUT_UP_SELECT_PIN, button_states[BUTTON_UP]);
            gpio_put(DMG_OUTPUT_DOWN_START_PIN, button_states[BUTTON_DOWN]);
        }

        if (events & GPIO_IRQ_EDGE_RISE)   // Send BUTTON states on low
        {
            // When P15 pin goes high, read cycle is complete, send all high
            if(gpio_get(DMG_READING_BUTTONS_PIN) == 1)
            {
                gpio_put(DMG_OUTPUT_RIGHT_A_PIN, 1);
                gpio_put(DMG_OUTPUT_LEFT_B_PIN, 1);
                gpio_put(DMG_OUTPUT_UP_SELECT_PIN, 1);
                gpio_put(DMG_OUTPUT_DOWN_START_PIN, 1);
            }
        }
    }

    if(gpio==DMG_READING_BUTTONS_PIN)
    {
        if (events & GPIO_IRQ_EDGE_FALL)   // Send BUTTON states on low
        {
            gpio_put(DMG_OUTPUT_RIGHT_A_PIN, button_states[BUTTON_A]);
            gpio_put(DMG_OUTPUT_LEFT_B_PIN, button_states[BUTTON_B]);
            gpio_put(DMG_OUTPUT_UP_SELECT_PIN, button_states[BUTTON_SELECT]);
            gpio_put(DMG_OUTPUT_DOWN_START_PIN, button_states[BUTTON_START]);

            // Prevent in-game reset lockup
            // If A,B,Select and Start are all pressed, release them!
            if ((button_states[BUTTON_A] | button_states[BUTTON_B] | button_states[BUTTON_SELECT]| button_states[BUTTON_START])==0)
            {
                button_states[BUTTON_A] = 1;
                button_states[BUTTON_B] = 1;
                button_states[BUTTON_SELECT] = 1;
                button_states[BUTTON_START] = 1;
            }
        }
    }
}



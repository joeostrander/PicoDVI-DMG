/*

TODO:

    Get HDMI (DVI) working on real TV - works in 640x480
    Possibly pull in some of the OSD features from PicoDVI-N64
    cleanup unused headers
    Add my version of OSD back in

*/

// Joe Ostrander
// 2023.11.19
// PicoDVI-DMG
//
// This is an early version of what might eventually be integrated into:
// https://github.com/joeostrander/consolized-game-boy

// Thanks to PicoDVI and PicoDVI-N64 for getting me started :)

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
#include "dmg_buttons.pio.h"
#include "tmds_encode.h"
#include "audio_ring.h"  // For get_write_size and get_read_size

#include "mario.h"

#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "analog_microphone.h"
#include "emusound.h"

#include "colors.h"

// ===== VIDEO CAPTURE MODE SELECTION =====
// Toggle between two VSYNC synchronization methods:
//   1 = IRQ-based (GPIO interrupt on VSYNC rising edge - precise, no PIO wait)
//   0 = WAIT-based (PIO 'wait' instruction for VSYNC - simpler, may scroll on power-on)
#define USE_VSYNC_IRQ 1  // Change to 0 to test WAIT mode, 1 for IRQ mode
// =========================================

#include "video_capture.pio.h"  // PIO-based video capture
#include "shared_dma_handler.h"

#define ENABLE_AUDIO                1  // Set to 1 to enable audio, 0 to disable all audio code
#define ENABLE_PIO_DMG_BUTTONS      0  // Set to 1 to enable DMG PIO controller, 0 to disable
#define ENABLE_CPU_DMG_BUTTONS      1  // Set to 1 to enable DMG CPU sending of buttons, 0 to disable
#define ENABLE_VIDEO_CAPTURE        1
#define ENABLE_OSD                  0  // Set to 1 to enable OSD code, 0 to disable (TODO)


#if ENABLE_AUDIO
static const int hdmi_n[6] = {4096, 6272, 6144, 3136, 4096, 6144};  // 32k, 44.1k, 48k, 22.05k, 16k, 24k
static uint16_t rate = SAMPLE_FREQ;
// #define AUDIO_BUFFER_SIZE   (0x1<<8) // Must be power of 2
audio_sample_t audio_buffer[AUDIO_BUFFER_SIZE];
#endif

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

// Packed DMA buffers - 4 pixels per byte (2 bits each)
// This is the native format from the Game Boy (2 bits per pixel)
// Used by BOTH 640x480 and 800x600 modes for DMA capture AND display
#define PACKED_FRAME_SIZE (DMG_PIXEL_COUNT / 4)  // 5760 bytes (160×144 ÷ 4)
static uint8_t packed_buffer_0[PACKED_FRAME_SIZE] = {0};
static uint8_t packed_buffer_1[PACKED_FRAME_SIZE] = {0};
static uint8_t packed_buffer_previous[PACKED_FRAME_SIZE] = {0};  // For frame blending

// Both modes now use packed buffers directly!
// TMDS encoder handles palette conversion and horizontal scaling
// - 640x480: 4× scaling with grayscale/color palette
// - 800x600: 5× scaling with RGB888 palette
static volatile uint8_t* packed_display_ptr = NULL;

// Frame blending - blends previous frame with current for sprite overlay effects
static volatile bool frameblending_enabled = false;

// Frame blending lookup tables for ultra-fast processing
// Precomputed lookup table for brightening effect (256 bytes)
// Only store_lut is needed; blend calculation is done inline to save 64KB of RAM
static uint8_t store_lut[256];             // What to store for next frame


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

#if ENABLE_PIO_DMG_BUTTONS
static PIO pio_dmg_buttons = pio0;
static uint dmg_buttons_sm = 3;  // Use SM3 (SM0, SM1, SM2 used by DVI TMDS channels)
static uint pio_buttons_out_value = 0;
#endif

// PIO video capture
// PIO NOTES:
// - Each PIO instance has a 32 instruction limit
// - Pico allows for 4 state machines per PIO instance
// - the 32 instructions are shared among all state machines in that PIO instance
// PIO1:
// - Video capture PIO uses 31 instructions, so we can't have any more PIO programs on PIO1
// PIO0:
// - DVI uses SM0, SM1, SM2, but all 3 use the same program (instruction count = 2)
// - I think we can use SM3 for DMG buttons PIO

static PIO pio_video = pio1;  // Exclusively for video capture
static uint video_sm = 0;
static uint video_offset = 0;

static uint8_t button_states[BUTTON_COUNT];
static uint8_t button_states_previous[BUTTON_COUNT];

#if RESOLUTION_800x600
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

	.bit_clk_khz       = 280000
};

#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_800x600p_60hz_280K
#else   // 640x480
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 160    // (×3 via DVI_VERTICAL_REPEAT = 480 lines)

#define VREG_VSEL VREG_VOLTAGE_1_10  // 252 MHz is comfortable at lower voltage
#define DVI_TIMING dvi_timing_640x480p_60hz
#endif

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

// RGB888 palettes - shared by both 640x480 and 800x600 modes
// Store in flash to save RAM
const uint32_t palette__dmg_nso[4] = {
    0x8cad28,  // GB 0 = White (DMG green lightest)
    0x6c9421,  // GB 1 = Light gray
    0x426b29,  // GB 2 = Dark gray
    0x214231   // GB 3 = Black (DMG green darkest)
};

const uint32_t palette__gbp_nso[4] = {
    0xb5c69c,  // GB 0 = White (GBP lightest)
    0x8d9c7b,  // GB 1 = Light gray
    0x6c7251,  // GB 2 = Dark gray
    0x303820   // GB 3 = Black (GBP darkest)
};

const uint32_t* game_palette_rgb888 = palette__gbp_nso;

static void set_game_palette(int index);

#if RESOLUTION_800x600
// For 2bpp packed mode with DVI_SYMBOLS_PER_WORD=2:
// - Buffer contains packed 2bpp data (4 pixels per byte)
// - 160 pixels = 40 bytes per scanline
// - The TMDS encoder handles 4× horizontal scaling (160→640 pixels) with RGB888 palette
// - Plus 80 pixels of horizontal borders on each side (80+640+80 = 800 pixels total)
uint8_t line_buffer[DMG_PIXELS_X / 4] = {0};  // 40 bytes for 160 pixels packed

#else // 640x480
// For 2bpp packed mode with DVI_SYMBOLS_PER_WORD=2:
// - Buffer contains packed 2bpp data (4 pixels per byte)
// - 160 pixels = 40 bytes per scanline
// - The TMDS encoder handles 4× horizontal scaling (160→640 pixels)
// - No hardware doubling needed - encoder outputs full 640 pixels!
uint8_t line_buffer[DMG_PIXELS_X / 4] = {0};  // 40 bytes for 160 pixels packed

#endif // RESOLUTION

#if ENABLE_AUDIO
// configuration
const struct analog_microphone_config mic_config = {
    // GPIO to use for input, must be ADC compatible (GPIO 26 - 28)
    .gpio = 28,

    // bias voltage of microphone in volts
    .bias_voltage = 0,

    // sample rate in Hz - match HDMI audio
    .sample_rate = SAMPLE_FREQ,

    // ADC fills 64 samples every 2ms, timer reads 64 samples every 2ms
    // This eliminates the race condition
    .sample_buffer_size = ADC_CHUNK_SIZE,
};
// variables - use double buffering to avoid race conditions
int16_t sample_buffer_a[ADC_CHUNK_SIZE];
int16_t sample_buffer_b[ADC_CHUNK_SIZE];
volatile int16_t* adc_write_buffer = sample_buffer_a;  // ADC writes here
volatile int16_t* adc_read_buffer = sample_buffer_b;   // timer reads from here

// Fixed buffer that audio always reads from (ADC copies to this)
int16_t sample_buffer_for_audio[ADC_CHUNK_SIZE];
volatile int samples_read = 0;
static void on_analog_samples_ready(void);
// static void __not_in_flash_func(on_analog_samples_ready)(void);
#endif // ENABLE_AUDIO

static void initialize_gpio(void);
#if ENABLE_PIO_DMG_BUTTONS
static void initialize_dmg_buttons_pio_program(void);
#endif

#if ENABLE_CPU_DMG_BUTTONS
static void __no_inline_not_in_flash_func(gpio_callback)(uint gpio, uint32_t events);
#endif

static bool __no_inline_not_in_flash_func(nes_classic_controller)(void);
static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline);

void core1_main(void)
{
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    dvi_start(&dvi0);
    dvi_scanbuf_main_2bpp_gameboy(&dvi0);

    __builtin_unreachable();
}

static void __no_inline_not_in_flash_func(core1_scanline_callback)(uint scanline)
{
    static uint dmg_line_idx = 0;
    
    // scanlines are 0 to 143 (Game Boy native resolution, scaled 4× vertically to 576)
    
#if DVI_VERTICAL_REPEAT == 4
    // 600 lines / 4 = 150 scanlines
    // 144 rows of pixels
    // 150 - 144 = 6 extra lines
    // divide by 2 to center vertically = 3
    int offset = 3;
#else // DVI_VERTICAL_REPEAT == 3
    // 480 rows of pixels / 3 = 160 scanlines
    // 144 rows of pixels
    // 160 - 144 = 16 extra lines
    // divide by 2 to center vertically = 8
    int offset = 8;
#endif

    // Note:  First two scanlines are pushed before DVI start, so subtract 2 from offset
    offset -= 2;

    if ((scanline < offset) || (scanline >= (DMG_PIXELS_Y+offset)))
    {
        // Beyond game area - fill with black (all bits set = 0xFF for each pixel pair)
        // In 2bpp packed format: 0xFF = all pixels are value 3 (black/darkest color in palette)
        memset(line_buffer, 0xFF, sizeof(line_buffer));
        dmg_line_idx = 0;
    }
    else 
    {
        dmg_line_idx = scanline - offset;
        
        const uint8_t* packed_fb = (const uint8_t*)packed_display_ptr;
        if (packed_fb != NULL) {
            const uint8_t* packed_line = packed_fb + (dmg_line_idx * DMG_PIXELS_X / 4);  // 40 bytes per line            
            // Simply copy the packed 2bpp data directly!
            // No unpacking, no palette lookup - the TMDS encoder handles everything
            // Encoder will apply RGB888 palette and 4× horizontal scaling (160→640 pixels)
            // Plus 80 pixels of blank borders on each side for centering in 800 pixels
            memcpy(line_buffer, packed_line, sizeof(line_buffer));  // Copy 40 bytes
        } else {
            // No frame yet, fill with black
            memset(line_buffer, 0xFF, sizeof(line_buffer));
        }
    }

    const uint32_t *bufptr = (uint32_t*)line_buffer;
    queue_add_blocking_u32(&dvi0.q_colour_valid, &bufptr);
    
    while (queue_try_remove_u32(&dvi0.q_colour_free, &bufptr))
    ;
}

// Initialize frame blending lookup tables for ultra-fast processing
// Called once at startup to precompute all 256×256 byte combinations
// This implements the exact logic from old_code.c:
//   Blend: new_value == 0 ? new_value|*pixel_old : new_value
//   Store: new_value > 0 ? 2 : 0  (brightens ghosts by storing gray)
// Used by BOTH 640x480 and 800x600 modes
static void init_frame_blending_luts(void) {
    // Build the store_lut first (what to save for next frame's ghost)
    // This implements: *pixel_old++ = new_value > 0 ? 2 : 0;
    // For each byte, convert: non-white pixels → gray (2), white → white (0)
    // The "2" (gray) value creates the "brightened" ghost effect
    for (int curr = 0; curr < 256; curr++) {
        uint8_t result = 0;
        for (int pixel = 0; pixel < 4; pixel++) {
            int shift = (3 - pixel) * 2;
            uint8_t p = (curr >> shift) & 0x03;
            // Non-white (1,2,3) becomes gray (2), white (0) stays white (0)
            // This is the "brighten up the previous frame" logic!
            uint8_t store_p = (p > 0) ? 2 : 0;
            result |= (store_p << shift);
        }
        store_lut[curr] = result;
    }
      // Note: blend_lut removed to save 64KB RAM (65,536 bytes)
    // Blending is now calculated inline in the frame processing loop
}

int main(void)
{
    // Initialize button states
    for (int i = 0; i < BUTTON_COUNT; i++)
    {
        button_states[i] = BUTTON_STATE_UNPRESSED;
        button_states_previous[i] = BUTTON_STATE_UNPRESSED;
    }    // Initialize stdio for USB serial debugging
    stdio_init_all();
    sleep_ms(3000);  // Give USB more time to enumerate

    // Initialize frame blending lookup tables (one-time computation)
    // Both modes use packed buffers for capture, so both need LUTs
    init_frame_blending_luts();
    
    // Force flush and try multiple times
    for (int i = 0; i < 5; i++) {
        printf("\n\n=== PicoDVI-DMG Starting (attempt %d) ===\n", i+1);
        stdio_flush();
        sleep_ms(100);
    }
      vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);    
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    // Mario image is now pre-packed in 2bpp format (saves 17KB RAM!)
    // Simply copy the packed data directly to the display buffers
    memcpy(packed_buffer_0, mario_packed_160x144, PACKED_FRAME_SIZE);
    memcpy(packed_buffer_1, packed_buffer_0, PACKED_FRAME_SIZE);

    // Both modes use packed buffer directly (TMDS encoder handles palette and scaling)
    packed_display_ptr = packed_buffer_0;

    // setup_default_uart();
    // stdio_uart_init_full(UART_ID, BAUD_RATE, UART_TX_PIN, UART_RX_PIN);
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;
    //dvi0.scanline_callback = (dvi_callback_t*)core1_scanline_callback;
    dvi0.scanline_callback = core1_scanline_callback;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Set initial palette for both 640x480 and 800x600 modes
    set_game_palette(SCHEME_SGB_4H);

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
    // 1. Start audio timer (500Hz chunk processing)
    // 2. Start Core1 (DVI output starts consuming audio)
    // 3. Initialize ADC microphone
    // 4. Initialize GPIO/PIO for DMG controller
    // 5. Register VSYNC interrupt (video capture begins)

#if ENABLE_AUDIO
    printf("Starting audio timer (500Hz chunk rate)...\n");
	emu_sndInit(false, false, &dvi0.audio_ring, sample_buffer_for_audio);
    printf("Audio system initialized\n");
    
#endif

    // Set up shared DMA IRQ handler for both audio and video on DMA_IRQ_1
    printf("Setting up shared DMA IRQ handler on DMA_IRQ_1...\n");
    SHARED_DMA_Init(DMA_IRQ_1);
    printf("Shared DMA IRQ handler installed\n");

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

    printf("Initializing GPIO...\n");
    initialize_gpio();
#if ENABLE_PIO_DMG_BUTTONS
    printf("Initializing PIO programs for DMG controller...\n");
    initialize_dmg_buttons_pio_program();
#endif

#if ENABLE_CPU_DMG_BUTTONS
    // Set up shared GPIO callback - this handles BOTH DMG buttons AND VSYNC
    gpio_set_irq_enabled_with_callback(DMG_READING_DPAD_PIN, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true, &gpio_callback);
    gpio_set_irq_enabled(DMG_READING_BUTTONS_PIN, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);  // Callback already registered
#endif // ENABLE_CPU_DMG_BUTTONS

#if ENABLE_VIDEO_CAPTURE
#if USE_VSYNC_IRQ
    // IRQ MODE: Enable VSYNC interrupt (GPIO 4) - must be done AFTER callback is registered above
    gpio_set_irq_enabled(VSYNC_PIN, GPIO_IRQ_EDGE_RISE, true);
    printf("Video capture mode: IRQ-based VSYNC synchronization\n");
#else
    printf("Video capture mode: WAIT-based VSYNC synchronization (PIO wait instruction)\n");
#endif
#endif
    
#if ENABLE_VIDEO_CAPTURE
    // Add the appropriate PIO program based on mode
#if USE_VSYNC_IRQ
    video_offset = pio_add_program(pio_video, &video_capture_irq_program);
#else
    video_offset = pio_add_program(pio_video, &video_capture_wait_program);
#endif
    
    video_capture_program_init(pio_video, video_sm, video_offset);
    
    // Enable DMA interrupt - use IRQ1 (IRQ0 is used by DVI on Core 1)    
    int video_dma_chan = video_capture_dma_init(pio_video, video_sm, DMA_IRQ_1, packed_buffer_0, PACKED_FRAME_SIZE);
    if (video_dma_chan < 0)
    {
        printf("ERROR: Video capture DMA initialization failed!\n");
        while (1) { tight_loop_contents(); }
    }
    if (!SHARED_DMA_RegisterCallback(video_dma_chan, video_capture_dma_irq_handler)) 
    {
        printf("ERROR: Video capture DMA callback registration failed!\n");
        while (1) { tight_loop_contents(); }
    }
    
    printf("  -> DMA initialized (packed format: %d bytes)\n", PACKED_FRAME_SIZE);    stdio_flush();
#endif

    // Track which packed buffer is being captured (both modes use packed buffers)
    uint8_t* packed_capture = packed_buffer_0;
    uint8_t* packed_next = packed_buffer_1;
    bool video_capture_active = false;
    
    // Main loop - PIO video + good audio
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
            // Frame complete! Get the packed buffer (2bpp DMA format)
            uint8_t* completed_packed = video_capture_get_frame();
              // Apply frame blending if enabled (works on packed 2bpp data)
            if (frameblending_enabled) {
                // Ultra-fast frame blending using precomputed lookup tables
                // Logic from old_code.c:
                //   - Blend: white pixels (0) OR with previous frame
                //   - Store: non-white pixels become gray (2) to "brighten up the previous frame"                // This creates visible ghost trails that fade after one frame
                // Blend calculation done inline to save 64KB RAM (instead of blend_lut)
                for (size_t i = 0; i < PACKED_FRAME_SIZE; i++) {
                    uint8_t current = completed_packed[i];
                    uint8_t previous = packed_buffer_previous[i];
                    
                    // Calculate blended result inline: white (0) pixels OR with previous (ghost effect)
                    uint8_t blended = 0;
                    for (int pixel = 0; pixel < 4; pixel++) {
                        int shift = (3 - pixel) * 2;
                        uint8_t p_curr = (current >> shift) & 0x03;
                        uint8_t p_prev = (previous >> shift) & 0x03;
                        uint8_t p_blend = (p_curr == 0) ? (p_curr | p_prev) : p_curr;
                        blended |= (p_blend << shift);
                    }
                    completed_packed[i] = blended;
                    
                    // Single lookup for brightened ghost (non-white→gray, white→white)
                    // This matches: *pixel_old++ = new_value > 0 ? 2 : 0;
                    packed_buffer_previous[i] = store_lut[current];
                }
            }

            // Both modes use packed buffer directly (TMDS encoder handles palette and scaling)
            // CRITICAL: Use memory barrier and atomic swap to prevent race conditions
            // Ensure all DMA writes are visible before swapping
            __dmb(); // Data Memory Barrier
              // Atomic pointer swap - Core 1 will see this as a single operation
            // No unpacking needed! TMDS encoder handles everything:
            // - 640x480: 4× scaling with grayscale/GBP color palette
            // - 800x600: 4× scaling with RGB888 palette + 80px borders on each side
            packed_display_ptr = (volatile uint8_t*)completed_packed;
            
            // Another barrier to ensure the pointer write is visible
            __dmb();

            // Swap packed buffers for next DMA capture
            uint8_t* temp = packed_capture;
            packed_capture = packed_next;
            packed_next = temp;
            
            // DMA is already finished (that's why we're here), no need to wait
            // Start capturing next frame immediately
            video_capture_start_frame(pio_video, video_sm, packed_capture, PACKED_FRAME_SIZE);
            frames_captured++;
        }
#endif // ENABLE_VIDEO_CAPTURE

        loop_counter++;
        
        // Check for frame blending toggle (SELECT + HOME) - works for both modes
        static button_state_t last_home = BUTTON_STATE_UNPRESSED;
        static button_state_t last_left = BUTTON_STATE_UNPRESSED;
        static button_state_t last_right = BUTTON_STATE_UNPRESSED;
        
        if (button_states[BUTTON_SELECT] == BUTTON_STATE_PRESSED)
        {
            if ((button_states[BUTTON_HOME] == BUTTON_STATE_UNPRESSED) 
                && (last_home == BUTTON_STATE_PRESSED))
            {
                frameblending_enabled = !frameblending_enabled;
                printf("Frame blending: %s\n", frameblending_enabled ? "ENABLED" : "DISABLED");
                if (!frameblending_enabled) {
                    // Clear previous frame buffer when disabling
                    memset(packed_buffer_previous, 0x00, PACKED_FRAME_SIZE);  // 0x00 = all white pixels
                }
            }

            if ((button_states[BUTTON_LEFT] == BUTTON_STATE_UNPRESSED)
                && (last_left == BUTTON_STATE_PRESSED))
            {
                int index = get_scheme_index();
                index--;
                if (index < 0)
                    index = NUMBER_OF_SCHEMES - 1;

                set_game_palette(index);
            }

            if ((button_states[BUTTON_RIGHT] == BUTTON_STATE_UNPRESSED)
                && (last_right == BUTTON_STATE_PRESSED))
            {
                int index = get_scheme_index();
                index++;
                if (index >= NUMBER_OF_SCHEMES)
                    index = 0;

                set_game_palette(index);
            }

        }
        last_home = button_states[BUTTON_HOME];
        last_left = button_states[BUTTON_LEFT];
        last_right = button_states[BUTTON_RIGHT];

        // Poll controller occasionally
#if ENABLE_PIO_DMG_BUTTONS
        (void)nes_classic_controller();
        pio_sm_put(pio_dmg_buttons, dmg_buttons_sm, pio_buttons_out_value);
#endif

#if ENABLE_CPU_DMG_BUTTONS
        nes_classic_controller();
#endif

    }
    __builtin_unreachable();
}

#if ENABLE_AUDIO
// static void __not_in_flash_func(on_analog_samples_ready)(void)
static void on_analog_samples_ready(void)
{
    static uint32_t callback_count = 0;
    
    // ADC has filled the write buffer with fresh samples
    // Read them into the current write buffer
    int samples_read = analog_microphone_read((int16_t*)adc_write_buffer, ADC_CHUNK_SIZE);
    
    // Copy the JUST-READ samples (from write buffer) to the fixed buffer that timed function reads from
    // We copy from adc_write_buffer because that's what we just filled above!
    memcpy(sample_buffer_for_audio, (void*)adc_write_buffer, ADC_CHUNK_SIZE * sizeof(int16_t));
    
    // Atomically swap buffers - get ready for next ADC capture
    volatile int16_t* temp = adc_write_buffer;
    adc_write_buffer = adc_read_buffer;
    adc_read_buffer = temp;
    
    // Debug output every 1000 callbacks (every ~2 seconds at 64-sample chunks)
    if (++callback_count % 1000 == 0) {
        printf("ADC callback #%lu: read %d samples, sample[0]=%d, sample_buffer_for_audio[32]=%d\n", 
               callback_count, samples_read, sample_buffer_for_audio[0], sample_buffer_for_audio[32]);
    }
}
#endif

#if ENABLE_PIO_DMG_BUTTONS
static void initialize_dmg_buttons_pio_program(void)
{
    static const uint start_in_pin = DMG_READING_BUTTONS_PIN;
    static const uint start_out_pin = DMG_OUTPUT_RIGHT_A_PIN;

    // Get first free state machine in PIO 0
    dmg_buttons_sm = pio_claim_unused_sm(pio_dmg_buttons, true);

    // Add PIO program to PIO instruction memory. SDK will find location and
    // return with the memory offset of the program.
    uint offset = pio_add_program(pio_dmg_buttons, &dmg_buttons_program);

    // Calculate the PIO clock divider
    // float div = (float)clock_get_hz(clk_sys) / pio_freq;
    float div = (float)2;

    // Initialize the program using the helper function in our .pio file
    dmg_buttons_program_init(pio_dmg_buttons, dmg_buttons_sm, offset, start_in_pin, start_out_pin, div);

    // Start running our PIO program in the state machine
    pio_sm_set_enabled(pio_dmg_buttons, dmg_buttons_sm, true);
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

#if ENABLE_CPU_DMG_BUTTONS
    gpio_init(DMG_OUTPUT_RIGHT_A_PIN);
    gpio_set_dir(DMG_OUTPUT_RIGHT_A_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_RIGHT_A_PIN, 1);

    gpio_init(DMG_OUTPUT_LEFT_B_PIN);
    gpio_set_dir(DMG_OUTPUT_LEFT_B_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_LEFT_B_PIN, 1);

    gpio_init(DMG_OUTPUT_UP_SELECT_PIN);
    gpio_set_dir(DMG_OUTPUT_UP_SELECT_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_UP_SELECT_PIN, 1);

    gpio_init(DMG_OUTPUT_DOWN_START_PIN);
    gpio_set_dir(DMG_OUTPUT_DOWN_START_PIN, GPIO_OUT);
    gpio_put(DMG_OUTPUT_DOWN_START_PIN, 1);

    gpio_init(DMG_READING_DPAD_PIN);
    gpio_set_dir(DMG_READING_DPAD_PIN, GPIO_IN);

    gpio_init(DMG_READING_BUTTONS_PIN);
    gpio_set_dir(DMG_READING_BUTTONS_PIN, GPIO_IN);
#endif // ENABLE_CPU_DMG_BUTTONS
}

// static bool nes_classic_controller(void)
static bool __no_inline_not_in_flash_func(nes_classic_controller)(void)
{
    static uint32_t last_micros = 0;
    uint32_t current_micros = time_us_32();
    if (current_micros - last_micros < 20000)   // probably longer than it needs to be, NES Classic queries about every 5ms
        return false;
    
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

#if ENABLE_PIO_DMG_BUTTONS
    uint8_t pio_report = ~((pins_dpad << 4) | (pins_other&0xF));
    pio_buttons_out_value = (uint32_t)pio_report;
#endif

    return true;
}

#if ENABLE_CPU_DMG_BUTTONS
static void __no_inline_not_in_flash_func(gpio_callback)(uint gpio, uint32_t events)
{
#if ENABLE_VIDEO_CAPTURE && USE_VSYNC_IRQ
    // Handle VSYNC IRQ for video capture (GPIO 4) - ONLY IN IRQ MODE
    if (gpio == VSYNC_PIN) {
        video_capture_handle_vsync_irq(events);
        return;  // VSYNC handled, done
    }
#endif

    // Prevent controller input to game if OSD is visible
#if ENABLE_OSD
    if (OSD_is_enabled())
        return;
#endif // ENABLE_OSD

    if(gpio==DMG_READING_DPAD_PIN)
    {
        if (events & GPIO_IRQ_EDGE_FALL)   // Send DPAD states on low
        {
            if (gpio_get(DMG_READING_BUTTONS_PIN) == 1)
            {

                gpio_put(DMG_OUTPUT_RIGHT_A_PIN, button_states[BUTTON_RIGHT]);
                gpio_put(DMG_OUTPUT_LEFT_B_PIN, button_states[BUTTON_LEFT]);
                gpio_put(DMG_OUTPUT_UP_SELECT_PIN, button_states[BUTTON_UP]);
                gpio_put(DMG_OUTPUT_DOWN_START_PIN, button_states[BUTTON_DOWN]);
            }
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
#endif // ENABLE_CPU_DMG_BUTTONS

// Palette support for both 640x480 and 800x600 modes
static void set_game_palette(int index)
{
    if ((index <0) || index >= NUMBER_OF_SCHEMES)
        return;

    set_scheme_index(index);
    game_palette_rgb888 = (uint32_t*)get_scheme();

    // Set RGB888 palette pointer for 2bpp palette mode
    // Works for both 640x480 (no borders) and 800x600 (with borders)
    dvi_get_blank_settings(&dvi0)->palette_rgb888 = game_palette_rgb888;
}
/**
 * PIO-based Audio Timer for HDMI Audio
 * 
 * This uses PIO to generate precise timing signals for audio sample processing,
 * eliminating CPU timer jitter that causes choppy audio.
 * 
 * How it works:
 * 1. PIO generates IRQ at exact sample rate (32kHz)
 * 2. IRQ handler copies samples from ADC buffer to HDMI audio ring
 * 3. DMA feeds ADC samples continuously (existing analog_microphone code)
 * 4. No CPU overhead except minimal IRQ handler
 */

#ifndef _PIO_AUDIO_TIMER_H
#define _PIO_AUDIO_TIMER_H

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"

typedef struct {
    PIO pio;
    uint sm;
    uint offset;
    uint irq_index;  // 0-3 for PIO IRQ0-3
} pio_audio_timer_t;

// IRQ handler function pointer
typedef void (*pio_audio_timer_callback_t)(void);

/**
 * Initialize PIO-based audio timer
 * @param timer Pointer to timer struct
 * @param pio Which PIO block to use (pio0 or pio1)
 * @param sample_rate Sample rate in Hz (e.g., 32000)
 * @param callback Function to call on each timer tick
 * @return true on success
 */
bool pio_audio_timer_init(pio_audio_timer_t *timer, PIO pio, uint sample_rate, 
                          pio_audio_timer_callback_t callback);

/**
 * Start the audio timer
 */
void pio_audio_timer_start(pio_audio_timer_t *timer);

/**
 * Stop the audio timer
 */
void pio_audio_timer_stop(pio_audio_timer_t *timer);

#endif // _PIO_AUDIO_TIMER_H

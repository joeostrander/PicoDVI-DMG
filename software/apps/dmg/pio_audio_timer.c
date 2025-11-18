/**
 * PIO-based Audio Timer Implementation
 */

#include "pio_audio_timer.h"
#include "audio_timer.pio.h"
#include "hardware/clocks.h"
#include <stdio.h>

static pio_audio_timer_callback_t g_callback = NULL;
static PIO g_pio = NULL;
static uint g_sm = 0;

// PIO IRQ handler - called at precise intervals by PIO
static void __isr __time_critical_func(pio_audio_timer_irq_handler)(void) {
    // Clear the interrupt - check which SM triggered it
    if (pio_interrupt_get(g_pio, g_sm)) {
        pio_interrupt_clear(g_pio, g_sm);
        
        // Call user callback
        if (g_callback) {
            g_callback();
        }
    }
}

bool pio_audio_timer_init(pio_audio_timer_t *timer, PIO pio, uint sample_rate, 
                          pio_audio_timer_callback_t callback) {
    
    // Claim a state machine
    int sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) {
        printf("No free PIO state machines!\n");
        return false;
    }
    
    timer->pio = pio;
    timer->sm = (uint)sm;
    timer->irq_index = 0; // Use IRQ0 from this PIO
    
    g_callback = callback;
    g_pio = pio;
    g_sm = timer->sm;    // Load the PIO program
    timer->offset = pio_add_program(pio, &audio_timer_program);
    
    // Calculate clock divider for desired sample rate
    // System clock / (cycles_per_sample * sample_rate)
    // Program uses 3 cycles per iteration (irq + nop + nop + wrap=0)
    // 
    // DVI library calculates its own consumption rate based on pixel clock and audio_freq
    // We should produce at EXACTLY the requested sample_rate (32kHz) - no compensation needed!
    // The DVI library's samples_per_line16 will match our production rate automatically.
    
    float sys_clk = (float)clock_get_hz(clk_sys);
    float div = sys_clk / (3.0f * (float)sample_rate);  // Use ACTUAL sample rate!
    
    printf("PIO audio timer: sys_clk=%.0f Hz, sample_rate=%u Hz, div=%.2f\n", 
           sys_clk, sample_rate, div);
    
    // Initialize the state machine
    pio_sm_config c = audio_timer_program_get_default_config(timer->offset);
    sm_config_set_clkdiv(&c, div);    pio_sm_init(pio, timer->sm, timer->offset, &c);
    
    // Enable PIO IRQ - Use IRQ1 to avoid conflict with DMG controller on IRQ0
    // DMG controller uses PIO1_IRQ_0, so audio uses PIO1_IRQ_1
    uint pio_irq = (pio == pio0) ? PIO0_IRQ_1 : PIO1_IRQ_1;  // Use IRQ_1 not IRQ_0!
      printf("PIO audio using %s\n", (pio == pio0) ? "PIO0_IRQ_1" : "PIO1_IRQ_1");
    
    // Set IRQ handler with HIGHEST priority to prevent video capture from blocking audio!
    // Video capture runs in GPIO IRQ and can take milliseconds per frame
    // Audio must run every 2ms (500Hz) with minimal jitter
    irq_set_exclusive_handler(pio_irq, pio_audio_timer_irq_handler);
    irq_set_priority(pio_irq, 0x00);  // Highest priority (0 = highest, 0xFF = lowest)
    irq_set_enabled(pio_irq, true);
    
    printf("PIO audio timer priority set to HIGHEST (preempts video capture)\n");
    
    // Map the PIO program's "irq 0" to PIO's IRQ1 output (not IRQ0)
    // This connects the PIO state machine's IRQ flag to the IRQ1 line
    const uint irq_source_int = pis_interrupt0;  // IRQ 0 from the PIO program
    pio_set_irq1_source_enabled(pio, irq_source_int, true);  // Route to IRQ1 output!
    
    return true;
}

void pio_audio_timer_start(pio_audio_timer_t *timer) {
    pio_sm_set_enabled(timer->pio, timer->sm, true);
}

void pio_audio_timer_stop(pio_audio_timer_t *timer) {
    pio_sm_set_enabled(timer->pio, timer->sm, false);
}

#include "shared_dma_handler.h"
#include "hardware/irq.h"
#include "hardware/dma.h"

#define MAX_DMA_CHANNELS 32
static dma_irq_callback_t dma_callbacks[MAX_DMA_CHANNELS] = { NULL };
static bool initialized = false;
static void __isr shared_dma_irq_handler(void);

void SHARED_DMA_Init(uint irq_num)
{
    irq_set_exclusive_handler(irq_num, shared_dma_irq_handler);
    irq_set_enabled(irq_num, true);
    initialized = true;
}

bool SHARED_DMA_RegisterCallback(int dma_channel, dma_irq_callback_t callback)
{
    if (!initialized)
        return false;
    
    if (dma_channel < 0 || dma_channel >= MAX_DMA_CHANNELS) 
        return false; // Invalid channel

    dma_callbacks[dma_channel] = callback;

    return true;
}

static void __isr shared_dma_irq_handler(void)
{
    uint32_t ints = dma_hw->ints1;

    for (int channel = 0; channel < MAX_DMA_CHANNELS; channel++) 
    {
        if (ints & (1u << channel)) 
        {
            // Clear the interrupt
            dma_hw->ints1 = (1u << channel);

            // Call the registered callback if it exists
            if (dma_callbacks[channel])
            {
                dma_callbacks[channel]();
            }

            break;
        }
    }
}
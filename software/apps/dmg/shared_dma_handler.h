// shared_dma_handler.h
#ifndef SHARED_DMA_HANDLER_H
#define SHARED_DMA_HANDLER_H

#include <stddef.h>
#include "pico/types.h"

typedef void (*dma_irq_callback_t)(void);

void SHARED_DMA_Init(uint irq_num);
bool SHARED_DMA_RegisterCallback(int dma_channel, dma_irq_callback_t callback);

#endif  // SHARED_DMA_HANDLER_H
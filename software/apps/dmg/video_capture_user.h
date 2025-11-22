// video_capture_user.h
#ifndef VIDEO_CAPTURE_USER_H
#define VIDEO_CAPTURE_USER_H

void __isr video_dma_handler(void);
void __isr shared_dma_irq_handler();

#endif
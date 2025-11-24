#ifndef EMUSOUND_H
#define EMUSOUND_H

#include <stdbool.h>

#define SAMPLE_FREQ         32000
// Use power of 2 for efficient modulo operations
// 32000 Hz divides perfectly: 32000/50 = 640, 640/10 = 64 samples per tick
#define NUMSAMPLES          (SAMPLE_FREQ / 50)   // 640 samples per frame at 50Hz @ 32000
#define ZEROSOUND 0                     // Zero point for sound

// ADC buffer size MUST match PIO chunk size for synchronization!
// 64 samples @ 32000 Hz = 2ms (matches PIO callback interval)
// This eliminates the race condition between ADC and PIO
#define ADC_CHUNK_SIZE      (64)        // Must match TICK_SAMPLES
#define AUDIO_BUFFER_SIZE   (2048)      // HDMI ring buffer (separate from ADC)

void emu_sndInit(bool playSound, bool reset, audio_ring_t* audio_ring, int16_t* sample_buff);  // JOE ADDED audio_ring, sample buffer
// void emu_generateSoundSamples(void);
void emu_silenceSound(void);
uint16_t emu_SoundSampleRate(void);



#endif
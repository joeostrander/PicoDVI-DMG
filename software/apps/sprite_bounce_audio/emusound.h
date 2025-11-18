#ifndef EMUSOUND_H
#define EMUSOUND_H

#include <stdbool.h>

#define SAMPLE_FREQ         32000             // JOE ADD...
#define NUMSAMPLES          (SAMPLE_FREQ / 50)   // JOE ADD...
// #define NUMSAMPLES       (SAMPLE_FREQ / 200)   // JOE ADD...
#define ZEROSOUND 0                     // Zero point for sound         // JOE ADDED ... from sound.h
// #define AUDIO_BUFFER_SIZE   (0x1<<8) // Must be power of 2
#define AUDIO_BUFFER_SIZE   (0x1<<13) // Must be power of 2

void emu_sndInit(bool playSound, bool reset, audio_ring_t* audio_ring, int16_t* sample_buff);  // JOE ADDED audio_ring, sample buffer
void emu_generateSoundSamples(void);
void emu_silenceSound(void);
uint16_t emu_SoundSampleRate(void);



#endif
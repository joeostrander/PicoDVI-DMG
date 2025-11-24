#include "pico.h"
#include <stdlib.h>
#include <limits.h>
#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware/irq.h"

#include "hardware/clocks.h"
#include "pico/sync.h"

#include "audio_ring.h"
#include "emusound.h"

// Set to 1 to test with sine wave, 0 for real microphone input
#define TEST_WITH_SINE_WAVE 0

static bool __time_critical_func(audio_timer_callback)(__unused repeating_timer_t* rt);

semaphore_t timer_sem;

// 

// #define SAMPLE_FREQ   32000

// static const uint16_t NUMSAMPLES = (SAMPLE_FREQ / 50); // samples in 50th of second

// static uint16_t soundBuffer16[NUMSAMPLES << 2]; // Effectively two stereo buffers
// static uint16_t* soundBuffer2 = &soundBuffer16[NUMSAMPLES << 1];

static volatile bool first = true;   // True if the first buffer is playing
static bool genSound = false;

static void beginAudio(void);

#define TWOFIVEHZMS   40    // ms between 25 HZ ticks
#define FIFTYHZMS   20    // ms between 50 HZ ticks
#define ONEHUNDRDHZMS   10    // ms between 100 HZ ticks
#define TWOHUNDRDHZMS   5    // ms between 200 HZ ticks
#define FIVEHUNDRDHZMS   2    // ms between 500 HZ ticks
#define TICKMS      2     // ms ticks to service hdmi audio ring buffer
// #define TICKCOUNT   (TWOFIVEHZMS/TICKMS)  // number of ring buffer ticks per 50Hz tick
#define TICKCOUNT   (FIFTYHZMS/TICKMS)  // number of ring buffer ticks per 50Hz tick
// #define TICKCOUNT   (FIVEHUNDRDHZMS/TICKMS)  // number of ring buffer ticks per 200Hz tick
// #define TICKCOUNT   (TWOHUNDRDHZMS/TICKMS)  // number of ring buffer ticks per 200Hz tick
// #define TICKCOUNT   (ONEHUNDRDHZMS/TICKMS)  // number of ring buffer ticks per 100Hz tick
#define TICK_SAMPLES        (NUMSAMPLES / TICKCOUNT)

repeating_timer_t audio_timer;
audio_ring_t* ring;
int16_t* samples; // JOE ADDED
audio_sample_t* hdmi_buffer;
int hdmi_buffer_size;

#ifdef TIME_SPARE
int32_t sound_count = 0;
int64_t int_count = 0;
#endif



uint16_t emu_SoundSampleRate(void)
{
  return SAMPLE_FREQ;
}

void emu_sndInit(bool playSound, bool reset, audio_ring_t* audio_ring, int16_t* sample_buff)  // JOE ADDED audio_ring
{
  genSound = playSound;

  ring = audio_ring;
  samples = sample_buff;

  beginAudio();
}

const int16_t sine[32] = {
    0x8000,0x98f8,0xb0fb,0xc71c,0xda82,0xea6d,0xf641,0xfd89,
    0xffff,0xfd89,0xf641,0xea6d,0xda82,0xc71c,0xb0fb,0x98f8,
    0x8000,0x6707,0x4f04,0x38e3,0x257d,0x1592,0x9be,0x276,
    0x0,0x276,0x9be,0x1592,0x257d,0x38e3,0x4f04,0x6707
};

static bool __time_critical_func(audio_timer_callback)(__unused repeating_timer_t* rt)
{
    static uint32_t call_count = 0;
    static uint32_t debug_counter = 0;
    
    // Process chunk of samples from ADC double-buffer
    // ADC fills one buffer while we read from the other
    // IMPORTANT: Always process exactly TICK_SAMPLES (64), never more!
    // The ADC buffer is only 64 samples, so we can't read beyond that.
    
    int size = get_write_size(ring, true);
    if (size >= TICK_SAMPLES)
    {
        int audio_offset = get_write_offset(ring);
        
        // ALWAYS process exactly 64 samples - no doubling!
        // Our ADC buffer is only 64 samples, reading more would be garbage data
        size = TICK_SAMPLES;
          // DEBUG: Print detailed audio info every 1000 callbacks
        int32_t debug_capture_value = 0;
        // if (++debug_counter % 1000 == 0) {
        //     int32_t adc_0 = (int32_t)((uint16_t)samples[0]);
        //     int32_t adc_32 = (int32_t)((uint16_t)samples[32]);
        //     printf("Audio: ADC[0]=%d, ADC[32]=%d | ", adc_0, adc_32);
        // }
        
        // Process chunk of samples (exactly 64 samples)
        for (int c = 0; c < size; c++)
        {
#if TEST_WITH_SINE_WAVE
            // Test with sine wave for debugging
            int32_t capture_value = sine[c % 32];
#else
            // ADC is 12-bit (0-4095), convert to 16-bit signed (-32768 to 32767)
            int32_t capture_value = ((int32_t)((uint16_t)samples[c]) << 4) - 32768;
            
            if (debug_counter % 1000 == 0 && c == 0) {
                debug_capture_value = capture_value;
            }
#endif
            
            hdmi_buffer[audio_offset].channels[0] = (int16_t)capture_value;
            hdmi_buffer[audio_offset].channels[1] = (int16_t)capture_value;
            audio_offset = (audio_offset + 1) & (hdmi_buffer_size-1);
        }          
        
        // if (debug_counter % 1000 == 0) {
        //     printf("HDMI[0]=%d (original formula, no extra gain)\n", (int16_t)debug_capture_value);
        // }
        set_write_offset(ring, audio_offset);
    }
    
    // 50Hz semaphore for compatibility with existing code
    if (++call_count >= TICKCOUNT)
    {
        call_count = 0;
        first = !first;
        sem_release(&timer_sem);
    }

    return true;
}

static void beginAudio(void)
{
  static bool begun = false;

  if (!begun) // Only begin once...
  {
    begun = true;
    sem_init(&timer_sem, 0, 1);    // Create the timer callback
    emu_silenceSound();    hdmi_buffer = ring->buffer;
    hdmi_buffer_size = ring->size;

    // Use CPU timer for audio chunking
    printf("Initializing CPU timer for audio chunking at %d ms intervals...\n", TICKMS);
    add_repeating_timer_ms(-TICKMS, audio_timer_callback, NULL, &audio_timer);

    printf("sound initialized\n");
  }
  else
  {
    emu_silenceSound();
  }
}

void emu_silenceSound(void)
{
//   if (soundBuffer16)
//   {
    // // Set buffers to silence
    // for (int i = 0; i < (NUMSAMPLES<<2); ++i)
    // {
    //   soundBuffer16[i] = ZEROSOUND;
    // }
//   }
}
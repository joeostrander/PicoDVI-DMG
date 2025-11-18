/*******************************************************************
 Sound
*******************************************************************/
#include "pico.h"
#include <stdlib.h>
#include <limits.h>
#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware/irq.h"

#include "hardware/clocks.h"
#include "pico/sync.h"

#include "audio_ring.h"
// #include "display.h"
// #include "sound.h"
// #include "emuapi.h"
// #include "emupriv.h"
#include "emusound.h"

semaphore_t timer_sem;

// 

// #define SAMPLE_FREQ   32000

// static const uint16_t NUMSAMPLES = (SAMPLE_FREQ / 50); // samples in 50th of second

static uint16_t soundBuffer16[NUMSAMPLES << 2]; // Effectively two stereo buffers
static uint16_t* soundBuffer2 = &soundBuffer16[NUMSAMPLES << 1];

static bool soundCreated = false;
static volatile bool first = true;   // True if the first buffer is playing
static bool genSound = false;

static void beginAudio(void);

#define FIFTYHZMS   20    // ms between 50 HZ ticks
// #define TWOHUNDRDHZMS   5    // ms between 200 HZ ticks
#define TICKMS      2     // ms ticks to service hdmi audio ring buffer
#define TICKCOUNT   (FIFTYHZMS/TICKMS)  // number of ring buffer ticks per 50Hz tick
// #define TICKCOUNT   (TWOHUNDRDHZMS/TICKMS)  // number of ring buffer ticks per 100Hz tick
#define TICK_SAMPLES        (NUMSAMPLES / TICKCOUNT)

struct repeating_timer audio_timer;
audio_ring_t* ring;
int16_t* samples; // JOE ADDED
audio_sample_t* hdmi_buffer;
int hdmi_buffer_size;

#ifdef TIME_SPARE
int32_t sound_count = 0;
int64_t int_count = 0;
#endif

static long map(long x, long in_min, long in_max, long out_min, long out_max);

uint16_t emu_SoundSampleRate(void)
{
  return SAMPLE_FREQ;
}

void emu_sndInit(bool playSound, bool reset, audio_ring_t* audio_ring, int16_t* sample_buff)  // JOE ADDED audio_ring
{
  genSound = playSound;

  ring = audio_ring;
  samples = sample_buff;

//   // This can be called multiple times...
//   if (!soundCreated)
//   {
//     soundCreated = sound_create(SAMPLE_FREQ, NUMSAMPLES);
//   }

//   // Call each time, as sound type may have changed
//   if (soundCreated)
//   {
//     sound_init(emu_ACBRequested(), reset);
//   }

  // Begin sound regardless, as needed for 50 Hz
  beginAudio();
}

// Calls to this function are synchronised to 50Hz through main timer interrupt
void emu_generateSoundSamples(void)
{
//   if (genSound && soundCreated)
//   {
//     sound_frame(first ? soundBuffer2 : soundBuffer16);
// #ifdef TIME_SPARE
//     sound_count++;
// #endif
//   }
}
const int16_t sine[32] = {
    0x8000,0x98f8,0xb0fb,0xc71c,0xda82,0xea6d,0xf641,0xfd89,
    0xffff,0xfd89,0xf641,0xea6d,0xda82,0xc71c,0xb0fb,0x98f8,
    0x8000,0x6707,0x4f04,0x38e3,0x257d,0x1592,0x9be,0x276,
    0x0,0x276,0x9be,0x1592,0x257d,0x38e3,0x4f04,0x6707
};
static bool __not_in_flash_func(audio_timer_callback)(struct repeating_timer *t)
{
  static uint32_t call_count = 0;
  static int cnt = 0;
  static uint sample_count = 0; // JOE AADED

  // write in chunks
  int size = get_write_size(ring, true);
  if (size >= TICK_SAMPLES)
  {
    int audio_offset = get_write_offset(ring);
    if ((size >= ((3*hdmi_buffer_size)>>2)) &&
        (cnt <= ((NUMSAMPLES << 2)-(TICK_SAMPLES << 1))))
    {
      // Allow to refill buffer
      size = (TICK_SAMPLES<<1);
    }
    else
    {
      size = TICK_SAMPLES;
    }

    int32_t capture_value;
    for (int c = 0; c < size; c++)
    {
    //   hdmi_buffer[audio_offset].channels[0] = soundBuffer16[cnt++];
    //   hdmi_buffer[audio_offset].channels[1] = soundBuffer16[cnt++];
        capture_value = samples[sample_count % AUDIO_BUFFER_SIZE];
        capture_value = sine[sample_count % 32];    //TESTING!!!
        capture_value = map(capture_value, 0, 4095, INT16_MIN, INT16_MAX);
        sample_count++;
        
        cnt+=2;
        hdmi_buffer[audio_offset].channels[0] = capture_value;
        hdmi_buffer[audio_offset].channels[1] = capture_value;
        audio_offset = (audio_offset + 1) & (hdmi_buffer_size-1);
    }
    set_write_offset(ring, audio_offset);
//NUMSAMPLES = 32000/50 = 640
// soundBuffer16 = 640 << 2 = 2560

    if (sample_count >= AUDIO_BUFFER_SIZE)
    {
        sample_count -= AUDIO_BUFFER_SIZE;
    }

    if (cnt >= (NUMSAMPLES << 2))
    {
      cnt -= (NUMSAMPLES << 2);
    }
  }

  if (++call_count == TICKCOUNT)
  {
    call_count = 0;

    // resync the play pointer
    if ((!first) & (cnt!=0))
    {
      cnt=0;
    //   sample_count = 0;
    }

    // Swap the buffers and Signal the 50Hz semaphore
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
    sem_init(&timer_sem, 0, 1);

    // Create the timer callback
    emu_silenceSound();
    // getAudioRing(&ring); //JOE
    hdmi_buffer = ring->buffer;
    hdmi_buffer_size = ring->size;
    add_repeating_timer_ms(-TICKMS, audio_timer_callback, NULL, &audio_timer);

    printf("sound initialized\n");
  }
  else
  {
    emu_silenceSound();
  }
}

static long map(long x, long in_min, long in_max, long out_min, long out_max)
{
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}


void emu_silenceSound(void)
{
//   if (soundBuffer16)
//   {
    // Set buffers to silence
    for (int i = 0; i < (NUMSAMPLES<<2); ++i)
    {
      soundBuffer16[i] = ZEROSOUND;
    }
//   }
}
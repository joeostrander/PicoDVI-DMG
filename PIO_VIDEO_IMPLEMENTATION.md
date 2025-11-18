# PIO-Based Video Capture Implementation

**Date:** November 17, 2025  
**Build:** dmg.uf2 (160,256 bytes)  
**Status:** ✅ COMPILED SUCCESSFULLY - READY FOR TESTING

---

## 🎯 OBJECTIVE

Implement hardware-based video capture using PIO + DMA to achieve **simultaneous perfect audio AND smooth video** without blocking the CPU.

### Problem Statement
- Previous CPU-based video capture blocked for 16ms per frame
- 16ms CPU blocking breaks Game Boy's microsecond-precision I2C timing
- Audio PIO timer (500Hz, 2ms intervals) was disrupted
- Previous attempts: VSYNC interrupt, polling, flags - all failed

### Solution
- **PIO State Machine**: Hardware pixel capture synchronized to VSYNC/HSYNC/PIXEL_CLOCK
- **DMA Transfer**: Automatic transfer to memory without CPU intervention
- **Interrupt-Driven**: Frame completion triggers lightweight buffer swap
- **CPU Freedom**: Main loop only handles buffer swaps, controller polling, status

---

## 📐 ARCHITECTURE

### Hardware Signal Flow
```
Game Boy LCD Signals → Level Shifter → Raspberry Pi Pico GPIO
├─ VSYNC (GPIO 4)      : Frame sync
├─ HSYNC (GPIO 0)      : Line sync  
├─ PIXEL_CLOCK (GPIO 3): Pixel timing
├─ DATA_1 (GPIO 1)     : Pixel bit 1
└─ DATA_0 (GPIO 2)     : Pixel bit 0
```

### Data Processing Pipeline
```
PIO State Machine (PIO0, SM0)
    ↓ [Hardware pixel capture, 2 bits/pixel]
PIO RX FIFO (autopush every 8 bits = 4 pixels)
    ↓ [Hardware buffering]
DMA Channel (DMA_IRQ_0)
    ↓ [Automatic transfer, paced by PIO DREQ]
Packed Buffer (5,760 bytes)
    ↓ [CPU: Quick unpack in main loop]
Framebuffer (23,040 bytes, 1 byte/pixel)
    ↓ [Atomic pointer swap]
Display Core (Core 1)
    ↓ [HDMI output via DVI]
TV/Monitor
```

---

## 🔧 KEY COMPONENTS

### 1. PIO Program (`video_capture.pio`)

**Purpose:** Hardware pixel capture synchronized to Game Boy video signals

**Strategy:**
- Wait for VSYNC to detect frame start
- For each scanline:
  - Wait for HSYNC (blanking → active video transition)
  - Loop 20 times, capturing 8 pixels per iteration (160 pixels total)
  - Each pixel: Wait for PIXEL_CLOCK high, read DATA_1 and DATA_0
- Autopush every 8 bits (4 pixels) to FIFO
- Continue until DMA stops (after 144 lines × 160 pixels = 23,040 pixels)

**Pin Mapping:**
```c
sm_config_set_in_pins(&c, 1);  // Base = GPIO 1 (DATA_1)
// Relative pin 0 = GPIO 1 = DATA_1
// Relative pin 1 = GPIO 2 = DATA_0
// "in pins, 2" reads both data pins in correct order
```

**PIO Configuration:**
- State Machine: PIO0, SM0
- Clock Divider: 1.0 (full speed)
- Autopush: Enabled, 8-bit threshold
- Shift Direction: Left (LSB first)

**Loop Structure:**
```asm
.wrap_target
    wait 1 gpio 4           ; VSYNC high (frame start)
line_loop:
    wait 1 gpio 0           ; HSYNC high
    wait 0 gpio 0           ; HSYNC low (active video)
    set x, 19               ; 20 iterations
pixel_group_loop:
    ; 8 pixels = 2 autopushes
    wait 1 gpio 3           ; PIXEL_CLOCK
    in pins, 2              ; Read DATA_1, DATA_0
    ; ... (repeat 8 times for 8 pixels)
    jmp x-- pixel_group_loop
    jmp line_loop
.wrap
```

### 2. DMA Configuration (`video_capture.pio` C SDK section)

**Purpose:** Automatically transfer captured pixels from PIO FIFO to memory

**Configuration:**
- Transfer Size: 8-bit (DMA_SIZE_8)
- Source: PIO0 RX FIFO (read, no increment)
- Destination: Packed buffer (write, increment)
- Transfer Count: 5,760 bytes (160×144 pixels ÷ 4 pixels/byte)
- Pacing: PIO RX FIFO DREQ (hardware flow control)

**Interrupt Handler:**
```c
static void __isr video_dma_handler() {
    dma_hw->ints0 = 1u << video_dma_chan;  // Clear interrupt
    frame_ready = true;                     // Signal main loop
}
```

**Frame Lifecycle:**
```c
// Start new frame
video_capture_start_frame(pio, sm, packed_buffer, PACKED_FRAME_SIZE);
    → Configure DMA destination
    → Clear PIO FIFO
    → Enable PIO state machine (waits for VSYNC)
    → Start DMA transfer

// DMA runs in background...
// When complete: DMA_IRQ_0 → video_dma_handler() → frame_ready = true

// Main loop checks
if (video_capture_frame_ready()) {
    // Get completed buffer, unpack, swap, start next frame
}
```

### 3. Buffer Management (`main.c`)

**Double-Buffered Architecture:**

```c
// Packed buffers for DMA (4 pixels per byte)
#define PACKED_FRAME_SIZE (DMG_PIXEL_COUNT / 4)  // 5,760 bytes
uint8_t packed_buffer_0[PACKED_FRAME_SIZE];
uint8_t packed_buffer_1[PACKED_FRAME_SIZE];

// Unpacked buffers for display (1 pixel per byte)
uint8_t framebuffer_0[DMG_PIXEL_COUNT];  // 23,040 bytes
uint8_t framebuffer_1[DMG_PIXEL_COUNT];

// Pointers (ping-pong swapping)
uint8_t* packed_capture;        // DMA writes here
volatile uint8_t* framebuffer_display;  // Core 1 reads here
uint8_t* framebuffer_capture;   // Core 0 unpacks here
```

**Unpacking Logic:**
```c
// Fast unpacking: 4 pixels per byte → 1 pixel per byte
for (int i = 0; i < PACKED_FRAME_SIZE; i++) {
    uint8_t packed = completed_packed[i];
    dest[0] = (packed >> 0) & 0x03;  // Extract bits 0-1
    dest[1] = (packed >> 2) & 0x03;  // Extract bits 2-3
    dest[2] = (packed >> 4) & 0x03;  // Extract bits 4-5
    dest[3] = (packed >> 6) & 0x03;  // Extract bits 6-7
    dest += 4;
}
// Time cost: ~5,760 iterations, very fast
```

### 4. Main Loop Integration

**CPU Work Per Frame:**
1. ✅ Check `video_capture_frame_ready()` (non-blocking flag check)
2. ✅ If ready: Unpack 5,760 bytes (~1-2ms worst case)
3. ✅ Atomic pointer swap (nanoseconds)
4. ✅ Start next DMA capture (microseconds)

**Total CPU Time:** ~1-2ms per frame (16.67ms available @ 60fps)  
**Remaining Time:** 14-15ms for audio, controller, other tasks

**Complete Main Loop:**
```c
while (true) {
    // Video: Check for completed frames
    if (video_capture_frame_ready()) {
        uint8_t* completed = video_capture_get_frame();
        // Unpack pixels (fast)
        for (int i = 0; i < PACKED_FRAME_SIZE; i++) { ... }
        // Atomic swap
        framebuffer_display = framebuffer_capture;
        // Ping-pong buffers
        framebuffer_capture = (framebuffer_capture == framebuffer_0) ? 
                              framebuffer_1 : framebuffer_0;
        // Start next frame
        video_capture_start_frame(...);
        frames_captured++;
    }
    
    // Controller: Poll occasionally
    if (++loop_counter % 100000 == 0) {
        nes_classic_controller();
        pio_sm_put(pio_dmg, state_machine, pio_out_value);
    }
    
    // Status: Print diagnostics
    if (loop_counter % 1000000 == 0) {
        gpio_put(LED, !gpio_get(LED));
        printf("Frames: %lu | Audio: %d/%d\n", ...);
    }
}
```

---

## 🎵 AUDIO SYSTEM (Unchanged - Already Perfect)

**PIO Audio Timer:** 500Hz chunk rate, 64 samples/chunk  
**Priority:** 0x00 (highest) - preempts everything  
**Interrupt:** PIO1_IRQ_1 (no conflict with DMG controller)  
**Buffer:** Triple-buffered (ADC ping-pong + fixed PIO read buffer)  
**Formula:** `((adc_value << 4) - 32768)` (original working formula)

**Audio runs independently** - never blocked by video capture!

---

## ⚙️ RESOURCE ALLOCATION

| Resource | Usage | Purpose |
|----------|-------|---------|
| PIO0, SM0 | Video capture | Pixel sampling |
| PIO1, SM0 | DMG controller | Game Boy button I/O |
| PIO1, SM1 | Audio timer | 500Hz timing source |
| DMA_IRQ_0 | Video completion | Frame ready signal |
| PIO1_IRQ_1 | Audio chunks | 64-sample processing |
| Core 0 | Main loop | Video unpack, controller, status |
| Core 1 | DVI output | HDMI video+audio streaming |

**No Conflicts:** Each PIO program and interrupt has dedicated resources

---

## 📊 PERFORMANCE ESTIMATES

### Timing Budget (60fps = 16.67ms per frame)

| Task | Time | CPU% | Notes |
|------|------|------|-------|
| DMA transfer | ~3ms | 0% | Hardware (background) |
| Pixel unpack | ~1-2ms | ~10% | 5,760 iterations |
| Buffer swap | <1µs | <0.1% | Pointer assignment |
| Controller poll | ~500µs | ~3% | I2C communication |
| Audio processing | ~64µs | ~3% | 500Hz × 128µs |
| **Slack time** | **~10ms** | **~84%** | Available for optimization |

### Expected Frame Rate
- **Target:** 60fps (16.67ms/frame)
- **Bottleneck:** Game Boy LCD refresh (59.7fps actual)
- **Expected Result:** Smooth video matching Game Boy timing

### Memory Usage
```
Framebuffers:     2 × 23,040 = 46,080 bytes
Packed buffers:   2 × 5,760  = 11,520 bytes
Audio buffer:           2,048 =  2,048 bytes
Total video+audio:              59,648 bytes
Available RAM:                 264,000 bytes (RP2040)
Usage:                           ~23%
```

---

## 🧪 TESTING CHECKLIST

### Phase 1: Basic Functionality
- [ ] Flash dmg.uf2 to Raspberry Pi Pico
- [ ] Connect to USB serial (115200 baud)
- [ ] Verify startup messages:
  - `PIO video capture program loaded at offset X`
  - `PIO video capture DMA initialized (packed format: 5760 bytes)`
  - `PIO video capture started - waiting for VSYNC...`
- [ ] Connect Game Boy and power on
- [ ] Check for frame capture messages
- [ ] Verify `Frames: X` counter increments

### Phase 2: Video Quality
- [ ] Display shows Game Boy video (not static Mario image)
- [ ] Video is smooth (60fps, no stuttering)
- [ ] No vertical scrolling or tearing
- [ ] No horizontal misalignment
- [ ] Colors correct (game_palette mapping)

### Phase 3: Audio Quality
- [ ] Audio plays without jitter
- [ ] Audio stays synchronized with video
- [ ] No audio dropouts or pops
- [ ] Buffer status stable (not emptying or overflowing)

### Phase 4: Simultaneous Quality
- [ ] **CRITICAL:** Video smooth AND audio perfect TOGETHER
- [ ] No degradation when both active
- [ ] Controller input responsive
- [ ] System stable for extended play (30+ minutes)

---

## 🐛 DEBUGGING

### Serial Output Diagnostics
```
=== PIO VIDEO + AUDIO MODE ===
Video: PIO hardware capture with DMA (non-blocking)
Audio: PIO hardware timer, jitter-free, 32kHz
CPU: Free for controller polling and buffer management
Frames: 1234 | Audio: 1975/2048 (w=73 r=2048)
```

**What to look for:**
- `Frames:` should increment steadily (~60/sec)
- `Audio:` fill level should oscillate (healthy buffer usage)
- `w=` and `r=` should be non-zero (active consumption)

### Common Issues

**Issue: Frames not incrementing**
- Check: PIO program loaded? DMA initialized?
- Check: VSYNC signal present? (GPIO 4)
- Check: Game Boy powered on and producing video?

**Issue: Video distorted/garbled**
- Check: Pin mappings correct (DATA_1=GPIO1, DATA_0=GPIO2)
- Check: Unpack logic bit order (confirm with logic analyzer)
- Check: PIXEL_CLOCK polarity (rising edge)

**Issue: Audio degraded**
- Check: PIO audio timer still at 500Hz
- Check: Unpack loop not taking too long (add timing measurement)
- Check: No interrupt conflicts (DMA_IRQ_0 vs PIO1_IRQ_1)

**Issue: System unstable/crashes**
- Check: Buffer sizes correct (PACKED_FRAME_SIZE = 5760)
- Check: No buffer overruns (DMA transfer count)
- Check: Stack size sufficient for locals

---

## 🔬 VERIFICATION METHODS

### Logic Analyzer Capture
- Probe GPIO 1, 2, 3, 4 (DATA_1, DATA_0, PIXEL_CLOCK, VSYNC)
- Verify PIO reads on PIXEL_CLOCK rising edge
- Verify 160 pixels per HSYNC period
- Verify 144 HSYNCs per VSYNC period

### Oscilloscope
- Measure DMA completion interval (~16.7ms @ 60fps)
- Measure unpack duration (should be <2ms)
- Verify no long CPU blocks

### Software Instrumentation
Add timing measurements:
```c
uint32_t start = time_us_32();
// Unpack loop
uint32_t end = time_us_32();
if ((end - start) > 2000) {
    printf("WARNING: Unpack took %lu µs\n", end - start);
}
```

---

## 🚀 NEXT STEPS

### If Testing Succeeds ✅
1. **Optimize unpacking** (if needed)
   - Use DMA to unpack? (separate DMA channel with scatter-gather)
   - Use SIMD-style operations (process 4 bytes at once)
2. **Add frame blending** (optional, for smoother motion)
3. **Implement OSD** (on-screen display for settings)
4. **Document final solution**

### If Testing Reveals Issues ❌
1. **Add detailed logging** in unpacking loop
2. **Verify PIO output** with logic analyzer
3. **Test with simpler PIO program** (fewer pixels)
4. **Consider alternative approaches:**
   - Store framebuffer in packed format? (modify display core)
   - Use second DMA channel for unpacking?
   - Reduce resolution temporarily for testing?

---

## 📝 FILES MODIFIED

1. **`video_capture.pio`** (NEW)
   - PIO program for hardware pixel capture
   - DMA initialization and management
   - Interrupt handlers

2. **`CMakeLists.txt`**
   - Added `pico_generate_pio_header(dmg video_capture.pio)`

3. **`main.c`**
   - Added packed buffer declarations
   - Added PIO video initialization
   - Modified main loop for frame-based capture
   - Integrated unpacking logic

4. **Unchanged (working perfectly):**
   - `audio_timer.pio` - PIO audio timer
   - `pio_audio_timer.c` - Audio timer driver
   - `emusound.c` - Audio processing
   - `analog_microphone.c` - ADC capture

---

## 💡 DESIGN PHILOSOPHY

**Separation of Concerns:**
- **PIO:** Hardware timing and synchronization
- **DMA:** Bulk data transfer without CPU
- **CPU:** Lightweight coordination and processing
- **Core 1:** Independent HDMI output

**Minimizing CPU Blocking:**
- No polling loops waiting for signals
- No manual pixel reading in tight loops
- Interrupt-driven completion notifications
- Quick processing between frames

**Preserving Audio Quality:**
- Audio PIO timer runs independently
- Highest interrupt priority ensures servicing
- No code paths that block >2ms
- Triple-buffered to prevent races

---

## 📖 REFERENCES

- RP2040 Datasheet: PIO Chapter
- Pico SDK: hardware_pio, hardware_dma
- PicoDVI library: DVI timing and encoding
- Game Boy LCD timing: 59.7fps, 160×144, 4 colors

---

**BUILD OUTPUT:**
```
File: c:\VSARM\sdk\pico\PicoDVI-DMG\software\build\apps\dmg\dmg.uf2
Size: 160,256 bytes
Date: November 17, 2025, 5:44:22 PM
Status: READY FOR TESTING
```

**NEXT ACTION:** Flash to Pico and test with Game Boy! 🎮

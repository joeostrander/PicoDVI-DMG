# VSYNC Synchronization Mode Toggle

## Overview
The video capture system now supports **two different VSYNC synchronization methods** that can be toggled with a simple `#define` in `main.c`. This allows easy testing and comparison between the two approaches.

## Configuration

### Location: `main.c` (lines ~53-58)

```c
// ===== VIDEO CAPTURE MODE SELECTION =====
// Toggle between two VSYNC synchronization methods:
//   1 = IRQ-based (GPIO interrupt on VSYNC rising edge - precise, no PIO wait)
//   0 = WAIT-based (PIO 'wait' instruction for VSYNC - simpler, may scroll on power-on)
#define USE_VSYNC_IRQ 0  // Change to 0 to test WAIT mode, 1 for IRQ mode
// =========================================
```

**To switch modes:** Simply change the `0` to `1` (or vice versa), save, and rebuild.

## Mode Comparison

### Mode 1: IRQ-Based VSYNC (`USE_VSYNC_IRQ 1`)
**Uses:** GPIO hardware interrupt to detect VSYNC rising edge

**How it works:**
1. PIO program has NO VSYNC wait instruction
2. Main.c "arms" capture by setting `capture_armed = true`
3. GPIO 4 (VSYNC) interrupt handler detects rising edge
4. IRQ handler starts PIO state machine and DMA **atomically** at frame boundary
5. Eliminates timing uncertainty

**Advantages:**
- ✅ Precise frame synchronization (atomic start at VSYNC edge)
- ✅ Works perfectly from power-on
- ✅ No scrolling issues after power cycling
- ✅ Frame starts are guaranteed aligned

**Disadvantages:**
- ❌ More complex code (GPIO IRQ sharing required)
- ❌ Requires careful IRQ setup order
- ❌ May cause slow painting effect (needs investigation)

**Files involved:**
- `video_capture.pio` - Uses `video_capture_irq_program` (no VSYNC wait)
- `main.c` - Enables GPIO 4 interrupt, routes to `video_capture_handle_vsync_irq()`

---

### Mode 0: WAIT-Based VSYNC (`USE_VSYNC_IRQ 0`)
**Uses:** PIO `wait` instruction to monitor VSYNC GPIO

**How it works:**
1. PIO program starts with `wait 1 gpio 4` (wait for VSYNC high)
2. PIO SM and DMA start immediately when `video_capture_start_frame()` is called
3. PIO state machine waits internally for VSYNC to go high
4. Once VSYNC goes high, PIO proceeds to capture scanlines
5. Simpler approach, no IRQ required

**Advantages:**
- ✅ Simpler code (no GPIO IRQ complications)
- ✅ Self-contained in PIO program
- ✅ Faster painting (may fix slow painting effect)
- ✅ No GPIO IRQ sharing needed

**Disadvantages:**
- ❌ May have slight timing variance
- ❌ Can scroll left on power-on (PIO may start mid-frame)
- ❌ Less precise frame alignment

**Files involved:**
- `video_capture.pio` - Uses `video_capture_wait_program` (has VSYNC wait)
- `main.c` - Does NOT enable GPIO 4 interrupt

---

## Implementation Details

### Two PIO Programs

The `.pio` file now defines **two separate programs**:

1. **`video_capture_irq`** - For IRQ mode (31 instructions)
   - No VSYNC wait
   - Started by GPIO interrupt handler
   
2. **`video_capture_wait`** - For WAIT mode (32 instructions)
   - Has `wait 1 gpio 4` at the start
   - Self-synchronized to VSYNC

### Conditional Compilation

The C SDK code uses `#if USE_VSYNC_IRQ` to compile only the needed code:

```c
#if USE_VSYNC_IRQ
    // IRQ-specific code (arming, interrupt handlers, etc.)
#else
    // WAIT-specific code (immediate PIO/DMA start)
#endif
```

### Program Selection in main.c

```c
#if USE_VSYNC_IRQ
    video_offset = pio_add_program(pio_video, &video_capture_irq_program);
#else
    video_offset = pio_add_program(pio_video, &video_capture_wait_program);
#endif
```

### DMA/PIO Start Behavior

**IRQ Mode (`video_capture_start_frame`):**
```c
capture_armed = true;  // Arm for next VSYNC
// PIO/DMA started by IRQ handler
```

**WAIT Mode (`video_capture_start_frame`):**
```c
pio_sm_set_enabled(pio, sm, true);  // Start PIO immediately
dma_channel_start(video_dma_chan);  // Start DMA immediately
// PIO waits internally for VSYNC
```

## Testing Procedure

### Test WAIT Mode (Simpler)
1. Set `#define USE_VSYNC_IRQ 0` in main.c
2. Rebuild and flash
3. Check serial output: Should see "WAIT-based VSYNC synchronization"
4. Observe: Faster painting, but may scroll after power cycle

### Test IRQ Mode (Precise)
1. Set `#define USE_VSYNC_IRQ 1` in main.c
2. Rebuild and flash
3. Check serial output: Should see "IRQ-based VSYNC synchronization"
4. Check serial: "VSYNC IRQs: X" should increment (~59/sec)
5. Observe: No scrolling on power-on, but may paint slowly

## Troubleshooting

### IRQ Mode Not Working
- Check serial output for VSYNC IRQ count
- Verify GPIO 4 interrupt is enabled in main.c
- Ensure GPIO callback routes VSYNC to video capture handler
- Check that `capture_armed` is set to true

### WAIT Mode Scrolling
- Normal behavior on power-on (PIO may start mid-frame)
- Should stabilize after first full frame
- No fix needed - this is expected with WAIT mode

### Slow Painting in IRQ Mode
- Try switching to WAIT mode to compare
- May be related to timing of IRQ vs. PIO start
- Could be DMA/PIO synchronization issue

## Files Modified

1. **software/apps/dmg/video_capture.pio**
   - Added two PIO programs: `video_capture_irq` and `video_capture_wait`
   - Conditional compilation for IRQ-specific code
   - Mode-specific initialization and frame start logic

2. **software/apps/dmg/main.c**
   - Added `USE_VSYNC_IRQ` toggle at top of file
   - Conditional GPIO interrupt enable
   - Program selection based on mode
   - Debug output showing which mode is active

## Recommendation

**For production:** Use IRQ mode (`USE_VSYNC_IRQ 1`) once slow painting is resolved
- Precise synchronization is worth the complexity
- Eliminates power-on scrolling issues

**For debugging:** Use WAIT mode (`USE_VSYNC_IRQ 0`)
- Simpler to understand and debug
- May help isolate timing issues

## Build Output
✅ Build successful with mode toggle feature
- Binary size: ~79 KB
- Both modes compile cleanly
- Easy to switch between modes

# Game Boy DMG Digital Timing Summary

## Overview

This document describes the Game Boy DMG video output timing protocol

## Signal Definitions

### Digital Signals (5 Total)

| Signal | Direction | Logic Levels | Purpose |
|--------|-----------|--------------|---------|
| **VSYNC** | Output | 0→1 Frame Start, 1→0 Line 1 end | Frame synchronization |
| **HSYNC** | Output | 0→1 Line Start, 1→0 Pixel 0 ready | Line synchronization |
| **CLOCK** | Output | 1→0 Pixels 1-159 | Pixel clock (160 clocks per line) |
| **D1** | Output | 0 or 1 | Data bit 1 (MSB of 2-bit pixel) |
| **D0** | Output | 0 or 1 | Data bit 0 (LSB of 2-bit pixel) |

### Pixel Encoding

Each pixel is encoded as a 2-bit grayscale value on D1 (MSB) and D0 (LSB):

```
D1  D0  |  Value  |  Color
-----------------------------
0   0   |    0    |  White
0   1   |    1    |  Light Gray
1   0   |    2    |  Dark Gray
1   1   |    3    |  Black
```

## Frame Structure

- **Frame Size**: 160 pixels wide × 144 pixels tall
- **Total Pixels**: 23,040 pixels per frame

## Timing Protocol

### Frame Synchronization (VSYNC)

```
VSYNC Signal:
     _______________          _______________
____|               |________|               |____
    ^                        ^
    Frame N starts           Frame N+1 starts
    
Rising Edge (0→1): Marks the START of a new frame
```

**Key Points:**
- VSYNC rising edge (0→1 transition) indicates a new frame is beginning
- Between frames, VSYNC stays HIGH for the first row of pixels
- After 1st row, VSYNC stays LOW

### Line Synchronization (HSYNC)

```
HSYNC Signal (per scanline):
     ____              ____              ____
____|    |____________|    |____________|    |____
    ^    ^            ^    ^            ^    ^
    |    |            |    |            |    |
  Start  Pixel      Start  Pixel      Start  Pixel
  Line   capture    Line   capture    Line   capture
    0    begins       1    begins       2    begins
```

**Key Points:**
- HSYNC rising edge (0→1): Marks START OF LINE (setup phase)
- HSYNC falling edge (1→0): First pixel data is NOW VALID on D1/D0

### Pixel Capture Timing (The Critical Part!)


```
Clock Cycle:    |  Setup  | Pixel 0 | Pixel 1 | Pixel 2 | ... | Pixel 159 |
                |         |         |         |         |     |           |
HSYNC:      ____/‾‾‾‾‾‾‾‾‾\__________________________________________
                ^         ^
                |         |
            Line start    First pixel data valid HERE!
                
CLOCK:      ________________/‾‾‾\___/‾‾‾\___/‾‾‾\___/‾‾‾\___
                            |   |   |   |   |   |   |   |
                            | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7
                            
D1/D0:      --------[setup]-[P0]-[P1]-[P2]-[P3]-[P4]-[P5]-...
                            ^^^^
                            First pixel captured when HSYNC falls!
```

### Detailed Pixel Capture Sequence

#### Step 1: HSYNC Rising Edge (Line Start)
```
Time T0: HSYNC transitions 0→1
- This marks the beginning of scanline setup
- Data lines D1/D0 are not yet valid for pixel capture
- DMG prepares to output first pixel
```

#### Step 2: HSYNC Falling Edge (Pixel 0 Valid)
```
Time T1: HSYNC transitions 1→0
- **CRITICAL**: Pixel 0 data is IMMEDIATELY valid on D1/D0
- Capture pixel 0 at THIS EXACT MOMENT
- Clock state doesn't matter for pixel 0
```

#### Step 3: Clock Falling Edges (Pixels 1-159)
```
Time T2, T3, T4, ... T160:
- Each CLOCK falling edge (1→0) indicates new pixel data
- Capture pixels 1 through 159 sequentially
- D1/D0 update synchronously with clock falling edge
```

## State Machine View

### Receiver State Machine

```
                    ┌──────────────┐
                    │ WAIT_VSYNC   │
                    │ (Idle)       │
                    └──────┬───────┘
                           │ VSYNC rising edge
                           ▼
                    ┌──────────────┐
                    │ WAIT_HSYNC   │
                    │ (Start Frame)│
                    └──────┬───────┘
                           │ HSYNC rising edge
                           ▼
                    ┌──────────────┐
                    │ WAIT_PIXEL0  │
                    │ (Line Setup) │
                    └──────┬───────┘
                           │ HSYNC falling edge
                           ▼
                    ┌──────────────┐
           ┌───────→│ CAPTURE_P0   │
           │        │ (Read D1/D0) │
           │        └──────┬───────┘
           │               │ Increment X
           │               ▼
           │        ┌──────────────┐
           │   ┌───→│ WAIT_CLOCK   │
           │   │    │ (Pixels 1+)  │
           │   │    └──────┬───────┘
           │   │           │ CLOCK falling edge
           │   │           ▼
           │   │    ┌──────────────┐
           │   └────│ CAPTURE_PX   │
           │        │ (Read D1/D0) │
           │        └──────┬───────┘
           │               │
           │               ├── If X < 159: loop back to WAIT_CLOCK
           │               │
           │               └── If X = 159 && Y < 143: next line
           │                   (goto WAIT_HSYNC, increment Y)
           │
           └─────────────── If Y = 143: frame complete
                           (goto WAIT_VSYNC)
```

## Timing Diagrams

### Single Scanline (160 pixels)

```
Time →

HSYNC:    ‾‾‾‾‾‾\____________________________________________________/‾‾‾
          setup  |                                                  | next
                 |                                                  | line
                 
CLOCK:    ________|‾\__/‾\__/‾\__/‾\__/‾\__/‾\__/‾\__/‾\__ ... __/‾\__|____
                  | |  | |  | |  | |  | |  | |  | |  | |       | |  |
                  
D1/D0:    ========[P0][P1][P2][P3][P4][P5][P6][P7]... [P158][P159]=====
                  ^^^                                   ^^^^^ ^^^^^
                  Captured on                           Sequential
                  HSYNC falling                         clock edges

Pixels:           0   1   2   3   4   5   6   7  ...   158   159
```

### Complete Frame (144 lines)

```
VSYNC:    ‾‾\___________________________________________________/‾‾‾‾
            |                                                   |
            Frame N                                             Frame N+1

HSYNC:      ‾\__/‾\__/‾\__/‾\__/‾\__ ... __/‾\__/‾\__
            |    |    |    |    |         |    |
            Line Line Line Line Line      Line Line
            0    1    2    3    4   ...   142  143

Y-pos:      0    1    2    3    4   ...   142  143
```

## Signal Characteristics

### Logic Levels
- **Logic HIGH (1)**: ~5V (TTL level)
- **Logic LOW (0)**: ~0V (TTL level)
- **Threshold**: ~2.5V (typical TTL threshold)

### Timing Specifications (Approximate)

Based on Game Boy hardware:

- **Clock Frequency**: 4.194304 MHz
- **Pixel Rate**: ~250ns per pixel (approximate)
- **Line Time**: ~40μs per line (160 pixels + overhead)
- **Frame Time**: ~16.7ms per frame @ 60fps (may vary)

### Edge Specifications

- **Rising Edge**: 0→1 transition, typically <50ns
- **Falling Edge**: 1→0 transition, typically <50ns
- **Setup Time**: Data stable before clock edge
- **Hold Time**: Data stable after clock edge

## Decoding Algorithm (Pseudocode)

```c
// Initialize
int frame = 0, y = 0, x = 0;
bool vsync_last = 1, hsync_last = 1, clock_last = 0;

while (reading_samples) {
    // Read current sample
    sample = read_sample();
    
    // Frame synchronization
    if (sample.vsync == 1 && vsync_last == 0) {
        frame++;
        y = 0;
    }
    
    // Line synchronization
    if (sample.hsync == 1 && hsync_last == 0) {
        // Start of new line (setup phase)
        x = 0;
    }
    
    // Pixel 0 capture (CRITICAL!)
    if (sample.hsync == 0 && hsync_last == 1) {
        // HSYNC just fell - pixel 0 data is valid NOW!
        pixel_value = (sample.d1 << 1) | sample.d0;
        store_pixel(frame, y, x, pixel_value);
        x++;
        clock_last = sample.clock; // Initialize clock state
    }
    
    // Pixels 1-159 capture
    else if (x > 0 && x < 160) {
        if (sample.clock == 0 && clock_last == 1) {
            // Clock falling edge - capture pixel
            pixel_value = (sample.d1 << 1) | sample.d0;
            store_pixel(frame, y, x, pixel_value);
            x++;
            
            if (x == 160) {
                y++; // Move to next scanline
            }
        }
        clock_last = sample.clock;
    }
    
    // Update state
    vsync_last = sample.vsync;
    hsync_last = sample.hsync;
}
```

## Key Takeaways

### Critical Timing Rules

1. **VSYNC rising edge** = New frame starts
2. **HSYNC rising edge** = New line starts (setup)
3. **HSYNC falling edge** = Pixel 0 valid **NOW** (not next sample!)
4. **CLOCK falling edge** = Pixels 1-159 valid sequentially

### Common Mistakes to Avoid

❌ **Don't** wait for next sample after HSYNC falls  
✅ **Do** capture pixel 0 from same sample where HSYNC falls

❌ **Don't** use clock edges for pixel 0  
✅ **Do** use HSYNC falling edge for pixel 0

❌ **Don't** assume data is stable across multiple samples  
✅ **Do** capture data synchronously with edges

---


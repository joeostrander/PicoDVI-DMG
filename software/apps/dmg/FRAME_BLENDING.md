# Frame Blending Feature

## What is Frame Blending?

Frame blending is a technique that blends the current video frame with the previous frame to create ghost images. This is particularly useful for certain Game Boy games that used alternating frame techniques to create overlay effects or transparency illusions.

## Game Boy Overlay Effects

Some Game Boy games (like certain RPGs and action games) would alternate sprites between frames to create:
- **Transparency effects** - Make sprites appear semi-transparent
- **Overlay indicators** - Show targeting reticles or selection indicators
- **Composite images** - Combine multiple sprite layers into one visual

Without frame blending, these effects appear as **rapid flickering**. With frame blending enabled, the flickering sprites blend together to create the intended visual effect.

## Implementation Details

### How It Works

The frame blending system maintains a copy of the previous frame and blends it with the current frame using this logic:

```c
For each pixel:
    if (current_pixel == BLACK) {
        display_pixel = previous_pixel;  // Show ghost from previous frame
    } else {
        display_pixel = current_pixel;   // Current sprite takes priority
    }
```

This means:
- **Black pixels (value 3)** in the current frame are replaced with pixels from the previous frame
- **Non-black pixels** (white, light gray, dark gray) are displayed normally
- Creates a "ghost image" effect where previous frame sprites show through black areas

### Performance Optimizations

1. **Packed Byte Processing**: Operates on packed 2bpp data (4 pixels per byte) for efficiency
2. **In-Place Blending**: Modifies the captured frame buffer directly
3. **DMA-Safe**: Uses memory barriers to ensure Core 1 sees complete frames

### Memory Usage

- **Previous Frame Buffer**: 5,760 bytes (160×144 ÷ 4 pixels per byte)
- **No Additional Display Buffer**: Blending happens on the captured buffer before display

## Usage

### Enabling/Disabling Frame Blending

Press **SELECT + HOME** simultaneously to toggle frame blending:
- ✅ **ENABLED**: Previous frames blend with current frames
- ❌ **DISABLED**: Only current frame is displayed (normal mode)

Status messages are printed to the serial console:
```
Frame blending: ENABLED
Frame blending: DISABLED
```

### When to Use Frame Blending

**Enable for games with:**
- Rapidly flickering sprites
- Transparency effects that appear as flicker
- Overlay indicators (crosshairs, selection boxes)
- Games known to use alternating frame techniques

**Disable for:**
- Normal gameplay without overlay effects
- Games where motion blur is undesirable
- When you want crisp, flicker-free visuals

## Code Structure

### Key Variables

```c
static uint8_t packed_buffer_previous[PACKED_FRAME_SIZE];  // Previous frame storage
static volatile bool frameblending_enabled = false;        // Feature toggle
```

### Blending Loop (in main.c)

```c
if (frameblending_enabled) {
    for (size_t i = 0; i < PACKED_FRAME_SIZE; i++) {
        uint8_t current_byte = completed_packed[i];
        uint8_t previous_byte = packed_buffer_previous[i];
        uint8_t blended_byte = 0;
        
        // Process each of the 4 pixels in this byte
        for (int pixel = 0; pixel < 4; pixel++) {
            // Extract 2-bit pixel values
            int shift = (3 - pixel) * 2;
            uint8_t current_pixel = (current_byte >> shift) & 0x03;
            uint8_t previous_pixel = (previous_byte >> shift) & 0x03;
            
            // Blend: black pixels show previous frame
            uint8_t blended_pixel = (current_pixel == 3) ? 
                                    previous_pixel : current_pixel;
            
            blended_byte |= (blended_pixel << shift);
        }
        
        completed_packed[i] = blended_byte;
    }
    
    // Save current frame for next blend
    memcpy(packed_buffer_previous, completed_packed, PACKED_FRAME_SIZE);
}
```

## Comparison with Original Implementation

### Original (old_code.c)

```c
// Per-pixel GPIO capture with inline blending
static void read_pixel(void)
{
    uint8_t new_value = (gpio_get(DATA_0_PIN) << 1) + gpio_get(DATA_1_PIN);
    *pixel_active++ = frameblending_enabled ? 
                      (new_value == 0 ? new_value|*pixel_old : new_value) : 
                      new_value;
    *pixel_old++ = new_value > 0 ? 2 : 0;  // Brighten previous frame
}
```

**Issues:**
- Blended during GPIO capture (critical timing path)
- Operated on unpacked 8bpp data
- "Brightening" previous frame was unclear/inconsistent

### New Implementation (main.c)

```c
// Post-capture blending on packed 2bpp data
if (frameblending_enabled) {
    // Process entire frame after DMA capture
    for (size_t i = 0; i < PACKED_FRAME_SIZE; i++) {
        // Blend at byte level (4 pixels at once)
        // Black pixels replaced with previous frame
    }
}
```

**Improvements:**
- ✅ **No timing impact**: Blending happens after capture, not during
- ✅ **Packed format**: Works directly on 2bpp data (more efficient)
- ✅ **Clear logic**: Black = show previous, non-black = show current
- ✅ **DMA-safe**: Proper memory barriers for dual-core access
- ✅ **Easy toggle**: SELECT + HOME button combo

## Example Games That Benefit

Games known to use alternating frame techniques:
- **The Legend of Zelda: Link's Awakening** - Transparency effects
- **Metroid II** - Certain enemy overlays
- **Kirby's Dream Land 2** - Some power-up indicators
- Various RPGs with selection indicators

## Technical Notes

### Game Boy Pixel Values

```
Value | Color      | Binary
------|------------|-------
  0   | White      | 00
  1   | Light Gray | 01
  2   | Dark Gray  | 10
  3   | Black      | 11
```

Frame blending uses **black (value 3)** as the "transparent" color because Game Boy games typically:
1. Draw sprites on black backgrounds
2. Use black for empty/transparent sprite areas
3. Alternate sprite positions between frames

### Why Black = Transparent?

When a game wants to show an overlay:
- **Frame A**: Draw sprite at position X with black background
- **Frame B**: Draw different sprite or nothing (all black)
- **Result**: With blending, Frame A's sprite "shows through" Frame B's black areas

This creates the illusion of transparency or overlay without actual alpha blending in the Game Boy hardware.

## Future Enhancements

Potential improvements:
- [ ] Adjustable blend strength (currently binary: show/hide)
- [ ] Multiple frame history (blend across 3-4 frames)
- [ ] Per-game blending presets (save to flash)
- [ ] Different blend modes (average, max, min)
- [ ] Motion detection to disable blending during fast movement

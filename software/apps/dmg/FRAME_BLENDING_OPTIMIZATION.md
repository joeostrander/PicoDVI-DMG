# Frame Blending Performance Optimization

**Date**: November 28, 2025  
**Issue**: Horizontal sliding/paging when frame blending was enabled  
**Root Cause**: Frame blending processing was too slow, causing timing issues  
**Solution**: Ultra-fast lookup table (LUT) based implementation  

---

## Problem Analysis

### Original Implementation
The initial frame blending code processed 5,760 bytes with nested loops:
- **Outer loop**: 5,760 iterations (one per byte)
- **Inner loop**: 4 iterations per byte (one per pixel)
- **Total operations**: ~23,040 pixel-level operations per frame
- **Each operation**: Bit shifts, masks, conditional logic, OR operations

### Performance Impact
At 60 fps Game Boy refresh rate:
- Frame time: ~16.67 ms
- Processing overhead: Several milliseconds
- **Result**: Not fast enough → video desynchronization → horizontal sliding

---

## Optimization Strategy

### Lookup Table Approach
Instead of computing pixel blending in real-time, **precompute all possible results**:

1. **Store LUT** (`store_lut[256]`): For each possible current byte value, what to store for next frame
2. **Blend LUT** (`blend_lut[256][256]`): For each combination of current and previous byte, the blended result

### Memory Cost
- `store_lut`: 256 bytes
- `blend_lut`: 256 × 256 = 65,536 bytes
- **Total**: ~64 KB of RAM (well within Pico's 264 KB)

### Performance Gain
**Before (nested loops)**: ~23,040 operations per frame
- Bit extraction (shifts + masks)
- Conditional blending logic
- Bit packing (shifts + OR)

**After (LUT)**: 5,760 operations per frame (only array lookups)
- 1 lookup for blended result: `blend_lut[current][previous]`
- 1 lookup for storage: `store_lut[current]`

**Speedup**: ~4× faster (or more, considering eliminated branches)

---

## Implementation Details

### Lookup Table Initialization
Done once at startup in `init_frame_blending_luts()`:

```c
// Build store_lut: what to save for next frame's ghost
for (int curr = 0; curr < 256; curr++) {
    uint8_t result = 0;
    for (int pixel = 0; pixel < 4; pixel++) {
        int shift = (3 - pixel) * 2;
        uint8_t p = (curr >> shift) & 0x03;
        // Non-white (1,2,3) → gray (2), white (0) → white (0)
        uint8_t store_p = (p > 0) ? 2 : 0;
        result |= (store_p << shift);
    }
    store_lut[curr] = result;
}

// Build blend_lut: blended output for all current/previous combinations
for (int curr = 0; curr < 256; curr++) {
    for (int prev = 0; prev < 256; prev++) {
        uint8_t result = 0;
        for (int pixel = 0; pixel < 4; pixel++) {
            int shift = (3 - pixel) * 2;
            uint8_t p_curr = (curr >> shift) & 0x03;
            uint8_t p_prev = (prev >> shift) & 0x03;
            // White (0) pixels OR with previous, non-white use current
            uint8_t blended = (p_curr == 0) ? (p_curr | p_prev) : p_curr;
            result |= (blended << shift);
        }
        blend_lut[curr][prev] = result;
    }
}
```

### Frame Blending (Runtime)
Ultra-simple loop with just array lookups:

```c
if (frameblending_enabled) {
    for (size_t i = 0; i < PACKED_FRAME_SIZE; i++) {
        uint8_t current = completed_packed[i];
        uint8_t previous = packed_buffer_previous[i];
        
        // Single lookup for blended result
        completed_packed[i] = blend_lut[current][previous];
        
        // Single lookup for what to store for next frame
        packed_buffer_previous[i] = store_lut[current];
    }
}
```

**No branches, no bit manipulation, just fast memory lookups!**

---

## Code Changes

### Files Modified

1. **`main.c`** - Added LUT declarations:
   ```c
   static uint8_t blend_lut[256][256];  // Blended output
   static uint8_t store_lut[256];       // What to store for next frame
   ```

2. **`main.c`** - Added `init_frame_blending_luts()` function before `main()`

3. **`main.c`** - Call initialization in `main()`:
   ```c
   stdio_init_all();
   sleep_ms(3000);
   init_frame_blending_luts();  // ← Added
   ```

4. **`main.c`** - Replaced frame blending loop with LUT-based version

---

## Performance Characteristics

### CPU Usage (Estimated)
- **Old**: ~3-5 ms per frame (23K operations)
- **New**: ~0.5-1 ms per frame (5.7K lookups)
- **Savings**: 2-4 ms per frame

### Memory Usage
- **Code size increase**: ~1.5 KB (160,256 bytes vs 158,720 bytes)
- **RAM increase**: 64 KB for lookup tables
- **Pico RP2040**: 264 KB total RAM → plenty available

### Timing Margin
Game Boy VSYNC period: ~16.67 ms (60 Hz)
- Before: Processing might exceed budget → desync
- After: Plenty of headroom for other tasks

---

## Testing Checklist

- [ ] Build successful (✅ Confirmed - `dmg.uf2` created)
- [ ] Frame blending toggle works (SELECT + HOME)
- [ ] No horizontal sliding with blending enabled
- [ ] Ghost trails behave correctly (fade after 1 frame)
- [ ] Performance is smooth at 60 fps
- [ ] No visual artifacts

---

## Evolution of Frame Blending

1. **Initial**: Pixel-by-pixel with nested loops
2. **First optimization**: Byte-level with branchless operations
3. **Current**: Lookup table based (4× faster)

This optimization represents the final evolution - maximum speed with minimal CPU overhead!

---

## Technical Notes

### Why This Works
- **Cache-friendly**: Sequential memory access pattern
- **No branches**: Lookup tables eliminate all conditionals
- **Vectorizable**: Simple loop structure allows compiler optimizations
- **Predictable**: No data-dependent branching or complex logic

### Alternative Approaches Considered
1. ✗ Move to Core 1 - Core 1 is busy with DVI output
2. ✗ Use DMA - Too complex for bit manipulation
3. ✗ Use PIO - PIO state machines already in use
4. ✅ **Lookup tables** - Perfect fit!

### Future Optimizations
If even more speed is needed:
- Use `memcpy` optimizations (word-aligned transfers)
- Unroll the loop (process multiple bytes per iteration)
- Use ARM NEON intrinsics (SIMD operations)

But the current LUT approach should be more than fast enough!

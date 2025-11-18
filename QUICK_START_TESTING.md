# Quick Start Guide - PIO Video Capture Testing

## Flash the Firmware

1. Connect Raspberry Pi Pico to PC via USB while holding BOOTSEL button
2. Copy `dmg.uf2` to the RPI-RP2 drive that appears
3. Pico will automatically reboot with new firmware

**File Location:** `c:\VSARM\sdk\pico\PicoDVI-DMG\software\build\apps\dmg\dmg.uf2`

## Connect Serial Monitor

1. Open terminal/PuTTY at **115200 baud**
2. Connect to Pico's USB serial port
3. You should see startup messages:

```
=== PicoDVI-DMG Starting ===
Audio system initialized
Core 1 running - now consuming audio!
Analog microphone started successfully
=== INITIALIZING PIO VIDEO CAPTURE ===
PIO video capture program loaded at offset X
PIO video capture DMA initialized (packed format: 5760 bytes)
PIO video capture started - waiting for VSYNC...
=== PIO VIDEO + AUDIO MODE ===
```

## What to Expect

### ✅ SUCCESS INDICATORS

**Video:**
- Game Boy display appears on HDMI screen (not static Mario)
- Smooth 60fps video
- No tearing, scrolling, or stuttering
- Colors mapped correctly

**Audio:**
- Clear game audio through HDMI
- No crackling, pops, or jitter
- Stays synchronized with video

**Serial Output:**
```
Frames: 60 | Audio: 1950/2048 (w=98 r=2048)
Frames: 120 | Audio: 1975/2048 (w=73 r=2048)
Frames: 180 | Audio: 1925/2048 (w=123 r=2048)
```
- Frames counter increments steadily
- Audio fill level oscillates (healthy)

### ❌ FAILURE MODES

**No Video Capture:**
```
Frames: 0 | Audio: 1975/2048 (w=73 r=2048)
```
- Frames stuck at 0 = PIO not triggering or DMA not completing
- Check Game Boy is powered on
- Check VSYNC connection (GPIO 4)

**Distorted Video:**
- Vertical lines, wrong colors = bit order issue
- Check DATA_0 and DATA_1 pin connections
- May need to swap bits in unpack logic

**Bad Audio:**
- Crackling/jitter = CPU blocking too long
- Add timing measurement to unpack loop
- May need to optimize unpacking

**System Crash/Hang:**
- Check buffer sizes are correct
- Check for memory corruption
- Review DMA configuration

## Testing Procedure

1. **Power on Game Boy** with game inserted
2. **Flash firmware** to Pico
3. **Connect HDMI** to TV/monitor
4. **Open serial monitor** to see diagnostics
5. **Play game** for at least 5 minutes
6. **Observe:**
   - Video quality (smoothness, clarity)
   - Audio quality (no jitter, sync)
   - Serial output (frame rate, buffer status)
   - Controller responsiveness

## Troubleshooting Commands

### Check Frame Timing
Add to main loop temporarily:
```c
static uint32_t last_frame_time = 0;
if (video_capture_frame_ready()) {
    uint32_t now = time_us_32();
    uint32_t delta = now - last_frame_time;
    if (delta < 15000 || delta > 18000) {
        printf("WARNING: Frame interval %lu µs\n", delta);
    }
    last_frame_time = now;
    // ... rest of frame handling
}
```
Expected: ~16,667 µs (60fps)

### Check Unpack Performance
```c
if (video_capture_frame_ready()) {
    uint32_t unpack_start = time_us_32();
    // ... unpack loop ...
    uint32_t unpack_time = time_us_32() - unpack_start;
    if (unpack_time > 2000) {
        printf("Unpack slow: %lu µs\n", unpack_time);
    }
}
```
Expected: <2,000 µs

## Pin Reference

| Signal | GPIO | Direction | Notes |
|--------|------|-----------|-------|
| VSYNC | 4 | Input | Frame sync (active high) |
| HSYNC | 0 | Input | Line sync (active high) |
| PIXEL_CLOCK | 3 | Input | Pixel timing (rising edge) |
| DATA_1 | 1 | Input | Pixel data bit 1 |
| DATA_0 | 2 | Input | Pixel data bit 0 |
| LED | 25 | Output | Status indicator |

## Success Criteria

- [ ] Video displays correctly (Game Boy screen visible)
- [ ] Video runs at 60fps (smooth motion)
- [ ] Audio plays clearly (no jitter or distortion)
- [ ] Audio and video stay synchronized
- [ ] Controller input works (buttons responsive)
- [ ] System stable for 30+ minutes
- [ ] Serial output shows healthy buffer status

If all criteria met: **MISSION ACCOMPLISHED!** 🎉

## Next Steps After Success

1. Document the solution
2. Consider optimizations:
   - DMA-based unpacking (if needed)
   - Frame blending (if desired)
   - OSD features
3. Test with different games
4. Measure power consumption
5. Share results!

## If It Doesn't Work

Don't panic! This is complex hardware/software integration. Gather data:

1. **Capture serial output** for analysis
2. **Note specific symptoms** (video, audio, both?)
3. **Test incrementally:**
   - Audio-only mode (disable video) - still works?
   - Simple PIO test (capture fewer pixels)
   - Logic analyzer on signals
4. **Share diagnostics** for collaborative debugging

Remember: We've proven perfect audio works. We've proven video capture works. 
Now we're combining them with hardware assistance. The PIO/DMA approach should 
eliminate the CPU blocking that caused previous issues.

---

**Ready to test? Flash the UF2 and let's see if we've achieved the impossible!** 🚀

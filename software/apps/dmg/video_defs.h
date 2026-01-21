#ifndef VIDEO_DEFS_H
#define VIDEO_DEFS_H

// Game Boy capture geometry (2bpp packed)
#define DMG_PIXELS_X                160
#define DMG_PIXELS_Y                144
#define DMG_PIXEL_COUNT             (DMG_PIXELS_X * DMG_PIXELS_Y)

// Packed DMA buffers - 4 pixels per byte (2 bits each)
#define PACKED_FRAME_SIZE           (DMG_PIXEL_COUNT / 4)
#define PACKED_LINE_STRIDE_BYTES    (DMG_PIXELS_X / 4)

#endif // VIDEO_DEFS_H

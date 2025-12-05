#include "hardware/interp.h"
#include "tmds_encode.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

static const uint32_t __scratch_x("tmds_table") tmds_table[] = {
#include "tmds_table.h"
};

// Fullres table is bandwidth-critical, so gets one copy for each scratch
// memory. There is a third copy which can go in flash, because it's just used
// to generate palette LUTs. The ones we don't use will get garbage collected
// during linking.
const uint32_t __scratch_x("tmds_table_fullres_x") tmds_table_fullres_x[] = {
#include "tmds_table_fullres.h"
};

const uint32_t __scratch_y("tmds_table_fullres_y") tmds_table_fullres_y[] = {
#include "tmds_table_fullres.h"
};

// Configure an interpolator to extract a single colour channel from each of a pair
// of pixels, with the first pixel's lsb at pixel_lsb, and the pixels being
// pixel_width wide. Produce a LUT address for the first pixel's colour data on
// LANE0, and the second pixel's colour data on LANE1.
//
// Returns nonzero if the *_leftshift variant of the encoder loop must be used
// (needed for blue channel because I was a stubborn idiot and didn't put
// signed/bidirectional shift on interpolator, very slightly slower). The
// return value is the size of left shift required.

static int __not_in_flash_func(configure_interp_for_addrgen)(interp_hw_t *interp, uint channel_msb, uint channel_lsb, uint pixel_lsb, uint pixel_width, uint lut_index_width, const uint32_t *lutbase) {
	interp_config c;
	const uint index_shift = 2; // scaled lookup for 4-byte LUT entries

	int shift_channel_to_index = pixel_lsb + channel_msb - (lut_index_width - 1) - index_shift;
	int oops = 0;
	if (shift_channel_to_index < 0) {
		// "It's ok we'll fix it in software"
		oops = -shift_channel_to_index;
		shift_channel_to_index = 0;
	}

	uint index_msb = index_shift + lut_index_width - 1;

	c = interp_default_config();
	interp_config_set_shift(&c, shift_channel_to_index);
	interp_config_set_mask(&c, index_msb - (channel_msb - channel_lsb), index_msb);
	interp_set_config(interp, 0, &c);

	c = interp_default_config();
	interp_config_set_shift(&c, pixel_width	+ shift_channel_to_index);
	interp_config_set_mask(&c, index_msb - (channel_msb - channel_lsb), index_msb);
	interp_config_set_cross_input(&c, true);
	interp_set_config(interp, 1, &c);

	interp->base[0] = (uint32_t)lutbase;
	interp->base[1] = (uint32_t)lutbase;

	return oops;
}

// Extract up to 6 bits from a buffer of 16 bit pixels, and produce a buffer
// of TMDS symbols from this colour channel. Number of pixels must be even,
// pixel buffer must be word-aligned.

void __not_in_flash_func(tmds_encode_data_channel_16bpp)(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb) {
	interp_hw_save_t interp0_save;
	interp_save(interp0_hw, &interp0_save);
	int require_lshift = configure_interp_for_addrgen(interp0_hw, channel_msb, channel_lsb, 0, 16, 6, tmds_table);
	if (require_lshift)
		tmds_encode_loop_16bpp_leftshift(pixbuf, symbuf, n_pix, require_lshift);
	else
		tmds_encode_loop_16bpp(pixbuf, symbuf, n_pix);

	interp_restore(interp0_hw, &interp0_save);
}

// As above, but 8 bits per pixel, multiple of 4 pixels, and still word-aligned.
void __not_in_flash_func(tmds_encode_data_channel_8bpp)(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb) {
	interp_hw_save_t interp0_save, interp1_save;
	interp_save(interp0_hw, &interp0_save);
	interp_save(interp1_hw, &interp1_save);
	// Note that for 8bpp, some left shift is always required for pixel 0 (any
	// channel), which destroys some MSBs of pixel 3. To get around this, pixel
	// data sent to interp1 is *not left-shifted*
	int require_lshift = configure_interp_for_addrgen(interp0_hw, channel_msb, channel_lsb, 0, 8, 6, tmds_table);
	int lshift_upper = configure_interp_for_addrgen(interp1_hw, channel_msb, channel_lsb, 16, 8, 6, tmds_table);
	assert(!lshift_upper); (void)lshift_upper;
	if (require_lshift || (DVI_SYMBOLS_PER_WORD==1))
		tmds_encode_loop_8bpp_leftshift(pixbuf, symbuf, n_pix, require_lshift);
	else
		tmds_encode_loop_8bpp(pixbuf, symbuf, n_pix);
	interp_restore(interp0_hw, &interp0_save);
	interp_restore(interp1_hw, &interp1_save);
}

// ----------------------------------------------------------------------------
// Code for full-resolution TMDS encode (barely possible, utterly impractical):

// Different scheme used for full res as the fun pixel-doubling DC balance
// trick doesn't work, so we need to actually do running disparity. ACCUM0 has
// pixel data, ACCUM1 has running disparity. INTERP0 is used to process even
// pixels, and INTERP1 for odd pixels. Note this means that even and odd
// symbols have their DC balance handled separately, which is not to spec.

static int __not_in_flash_func(configure_interp_for_addrgen_fullres)(interp_hw_t *interp, uint channel_msb, uint channel_lsb, uint lut_index_width, const uint32_t *lutbase) {
	const uint index_shift = 2; // scaled lookup for 4-byte LUT entries

	int shift_channel_to_index = channel_msb - (lut_index_width - 1) - index_shift;
	int oops = 0;
	if (shift_channel_to_index < 0) {
		// "It's ok we'll fix it in software"
		oops = -shift_channel_to_index;
		shift_channel_to_index = 0;
	}

	uint index_msb = index_shift + lut_index_width - 1;

	interp_config c;
	// Shift and mask colour channel to lower 6 bits of LUT index (note lut_index_width excludes disparity sign)
	c = interp_default_config();
	interp_config_set_shift(&c, shift_channel_to_index);
	interp_config_set_mask(&c, index_msb - (channel_msb - channel_lsb), index_msb);
	interp_set_config(interp, 0, &c);

	// Concatenate disparity (ACCUM1) sign onto the LUT index
	c = interp_default_config();
	interp_config_set_shift(&c, 30 - index_msb);
	interp_config_set_mask(&c, index_msb + 1, index_msb + 1);
	interp_set_config(interp, 1, &c);

	interp->base[2] = (uint32_t)lutbase;

	return oops;
}

void __not_in_flash_func(tmds_encode_data_channel_fullres_16bpp)(const uint32_t *pixbuf, uint32_t *symbuf, size_t n_pix, uint channel_msb, uint channel_lsb) {
	uint core = get_core_num();
#if !TMDS_FULLRES_NO_INTERP_SAVE
	interp_hw_save_t interp0_save, interp1_save;
	interp_save(interp0_hw, &interp0_save);
	interp_save(interp1_hw, &interp1_save);
#endif

	// There is a copy of the inner loop and the LUT in both scratch X and
	// scratch Y memories. Use X on core 1 and Y on core 0 so the cores don't
	// tread on each other's toes too much.
	const uint32_t *lutbase = core ? tmds_table_fullres_x : tmds_table_fullres_y;
	int lshift_lower = configure_interp_for_addrgen_fullres(interp0_hw, channel_msb, channel_lsb, 6, lutbase);
	int lshift_upper = configure_interp_for_addrgen_fullres(interp1_hw, channel_msb + 16, channel_lsb + 16, 6, lutbase);
	assert(!lshift_upper); (void)lshift_upper;
	if (lshift_lower) {
		(core ?
			tmds_fullres_encode_loop_16bpp_leftshift_x :
			tmds_fullres_encode_loop_16bpp_leftshift_y
		)(pixbuf, symbuf, n_pix, lshift_lower);
	}
	else {
		(core ?
			tmds_fullres_encode_loop_16bpp_x :
			tmds_fullres_encode_loop_16bpp_y
		)(pixbuf, symbuf, n_pix);
	}
#if !TMDS_FULLRES_NO_INTERP_SAVE
	interp_restore(interp0_hw, &interp0_save);
	interp_restore(interp1_hw, &interp1_save);
#endif
}

static const int8_t imbalance_lookup[16] = { -4, -2, -2, 0, -2, 0, 0, 2, -2, 0, 0, 2, 0, 2, 2, 4 };

static inline int byte_imbalance(uint32_t x)
{
	return imbalance_lookup[x >> 4] + imbalance_lookup[x & 0xF];
}

static void tmds_encode_symbols(uint8_t pixel, uint32_t* negative_balance_sym, uint32_t* positive_balance_sym)
{
	int pixel_imbalance = byte_imbalance(pixel);
	uint32_t sym = pixel & 1;
	if (pixel_imbalance > 0 || (pixel_imbalance == 0 && sym == 0)) {
		for (int i = 0; i < 7; ++i) {
			sym |= (~((sym >> i) ^ (pixel >> (i + 1))) & 1) << (i + 1);
		}
	}
	else {
		for (int i = 0; i < 7; ++i) {
			sym |= ( ((sym >> i) ^ (pixel >> (i + 1))) & 1) << (i + 1);
		}
		sym |= 0x100;
	}

	int imbalance = byte_imbalance(sym & 0xFF);
  if (imbalance == 0) {
		if ((sym & 0x100) == 0) sym ^= 0x2ff;
		*positive_balance_sym = sym;
		*negative_balance_sym = sym;
		return;
	}
	else if (imbalance > 0) {
		*negative_balance_sym = (sym ^ 0x2ff) | (((-imbalance + imbalance_lookup[2 ^ (sym >> 8)] + 2) & 0x3F) << 26);
		*positive_balance_sym = sym | ((imbalance + imbalance_lookup[sym >> 8] + 2) << 26);
	}
	else {
		*negative_balance_sym = sym | (((imbalance + imbalance_lookup[sym >> 8] + 2) & 0x3F) << 26);
		*positive_balance_sym = (sym ^ 0x2ff) | ((-imbalance + imbalance_lookup[2 ^ (sym >> 8)] + 2) << 26);
	}
}

// This takes a 16-bit (RGB 565) colour palette and makes palettes of TMDS symbols suitable
// for performing fullres encode.
// The TMDS palette buffer should be 6 * n_palette words long.
// n_palette must be a power of 2 <= 256.
void tmds_setup_palette_symbols(const uint16_t *palette, uint32_t *tmds_palette, size_t n_palette) {
	uint32_t* tmds_palette_blue = tmds_palette;
	uint32_t* tmds_palette_green = tmds_palette + 2 * n_palette;
	uint32_t* tmds_palette_red = tmds_palette + 4 * n_palette;
	for (int i = 0; i < n_palette; ++i) {
		uint16_t blue = (palette[i] << 3) & 0xf8;
		uint16_t green = (palette[i] >> 3) & 0xfc;
		uint16_t red = (palette[i] >> 8) & 0xf8;
		tmds_encode_symbols(blue, &tmds_palette_blue[i], &tmds_palette_blue[i + n_palette]);
		tmds_encode_symbols(green, &tmds_palette_green[i], &tmds_palette_green[i + n_palette]);
		tmds_encode_symbols(red, &tmds_palette_red[i], &tmds_palette_red[i + n_palette]);
	}
}

// This takes a 24-bit (RGB 888) colour palette and makes palettes of TMDS symbols suitable
// for performing fullres encode.
// The TMDS palette buffer should be 6 * n_palette words long.
// n_palette must be a power of 2 <= 256.
void tmds_setup_palette24_symbols(const uint32_t *palette, uint32_t *tmds_palette, size_t n_palette) {
	uint32_t* tmds_palette_blue = tmds_palette;
	uint32_t* tmds_palette_green = tmds_palette + 2 * n_palette;
	uint32_t* tmds_palette_red = tmds_palette + 4 * n_palette;
	for (int i = 0; i < n_palette; ++i) {
		uint16_t blue = palette[i] & 0xff;
		uint16_t green = (palette[i] >> 8) & 0xff;
		uint16_t red = (palette[i] >> 16) & 0xff;
		tmds_encode_symbols(blue, &tmds_palette_blue[i], &tmds_palette_blue[i + n_palette]);
		tmds_encode_symbols(green, &tmds_palette_green[i], &tmds_palette_green[i + n_palette]);
		tmds_encode_symbols(red, &tmds_palette_red[i], &tmds_palette_red[i + n_palette]);
	}
}

// Encode palette data for all 3 channels.
// pixbuf is an array of n_pix 8-bit wide pixels containing palette values (32-bit word aligned)
// tmds_palette is a palette of TMDS symbols produced by tmds_setup_palette_symbols
// symbuf is 3*n_pix 32-bit words, this function writes the symbol values for each of the channels to it.
void __not_in_flash_func(tmds_encode_palette_data)(const uint32_t *pixbuf, const uint32_t *tmds_palette, uint32_t *symbuf, size_t n_pix, uint32_t palette_bits) {
	uint core = get_core_num();
#if !TMDS_FULLRES_NO_INTERP_SAVE
	interp_hw_save_t interp0_save, interp1_save;
	interp_save(interp0_hw, &interp0_save);
	interp_save(interp1_hw, &interp1_save);
#endif

	interp0_hw->base[2] = (uint32_t)tmds_palette;
	interp1_hw->base[2] = (uint32_t)tmds_palette;

	// Lane 0 on both interpolators masks the palette bits, starting at bit 2,
	// The second interpolator also shifts to read the 2nd or 4th byte of the word.
	interp0_hw->ctrl[0] =
		(2 << SIO_INTERP0_CTRL_LANE0_MASK_LSB_LSB) |
		((palette_bits + 1) << SIO_INTERP0_CTRL_LANE0_MASK_MSB_LSB);
	interp1_hw->ctrl[0] =
		(8 << SIO_INTERP0_CTRL_LANE0_SHIFT_LSB) |
		(2 << SIO_INTERP0_CTRL_LANE0_MASK_LSB_LSB) |
		((palette_bits + 1) << SIO_INTERP0_CTRL_LANE0_MASK_MSB_LSB);

	// Lane 1 shifts and masks the sign bit into the right position to add to the symbol
	// table index to choose the negative disparity symbols if the sign is negative.
	const uint32_t ctrl_lane_1 =
		((31 - (palette_bits + 2)) << SIO_INTERP0_CTRL_LANE0_SHIFT_LSB) |
		(palette_bits + 2) * ((1 << SIO_INTERP0_CTRL_LANE0_MASK_LSB_LSB) | (1 << SIO_INTERP0_CTRL_LANE0_MASK_MSB_LSB));
	interp0_hw->ctrl[1] = ctrl_lane_1;
	interp1_hw->ctrl[1] = ctrl_lane_1;

	if (core) {
		tmds_palette_encode_loop_x(pixbuf, symbuf, n_pix);

		interp0_hw->base[2] = (uint32_t)(tmds_palette + (2 << palette_bits));
		interp1_hw->base[2] = (uint32_t)(tmds_palette + (2 << palette_bits));
		tmds_palette_encode_loop_x(pixbuf, symbuf + (n_pix >> 1), n_pix);

		interp0_hw->base[2] = (uint32_t)(tmds_palette + (4 << palette_bits));
		interp1_hw->base[2] = (uint32_t)(tmds_palette + (4 << palette_bits));
		tmds_palette_encode_loop_x(pixbuf, symbuf + n_pix, n_pix);
	} else {
		tmds_palette_encode_loop_y(pixbuf, symbuf, n_pix);

		interp0_hw->base[2] = (uint32_t)(tmds_palette + (2 << palette_bits));
		interp1_hw->base[2] = (uint32_t)(tmds_palette + (2 << palette_bits));
		tmds_palette_encode_loop_y(pixbuf, symbuf + (n_pix >> 1), n_pix);

		interp0_hw->base[2] = (uint32_t)(tmds_palette + (4 << palette_bits));
		interp1_hw->base[2] = (uint32_t)(tmds_palette + (4 << palette_bits));
		tmds_palette_encode_loop_y(pixbuf, symbuf + n_pix, n_pix);
	}

#if !TMDS_FULLRES_NO_INTERP_SAVE
	interp_restore(interp0_hw, &interp0_save);
	interp_restore(interp1_hw, &interp1_save);
#endif
}

// Flexible 2bpp packed encoder with runtime RGB888 palette support
// Input: packed 2bpp data (4 pixels per byte, 40 bytes = 160 pixels per scanline)
// Output: RGB TMDS symbols with centered 4× horizontal scaling (160→640 pixels) + optional borders
// Works for any resolution: calculates borders automatically based on output_words
//   - 640x480: output_words=320 → 0px borders, 640 game pixels (160×4)
//   - 800x600: output_words=400 → 80px borders each side, 640 game pixels (160×4)
// With DVI_SYMBOLS_PER_WORD=2: pixels = output_words × 2
// 2bpp packed encoder for 800x600 resolution with RGB888 palette support
// Input: 40 bytes (160 GB pixels packed as 2bpp)
// Output: 800 pixels (80 black border + 640 game area + 80 black border)
// With DVI_SYMBOLS_PER_WORD=2: 800 pixels = 400 words per channel
void __not_in_flash_func(tmds_encode_2bpp_packed_gameboy)(
    const uint8_t *packed_pixbuf,    // Input: 40 bytes (160 pixels packed)
    uint32_t *symbuf_r,              // Output: Red channel TMDS symbols
    uint32_t *symbuf_g,              // Output: Green channel TMDS symbols
    uint32_t *symbuf_b,              // Output: Blue channel TMDS symbols
    size_t output_words,             // Number of output words per channel (400 words = 800 pixels)
    const uint32_t *palette_rgb888   // Palette: 4 RGB888 colors (0xRRGGBB format)
) 
{
  // Build TMDS symbol lookup tables from RGB888 palette at runtime
  // This happens once per scanline, but it's only 12 lookups total (4 colors × 3 channels)
  uint32_t tmds_palette_red[4];
  uint32_t tmds_palette_green[4];
  uint32_t tmds_palette_blue[4];
  
  for (int i = 0; i < 4; i++) {
      uint32_t color = palette_rgb888[i];
      uint8_t r8 = (color >> 16) & 0xFF;
      uint8_t g8 = (color >> 8) & 0xFF;
      uint8_t b8 = color & 0xFF;
      
      // Convert 8-bit to 6-bit indices for tmds_table lookup
      tmds_palette_red[i]   = tmds_table[r8 >> 2];
      tmds_palette_green[i] = tmds_table[g8 >> 2];
      tmds_palette_blue[i]  = tmds_table[b8 >> 2];
  }
  
  // Get black color for borders (darkest color in palette)
  uint32_t black_r = tmds_palette_red[3];
  uint32_t black_g = tmds_palette_green[3];
  uint32_t black_b = tmds_palette_blue[3];
  
  const uint8_t *src = packed_pixbuf;
  
  // Calculate horizontal layout based on output_words
  // Game area is always 640 pixels (160 GB pixels × 4) = 320 words
  const size_t game_words = 320;  // 160 pixels × 4 scaling ÷ 2 pixels per word
  
  // Calculate border width on each side (centered)
  // For 640x480: output_words=320, border=0
  // For 800x600: output_words=400, border=40 words (80 pixels)
  size_t border_words = (output_words > game_words) ? (output_words - game_words) / 2 : 0;
  
  size_t word_idx = 0;
  
  // LEFT BORDER (if any)
  for (size_t i = 0; i < border_words; i++) {
      symbuf_r[word_idx] = black_r;
      symbuf_g[word_idx] = black_g;
      symbuf_b[word_idx] = black_b;
      word_idx++;
  }
  
  // GAME AREA: 160 pixels → 640 pixels (4× scaling) = 320 words
  // Process each input byte (contains 4 packed pixels)
  // Each pixel gets replicated 4× for horizontal scaling
  for (size_t byte_idx = 0; byte_idx < 40; byte_idx++) {
      uint8_t packed_byte = src[byte_idx];
      
      // Extract and process each of the 4 pixels in this byte
      for (int pixel_in_byte = 0; pixel_in_byte < 4; pixel_in_byte++) {
          // Extract 2-bit pixel value (MSB first: bits 7-6, 5-4, 3-2, 1-0)
          uint shift = (3 - pixel_in_byte) * 2;
          uint8_t pixel_2bpp = (packed_byte >> shift) & 0x03;
          
          // Get TMDS symbol pair for this color
          uint32_t word_r = tmds_palette_red[pixel_2bpp];
          uint32_t word_g = tmds_palette_green[pixel_2bpp];
          uint32_t word_b = tmds_palette_blue[pixel_2bpp];
          
          // Replicate this pixel 4× by writing the same word twice
          // Each word contains 2 pixels, so 2 words = 4 pixels total
          symbuf_r[word_idx] = word_r;
          symbuf_g[word_idx] = word_g;
          symbuf_b[word_idx] = word_b;
          word_idx++;
          
          symbuf_r[word_idx] = word_r;
          symbuf_g[word_idx] = word_g;
          symbuf_b[word_idx] = word_b;
          word_idx++;
      }
  }
  
  // RIGHT BORDER: Fill remaining words with black
  while (word_idx < output_words) {
      symbuf_r[word_idx] = black_r;
      symbuf_g[word_idx] = black_g;
      symbuf_b[word_idx] = black_b;
      word_idx++;
  }
}
#include "osd.h"
#include "video_defs.h"
#include <string.h>
#include <stdint.h>


#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

typedef struct
{
    char ch;
    uint8_t rows[7];
} glyph_5x7_t;

#define GLYPH(_c, _r0, _r1, _r2, _r3, _r4, _r5, _r6) \
    { (_c), { (_r0), (_r1), (_r2), (_r3), (_r4), (_r5), (_r6) } }

static const glyph_5x7_t menu_font[] = {
    GLYPH(' ', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00),
    GLYPH('?', 0x0E, 0x11, 0x02, 0x04, 0x04, 0x00, 0x04),
    GLYPH('0', 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E),
    GLYPH('1', 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E),
    GLYPH('2', 0x1E, 0x01, 0x01, 0x0E, 0x10, 0x10, 0x1F),
    GLYPH('3', 0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E),
    GLYPH('4', 0x12, 0x12, 0x12, 0x1F, 0x02, 0x02, 0x02),
    GLYPH('5', 0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E),
    GLYPH('6', 0x0F, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E),
    GLYPH('7', 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    GLYPH('8', 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E),
    GLYPH('9', 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x1E),
    GLYPH('A', 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    GLYPH('B', 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E),
    GLYPH('C', 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E),
    GLYPH('D', 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C),
    GLYPH('E', 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F),
    GLYPH('F', 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10),
    GLYPH('G', 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E),
    GLYPH('H', 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11),
    GLYPH('I', 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E),
    GLYPH('J', 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E),
    GLYPH('K', 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11),
    GLYPH('L', 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F),
    GLYPH('M', 0x11, 0x1B, 0x15, 0x11, 0x11, 0x11, 0x11),
    GLYPH('N', 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11),
    GLYPH('O', 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E),
    GLYPH('P', 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10),
    GLYPH('Q', 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D),
    GLYPH('R', 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11),
    GLYPH('S', 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E),
    GLYPH('T', 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04),
    GLYPH('U', 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E),
    GLYPH('V', 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04),
    GLYPH('W', 0x11, 0x11, 0x11, 0x11, 0x15, 0x1B, 0x11),
    GLYPH('X', 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11),
    GLYPH('Y', 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04),
    GLYPH('Z', 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F),
    GLYPH('-', 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00),
    GLYPH('_', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F),
    GLYPH('.', 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C),
    GLYPH('\'', 0x06, 0x06, 0x02, 0x04, 0x00, 0x00, 0x00),
    GLYPH('=', 0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00),
    GLYPH('/', 0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00),
    GLYPH('(', 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02),
    GLYPH(')', 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08),
    GLYPH(',', 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x04, 0x08),
    GLYPH('[', 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E),
    GLYPH(']', 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E),
    GLYPH('!', 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x04),
    GLYPH(':', 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00)
};

static const glyph_5x7_t *lookup_glyph(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }

    const size_t glyph_count = ARRAY_SIZE(menu_font);
    for (size_t i = 0; i < glyph_count; ++i) {
        if (menu_font[i].ch == c) {
            return &menu_font[i];
        }
    }

    return &menu_font[1]; // '?'
}

static inline void set_pixel_2bpp(uint8_t *buf, int x, int y, uint8_t color)
{
    if ((buf == NULL) || (x < 0) || (x >= DMG_PIXELS_X) || (y < 0) || (y >= DMG_PIXELS_Y)) {
        return;
    }

    // Treat 0xFF as transparent (skip write)
    if (color == 0xFF) {
        return;
    }

    size_t idx = (size_t)y * PACKED_LINE_STRIDE_BYTES + (size_t)(x >> 2);
    uint32_t shift = (3u - (uint32_t)(x & 3)) * 2u;
    uint8_t mask = (uint8_t)(0x03u << shift);
    buf[idx] = (uint8_t)((buf[idx] & ~mask) | ((color & 0x03u) << shift));
}

static void fill_buffer_2bpp(uint8_t *buf, uint8_t color)
{
    if (buf == NULL) {
        return;
    }

    uint8_t packed = (uint8_t)(((color & 0x03u) << 6) | ((color & 0x03u) << 4) | ((color & 0x03u) << 2) | (color & 0x03u));
    memset(buf, packed, PACKED_FRAME_SIZE);
}

static void draw_glyph_5x7(uint8_t *buf, int x, int y, const glyph_5x7_t *glyph, uint8_t fg, uint8_t bg)
{
    if ((buf == NULL) || (glyph == NULL)) {
        return;
    }

    for (int row = 0; row < 7; ++row) {
        uint8_t bits = glyph->rows[row];
        for (int col = 0; col < 5; ++col) {
            const bool on = (bits & (0x10 >> col)) != 0;
            set_pixel_2bpp(buf, x + col, y + row, on ? fg : bg);
        }
        set_pixel_2bpp(buf, x + 5, y + row, bg); // 1px spacing
    }
}

static void draw_text_line(uint8_t *buf, int x, int y, const char *text, uint8_t fg, uint8_t bg)
{
    if ((buf == NULL) || (text == NULL)) {
        return;
    }

    // Render exactly OSD_MAX_CHARS glyph slots; pad with spaces
    int cursor_x = x;
    for (int i = 0; i < OSD_MAX_CHARS; ++i) {
        char c = text[i];
        if (c == '\0') c = ' ';
        const glyph_5x7_t *glyph = lookup_glyph(c);
        draw_glyph_5x7(buf, cursor_x, y, glyph, fg, bg);
        cursor_x += 6; // 5 pixels plus spacing
    }
}

static bool osd_enabled = false;
static uint16_t fb_w = DMG_PIXELS_X;
static uint16_t fb_h = DMG_PIXELS_Y;
static int osd_x = 0;
static int osd_y = 0;
static int osd_width = 0;
static int osd_height = 0;
static int osd_border = 2;
static int osd_padding = 1;
static int osd_active_line = 0;
static char osd_lines[OSD_MAX_LINES][OSD_MAX_CHARS + 1];

static int osd_visible_lines(void)
{
    int last = -1;
    for (int i = 0; i < OSD_MAX_LINES; ++i) {
        if (osd_lines[i][0] != '\0') {
            last = i;
        }
    }
    return last + 1; // 0 if nothing to show
}

// Public API
void OSD_init(uint16_t fb_width, uint16_t fb_height)
{
    fb_w = fb_width;
    fb_h = fb_height;
    osd_width  = 6 * OSD_MAX_CHARS + osd_border * 2 + osd_padding * 2;   // glyphs + spacing + border + padding
    osd_height = 0; // will be set dynamically per render
    osd_x = 0;
    osd_y = 0;
    OSD_clear();
}

void OSD_set_enabled(bool enable)
{
    osd_enabled = enable;
}

void OSD_toggle(void)
{
    osd_enabled = !osd_enabled;
}

bool OSD_is_enabled(void)
{
    return osd_enabled;
}

void OSD_set_line_text(int line, const char *text)
{
    if (line < 0 || line >= OSD_MAX_LINES || text == NULL) {
        return;
    }
    strncpy(osd_lines[line], text, OSD_MAX_CHARS);
    osd_lines[line][OSD_MAX_CHARS] = '\0';
}

void OSD_set_active_line(int line)
{
    int count = osd_visible_lines();
    if (count <= 0) {
        osd_active_line = 0;
        return;
    }
    if (line < 0) line = 0;
    if (line >= count) line = count - 1;
    osd_active_line = line;
}

void OSD_change_active_line(int delta)
{
    int count = osd_visible_lines();
    if (count <= 0) {
        osd_active_line = 0;
        return;
    }
    int next = (osd_active_line + delta) % count;
    if (next < 0) next += count;
    osd_active_line = next;
}

int OSD_get_active_line(void)
{
    return osd_active_line;
}

void OSD_clear(void)
{
    for (int i = 0; i < OSD_MAX_LINES; ++i) {
        osd_lines[i][0] = '\0';
    }
    osd_active_line = 0;
}

void OSD_render(uint8_t *packed_buf)
{
    if (!osd_enabled || packed_buf == NULL) {
        return;
    }

    int line_count = osd_visible_lines();
    if (line_count <= 0) {
        return;
    }

    if (osd_active_line >= line_count) {
        osd_active_line = line_count - 1;
    }

    osd_height = line_count * 8 + osd_border * 2 + osd_padding * 2;
    osd_x = (int)(fb_w - osd_width) / 2;
    osd_y = (int)(fb_h - osd_height) / 2;

    const uint8_t bg = 3;             // dark fill for text background
    const uint8_t border = 2;         // slightly lighter border
    const uint8_t highlight_bg = 1;   // lighter fill for active line
    const uint8_t fg = 0;             // bright text

    // Outer border box
    for (int y = 0; y < osd_height; ++y) {
        for (int x = 0; x < osd_width; ++x) {
            set_pixel_2bpp(packed_buf, osd_x + x, osd_y + y, border);
        }
    }

    // Inner fill
    int inner_w = osd_width - osd_border * 2;
    int inner_h = osd_height - osd_border * 2;
    int inner_x = osd_x + osd_border;
    int inner_y = osd_y + osd_border;
    for (int y = 0; y < inner_h; ++y) {
        for (int x = 0; x < inner_w; ++x) {
            set_pixel_2bpp(packed_buf, inner_x + x, inner_y + y, bg);
        }
    }

    // Render lines inside border; 8px stride fills inter-line gap with bg
    int text_origin_y = inner_y + osd_padding;
    int text_origin_x = inner_x + osd_padding;
    int text_width = 6 * OSD_MAX_CHARS;
    for (int i = 0; i < line_count; ++i) {
        int y = text_origin_y + i * 8; // 7px glyph height + 1px spacing
        // Highlight active line across glyph height (leave spacing as bg)
        if (i == osd_active_line) {
            for (int row = 0; row < 7; ++row) {
                for (int x = 0; x < text_width; ++x) {
                    set_pixel_2bpp(packed_buf, text_origin_x + x, y + row, highlight_bg);
                }
            }
        }
        draw_text_line(packed_buf, text_origin_x, y, osd_lines[i], fg, (i == osd_active_line) ? highlight_bg : bg);
    }
}
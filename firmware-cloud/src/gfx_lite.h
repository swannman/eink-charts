#pragma once
// Minimal text rendering into a 792x528 1-bit MSB-first framebuffer.
// Uses Adafruit's classic 5x7 fixed-space bitmap font (font5x7.h, BSD).
// Also renders Adafruit GFX proportional fonts (GFXfont struct) for titles.

#include <stdint.h>

#include <gfxfont.h>

constexpr int FB_WIDTH = 792;
constexpr int FB_HEIGHT = 528;
constexpr int FB_BYTES_PER_ROW = FB_WIDTH / 8;   // 99
constexpr int FB_GLYPH_W = 5;                    // font glyph columns
constexpr int FB_GLYPH_H = 7;                    // font glyph rows (in 8-row byte)

void fbClear(uint8_t* fb, bool white);
void fbSetPixel(uint8_t* fb, int x, int y, bool black);
void fbFillRect(uint8_t* fb, int x, int y, int w, int h, bool black);
int fbCharCellW(int scale);
int fbCharCellH(int scale);
int fbStringWidth(const char* s, int scale);
void fbDrawChar(uint8_t* fb, int x, int y, int scale, char c, bool black);
void fbDrawString(uint8_t* fb, int x, int y, int scale, const char* s, bool black);
void fbDrawStringCentered(uint8_t* fb, int y, int scale, const char* s, bool black);

// Adafruit GFXfont rendering. (x, y) is the BASELINE of the first glyph (per
// the GFX convention), unlike the 5x7 helpers which take the top-left.
int fbGfxStringWidth(const GFXfont* font, const char* s);
int fbGfxStringWidthScaled(const GFXfont* font, const char* s, int scale);
int fbGfxLineHeight(const GFXfont* font);
void fbDrawStringGfx(uint8_t* fb, int x, int y, const GFXfont* font, const char* s, bool black);
// Integer-scaled GFX text — each glyph pixel becomes a scale×scale block.
// Looks blocky for scale>1 (vector fonts are pre-rasterized) but lets us
// reuse the existing 18pt face for huge stat values.
void fbDrawStringGfxScaled(uint8_t* fb, int x, int y, const GFXfont* font, const char* s, int scale, bool black);

// GFX equivalents of fbDrawStringCentered. `y` is the BASELINE (GFX
// convention), x is computed so the string is horizontally centered.
void fbDrawStringGfxCentered(uint8_t* fb, int y, const GFXfont* font, const char* s, bool black);
void fbDrawStringGfxScaledCentered(uint8_t* fb, int y, const GFXfont* font, const char* s, int scale, bool black);

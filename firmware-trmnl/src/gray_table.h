#pragma once
#include <stdint.h>

// TRMNL X 16-level (4bpp) grayscale waveform matrix, copied verbatim from the
// official usetrmnl/trmnl-firmware src/display.cpp (the `u8_graytable`). This
// is the vendor-calibrated waveform for this exact panel — fed to FastEPD via
// setCustomMatrix() it gives noticeably cleaner 16-gray output than FastEPD's
// stock matrix. 16 rows x 9 values = 144 bytes.
static const uint8_t TRMNL_X_GRAY_MATRIX[] = {
    /*  0 */ 0, 0, 0, 0, 0, 0, 1, 1, 1,
    /*  1 */ 0, 0, 1, 1, 1, 2, 2, 1, 1,
    /*  2 */ 0, 0, 0, 0, 1, 2, 2, 1, 1,
    /*  3 */ 1, 1, 2, 2, 1, 1, 1, 1, 2,
    /*  4 */ 0, 0, 0, 1, 2, 1, 1, 1, 2,
    /*  5 */ 1, 2, 2, 2, 2, 1, 1, 1, 2,
    /*  6 */ 0, 0, 1, 1, 2, 2, 1, 1, 2,
    /*  7 */ 0, 1, 1, 2, 1, 1, 2, 1, 2,
    /*  8 */ 0, 1, 1, 1, 2, 1, 2, 1, 2,
    /*  9 */ 0, 1, 1, 1, 1, 2, 2, 1, 2,
    /* 10 */ 1, 1, 1, 2, 1, 1, 1, 2, 2,
    /* 11 */ 0, 0, 1, 2, 1, 1, 1, 2, 2,
    /* 12 */ 0, 0, 0, 1, 2, 1, 1, 2, 2,
    /* 13 */ 0, 0, 0, 0, 1, 2, 1, 2, 2,
    /* 14 */ 0, 1, 1, 1, 2, 2, 2, 2, 2,
    /* 15 */ 0, 0, 0, 0, 0, 0, 0, 0, 2,
};

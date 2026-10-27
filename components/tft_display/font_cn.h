#ifndef FONT_CN_H
#define FONT_CN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CN_FONT_W 16
#define CN_FONT_H 16
#define CN_FONT_BYTES_PER_GLYPH 32

typedef struct {
    uint16_t codepoint;
    uint8_t bits[CN_FONT_BYTES_PER_GLYPH];
} cn_glyph_t;

extern const cn_glyph_t g_cn_glyphs[];
extern const size_t g_cn_glyph_count;

#ifdef __cplusplus
}
#endif

#endif /* FONT_CN_H */

// main/card_model.c -- small pure helpers; no ESP-IDF or LVGL dependencies.
#include "card_model.h"

#include <stdio.h>
#include <string.h>

void card_utf8_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
        // Step back over continuation bytes (10xxxxxx) so the cut lands on a
        // character boundary, then drop the lead byte that lost its tail.
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void card_format_money(char *dst, size_t dst_size, const char *sign, int cents)
{
    if (cents < 0) cents = 0;
    snprintf(dst, dst_size, "%s$%d.%02d", sign ? sign : "", cents / 100, cents % 100);
}

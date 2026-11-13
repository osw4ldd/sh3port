#ifndef PS2RT_MPEG_NATIVE_H
#define PS2RT_MPEG_NATIVE_H
#include <stddef.h>
#include <stdint.h>

typedef struct mpeg_native mpeg_native;

mpeg_native *mpn_create(void);
void mpn_destroy(mpeg_native *m);
size_t mpn_feed(mpeg_native *m, const uint8_t *data, size_t size);
void mpn_flush(mpeg_native *m);
int mpn_has_picture(mpeg_native *m);
int mpn_take_raw8(mpeg_native *m, uint8_t *out, int max_mbs, int *width, int *height);
void mpn_reset(mpeg_native *m);
int mpn_last_picture_type(mpeg_native *m);   /* 1 I, 2 P, 3 B */

#endif

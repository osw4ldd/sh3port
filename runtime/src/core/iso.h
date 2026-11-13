#ifndef PS2RT_ISO_H
#define PS2RT_ISO_H
#include <stdint.h>

typedef struct iso_entry {
    uint32_t lsn, size;
    int is_dir;
    uint8_t date[7];   /* ISO9660 recording date: years since 1900, month, day, h, m, s, gmt offset */
    char name[64];
} iso_entry;

int iso_open(const char *path);
int iso_is_open(void);
int iso_read(uint32_t lsn, uint32_t count, void *dst);
int iso_read_bytes(uint64_t offset, uint32_t size, void *dst);
/* path: "cdrom0:\\DIR\\FILE.EXT;1", "\\DIR\\FILE.EXT" ... (case-insensitive) */
int iso_find(const char *path, iso_entry *out);

#endif

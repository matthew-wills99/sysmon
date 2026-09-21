#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

typedef struct {
    char *label;
    char *value;
} kv_pair;

typedef struct {
    char unit[2];   /* single-letter unit + null terminator, e.g. "K", "M", "G" */
    float value;
} SizeInfo;


/* Converts a byte count to the largest sensible unit ("B", "K", "M", "G", "T"). */
SizeInfo size_from_bytes(double bytes);

/* ---- File / string helpers (utils.c) ---- */

void   free_kv_pairs(kv_pair *entries, size_t count);
char  *trim(char *s);
int    is_included(const char *label, const char **include, size_t include_count);

/* Reads a whole file into a malloc'd, null-terminated buffer. Caller frees. */
char  *read_info_file(const char *path, size_t *out_len);

/* Parses "label : value" lines, keeping only labels listed in `include`.
   Returns the pair count (free with free_kv_pairs) or (size_t)-1 on failure. */
size_t parse_kv_file(const char *path, const char **include, size_t include_count,
                     kv_pair **out_entries);

#endif /* UTILS_H */
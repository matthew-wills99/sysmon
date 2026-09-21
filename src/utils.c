#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

void free_kv_pairs(kv_pair *entries, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(entries[i].label);
        free(entries[i].value);
    }
    free(entries);
}

char *trim(char *s) {
    /* skip leading whitespace */
    while (*s == ' ' || *s == '\t') s++;

    if (*s == '\0') return s;   /* all whitespace / empty */

    /* find end of string, then walk back over trailing whitespace */
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        *end = '\0';
        end--;
    }
    return s;
}

int is_included(const char *label, const char **include, size_t include_count) {
    for (size_t i = 0; i < include_count; i++) {
        if (strcmp(label, include[i]) == 0) return 1;
    }
    return 0;
}

char *read_info_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);

    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) {
                free(buf);
                fclose(f);
                return NULL;
            }
            buf = tmp;
        }
    }

    fclose(f);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Parses "label : value" lines, keeping only labels found in `include`.
   On success returns the number of pairs and sets *out_entries (free it with
   free_kv_pairs). Returns (size_t)-1 on failure. */
size_t parse_kv_file(const char *path, const char **include, size_t include_count,
                     kv_pair **out_entries) {
    size_t file_len;
    char *data = read_info_file(path, &file_len);
    if (!data) return (size_t)-1;

    size_t cap = 32;
    size_t count = 0;
    kv_pair *entries = malloc(cap * sizeof(kv_pair));
    if (!entries) {
        free(data);
        return (size_t)-1;
    }

    char *line = strtok(data, "\n");
    while (line != NULL) {
        char *colon = strchr(line, ':');

        if (colon != NULL) {
            *colon = '\0';
            char *label = trim(line);
            char *value = trim(colon + 1);

            if (label[0] != '\0' && is_included(label, include, include_count)) {
                if (count == cap) {
                    cap *= 2;
                    kv_pair *tmp = realloc(entries, cap * sizeof(kv_pair));
                    if (!tmp) {
                        free_kv_pairs(entries, count);
                        free(data);
                        return (size_t)-1;
                    }
                    entries = tmp;
                }

                char *l = strdup(label);
                char *v = strdup(value);
                if (!l || !v) {
                    free(l);
                    free(v);
                    free_kv_pairs(entries, count);
                    free(data);
                    return (size_t)-1;
                }
                entries[count].label = l;
                entries[count].value = v;
                count++;
            }
        }

        line = strtok(NULL, "\n");
    }

    free(data);
    *out_entries = entries;
    return count;
}

SizeInfo size_from_bytes(double bytes) {
    static const char units[] = "BKMGT";
    size_t i = 0;

    while (bytes >= 1024.0 && i < sizeof units - 2) {
        bytes /= 1024.0;
        i++;
    }

    SizeInfo s;
    s.unit[0] = units[i];
    s.unit[1] = '\0';
    s.value = (float)bytes;
    return s;
}
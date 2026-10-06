/* allocators, string buffer, file helpers, hash map */
#define _GNU_SOURCE
#include "rmmgen.h"
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "rmm-gen: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}
char *xstrdup(const char *s) {
    char *d = strdup(s ? s : "");
    if (!d) die("out of memory");
    return d;
}

void sb_init(SBuf *b) { b->s = xmalloc(256); b->s[0] = 0; b->len = 0; b->cap = 256; }
void sb_free(SBuf *b) { free(b->s); b->s = NULL; b->len = b->cap = 0; }
void sb_reserve(SBuf *b, size_t need) {
    if (b->len + need + 1 > b->cap) {
        while (b->len + need + 1 > b->cap) b->cap *= 2;
        b->s = xrealloc(b->s, b->cap);
    }
}
void sb_putc(SBuf *b, char c) {
    sb_reserve(b, 1);
    b->s[b->len++] = c; b->s[b->len] = 0;
}
void sb_puts(SBuf *b, const char *s) {
    size_t n = strlen(s);
    sb_reserve(b, n);
    memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0;
}
void sb_add(SBuf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) {
        char *big = xmalloc((size_t)n + 1);
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        sb_puts(b, big);
        free(big);
        return;
    }
    sb_puts(b, tmp);
}

char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)sz + 1);
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz)
        die("short read on %s", path);
    buf[sz] = 0;
    fclose(f);
    if (len_out) *len_out = (size_t)sz;
    return buf;
}

void write_file(const char *path, const char *data, size_t len) {
    mkdirs_for(path);
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    if (len && fwrite(data, 1, len, f) != len)
        die("short write on %s", path);
    fclose(f);
}

void write_sbuf(const char *path, SBuf *b) { write_file(path, b->s, b->len); }

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

void mkdirs_for(const char *path) {
    char tmp[1024];
    size_t n = strlen(path);
    if (n >= sizeof tmp) die("path too long: %s", path);
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

void gen_path2(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

static uint64_t fnv(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

SMap *smap_new(void) {
    SMap *m = xmalloc(sizeof *m);
    m->cap = 64;
    m->tab = xmalloc(m->cap * sizeof *m->tab);
    m->order = xmalloc(m->cap * 2 * sizeof *m->order);
    m->cnt = m->ocnt = 0;
    return m;
}
static void smap_grow(SMap *m) {
    size_t ncap = m->cap * 2;
    SMapEnt *nt = xmalloc(ncap * sizeof *nt);
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->tab[i].used) continue;
        size_t j = fnv(m->tab[i].key) & (ncap - 1);
        while (nt[j].used) j = (j + 1) & (ncap - 1);
        nt[j] = m->tab[i];
    }
    free(m->tab);
    m->tab = nt; m->cap = ncap;
    free(m->order);
    m->order = xmalloc(m->cap * 2 * sizeof *m->order);
    for (size_t j = 0; j < m->cap; j++) {
        if (!m->tab[j].used) continue;
        m->order[m->tab[j].seq] = (int)j;
    }
}
void *smap_get(SMap *m, const char *key) {
    size_t j = fnv(key) & (m->cap - 1);
    while (m->tab[j].used) {
        if (strcmp(m->tab[j].key, key) == 0) return m->tab[j].val;
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}
int smap_has(SMap *m, const char *key) { return smap_get(m, key) != NULL; }
void smap_put(SMap *m, const char *key, void *val) {
    if (m->cnt * 2 >= m->cap) smap_grow(m);
    size_t j = fnv(key) & (m->cap - 1);
    while (m->tab[j].used) {
        if (strcmp(m->tab[j].key, key) == 0) { m->tab[j].val = val; return; }
        j = (j + 1) & (m->cap - 1);
    }
    m->tab[j].used = 1;
    m->tab[j].key = xstrdup(key);
    m->tab[j].val = val;
    m->tab[j].seq = (int)m->ocnt;
    m->cnt++;
    m->order[m->ocnt++] = (int)j;
}
int smap_count(SMap *m) { return (int)m->ocnt; }
const char *smap_next(SMap *m, int *it, void **val) {
    if (*it >= (int)m->ocnt) return NULL;
    SMapEnt *e = &m->tab[m->order[*it]];
    if (val) *val = e->val;
    (*it)++;
    return e->key;
}

static int cmp_keys(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
const char **smap_sorted(SMap *m, int *n_out) {
    int n = smap_count(m);
    const char **keys = xmalloc((size_t)(n ? n : 1) * sizeof *keys);
    int it = 0; int i = 0;
    while (smap_next(m, &it, NULL)) keys[i++] = m->tab[m->order[it - 1]].key;
    qsort(keys, (size_t)n, sizeof *keys, cmp_keys);
    *n_out = n;
    return keys;
}

SSet *sset_new(void) { return smap_new(); }
int sset_add(SSet *s, const char *key) {
    if (smap_has(s, key)) return 0;
    smap_put(s, key, (void *)1);
    return 1;
}
int sset_has(SSet *s, const char *key) { return smap_has(s, key); }
int sset_count(SSet *s) { return smap_count(s); }

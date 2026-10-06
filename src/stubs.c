/* deterministic trampoline table + persistence */
#define _GNU_SOURCE
#include "rmmgen.h"

void stubs_add(StubTab *t, const char *mod, const char *sym, uint64_t addr) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->v = xrealloc(t->v, (size_t)t->cap * sizeof *t->v);
    }
    Stub *st = &t->v[t->n++];
    snprintf(st->mod, sizeof st->mod, "%s", mod);
    snprintf(st->sym, sizeof st->sym, "%s", sym);
    st->addr = addr;
    st->idx = t->n - 1;
}
Stub *stubs_find(StubTab *t, const char *mod, const char *sym) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->v[i].mod, mod) == 0 && strcmp(t->v[i].sym, sym) == 0)
            return &t->v[i];
    return NULL;
}
Stub *stubs_by_sym(StubTab *t, const char *sym) {

    for (int i = 0; i < t->n; i++) {
        char key[384];
        snprintf(key, sizeof key, "%s::%s", t->v[i].mod, t->v[i].sym);
        if (strcmp(key, sym) == 0) return &t->v[i];
    }
    return NULL;
}
void stubs_save(const StubTab *t, const char *unused) {
    (void)unused;
    SBuf b; sb_init(&b);
    for (int i = 0; i < t->n; i++)
        sb_add(&b, "%s %s 0x%llX\n", t->v[i].mod, t->v[i].sym,
               (unsigned long long)t->v[i].addr);
    char p[1200];
    gen_path2(p, sizeof p, "%s/stubs.txt", g_gen);
    write_sbuf(p, &b);
    sb_free(&b);
}
void stubs_load(StubTab *t, const char *unused) {
    (void)unused;
    char p[1200];
    gen_path2(p, sizeof p, "%s/stubs.txt", g_gen);
    size_t len;
    char *txt = read_file(p, &len);
    char *save = NULL;
    for (char *ln = strtok_r(txt, "\n", &save); ln;
         ln = strtok_r(NULL, "\n", &save)) {
        char mod[128], sym[256], addr[64];
        if (sscanf(ln, "%127s %255s %63s", mod, sym, addr) == 3)
            stubs_add(t, mod, sym, strtoull(addr, NULL, 0));
    }
    free(txt);
}

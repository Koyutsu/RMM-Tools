/* linker/modules.map parser + validation */
#include "rmmgen.h"
#include <ctype.h>

const char *g_modnames[] = { "drv_bus", "drv_gfx", "drv_input", "fs", "sched", "net", NULL };

ModDef *manifest_mod(Manifest *mf, const char *name) {
    for (int i = 0; i < mf->nmods; i++)
        if (strcmp(mf->mods[i].name, name) == 0) return &mf->mods[i];
    return NULL;
}

int manifest_class_id(const char *klass) {
    if (!klass || !*klass) return 1;
    if (strcmp(klass, "CORE") == 0)    return 0;
    if (strcmp(klass, "TRUSTED") == 0) return 1;
    if (strcmp(klass, "SFI") == 0)     return 2;
    if (strcmp(klass, "DRIVER") == 0)  return 3;
    die("unknown module class '%s'", klass);
    return 1;
}

int manifest_tramp(Manifest *mf, uint64_t *base, uint64_t *size) {
    for (int i = 0; i < mf->nregions; i++)
        if (strcmp(mf->regions[i].name, "tramp") == 0) {
            *base = mf->regions[i].base;
            *size = mf->regions[i].size;
            return 1;
        }
    return 0;
}

typedef struct { const char *p, *end; } Cur;

static void skip_ws(Cur *c) {
    while (c->p < c->end) {
        if (*c->p == '#') {
            while (c->p < c->end && *c->p != '\n') c->p++;
        } else if (isspace((unsigned char)*c->p)) {
            c->p++;
        } else {
            break;
        }
    }
}

static int read_word(Cur *c, char *out, size_t cap) {
    skip_ws(c);
    size_t n = 0;
    while (c->p < c->end && (isalnum((unsigned char)*c->p) || *c->p == '_')) {
        if (n + 1 >= cap) die("identifier too long in modules.map");
        out[n++] = *c->p++;
    }
    out[n] = 0;
    return n > 0;
}
static uint64_t parse_u64(const char *v) {
    return strtoull(v, NULL, 0);

static void parse_assignments(Cur *c, const char *body_end,
                              void (*cb)(void *ctx, const char *k, const char *v),
                              void *ctx) {
    char key[128], val[512];
    while (c->p < body_end) {
        skip_ws(c);
        if (c->p >= body_end) break;
        if (*c->p == ';') { c->p++; continue; }
        size_t kn = 0;
        while (c->p < body_end && (isalnum((unsigned char)*c->p) || *c->p == '_')) {
            if (kn + 1 < sizeof key) key[kn++] = *c->p;
            c->p++;
        }
        key[kn] = 0;
        if (!kn) { c->p++; continue; }
        skip_ws(c);
        if (c->p < body_end && *c->p == '=') c->p++;
        skip_ws(c);
        size_t vn = 0;
        while (c->p < body_end && *c->p != ';') {
            if (vn + 1 < sizeof val) val[vn++] = *c->p;
            c->p++;
        }
        if (c->p < body_end && *c->p == ';') c->p++;

        while (vn && isspace((unsigned char)val[vn - 1])) vn--;
        val[vn] = 0;
        cb(ctx, key, val);
    }
}

typedef struct { ModDef *mod; RegionDef *reg; int is_region; } BlockCtx;

static void block_cb(void *ctxp, const char *k, const char *v) {
    BlockCtx *bc = ctxp;
    if (bc->is_region) {
        RegionDef *r = bc->reg;
        if (strcmp(k, "base") == 0) r->base = parse_u64(v);
        else if (strcmp(k, "size") == 0) r->size = parse_u64(v);
        return;
    }
    ModDef *m = bc->mod;
    if (strcmp(k, "base") == 0) {
        m->base = parse_u64(v);
    } else if (strcmp(k, "size") == 0) {
        m->size = parse_u64(v);
    } else if (strcmp(k, "image") == 0) {
        const char *q = v;
        if (*q == '"') q++;
        snprintf(m->image, sizeof m->image, "%s", q);
        size_t n = strlen(m->image);
        if (n && m->image[n - 1] == '"') m->image[n - 1] = 0;
    } else if (strcmp(k, "class") == 0) {
        snprintf(m->klass, sizeof m->klass, "%s", v);
    } else if (strcmp(k, "type") == 0) {
        snprintf(m->type, sizeof m->type, "%s", v);
        m->resident_flag = (strcmp(v, "resident") == 0);
    } else if (strcmp(k, "replaceable") == 0) {
        m->replaceable = (strcmp(v, "true") == 0);
    } else if (strcmp(k, "optional") == 0) {
        m->optional = (strcmp(v, "true") == 0);
    } else if (strcmp(k, "deps") == 0) {

        const char *p = v;
        m->ndeps = 0;
        while (*p) {
            while (*p && !isalnum((unsigned char)*p) && *p != '_') p++;
            if (!*p) break;
            const char *s = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            size_t n = (size_t)(p - s);
            if (m->ndeps >= 16) die("module %s: too many deps", m->name);
            if (n >= sizeof m->deps[0]) die("dep name too long");
            memcpy(m->deps[m->ndeps++], s, n);
            m->deps[m->ndeps - 1][n] = 0;
        }
    }
}


static int  g_cycle;
static char g_cycle_node[64];

static void dag_visit(Manifest *mf, const char *name, SSet *perm, SSet *temp) {
    if (strcmp(name, "core") == 0) return;
    if (sset_has(perm, name)) return;
    if (sset_has(temp, name)) {
        g_cycle = 1;
        snprintf(g_cycle_node, sizeof g_cycle_node, "%s", name);
        return;
    }
    sset_add(temp, name);
    ModDef *m = manifest_mod(mf, name);
    if (!m) return;
    for (int i = 0; i < m->ndeps; i++) {
        dag_visit(mf, m->deps[i], perm, temp);
        if (g_cycle) return;
    }
    sset_add(perm, name);
}

static void topo_visit(Manifest *mf, const char *name, SSet *stack,
                       char seen[32][64], int *nseen) {
    for (int i = 0; i < *nseen; i++)
        if (strcmp(seen[i], name) == 0) return;
    if (sset_has(stack, name)) {
        g_cycle = 1;
        snprintf(g_cycle_node, sizeof g_cycle_node, "%s", name);
        return;
    }
    sset_add(stack, name);
    ModDef *m = manifest_mod(mf, name);
    if (m)
        for (int i = 0; i < m->ndeps; i++) {
            if (strcmp(m->deps[i], "core") == 0) continue;
            topo_visit(mf, m->deps[i], stack, seen, nseen);
        }
    if (strcmp(name, "core") != 0) {
        snprintf(seen[*nseen], 64, "%s", name);
        (*nseen)++;
    }
}

void manifest_parse(Manifest *mf) {
    static char map_path[1024];
    snprintf(map_path, sizeof map_path, "%s/linker/modules.map", g_kdir);
    memset(mf, 0, sizeof *mf);
    size_t len;
    char *text = read_file(map_path, &len);

    mf->abi_version = 1;
    snprintf(mf->kernel_arch, sizeof mf->kernel_arch, "x86_64");

    Cur c = { text, text + len };
    while (c.p < c.end) {
        skip_ws(&c);
        if (c.p >= c.end) break;
        if (*c.p == '#') { while (c.p < c.end && *c.p != '\n') c.p++; continue; }

        char w[128];
        if (*c.p == ';') { c.p++; continue; }

        if (!read_word(&c, w, sizeof w)) { c.p++; continue; }

        if (strcmp(w, "module") == 0 || strcmp(w, "region") == 0) {
            char name[64];
            if (!read_word(&c, name, sizeof name))
                die("modules.map: %s missing name", w);
            skip_ws(&c);
            if (c.p >= c.end || *c.p != '{') die("modules.map: expected '{' after %s %s", w, name);
            c.p++;
            const char *body_start = c.p;
            int depth = 1;
            while (c.p < c.end && depth > 0) {
                if (*c.p == '{') depth++;
                else if (*c.p == '}') depth--;
                c.p++;
            }
            if (depth) die("modules.map: unterminated block for %s", name);
            const char *body_end = c.p - 1;

            if (strcmp(w, "module") == 0) {
                if (mf->nmods >= 32) die("too many modules");
                ModDef *m = &mf->mods[mf->nmods++];
                snprintf(m->name, sizeof m->name, "%s", name);
                snprintf(m->image, sizeof m->image, "%s.rmm", name);
                snprintf(m->klass, sizeof m->klass, "TRUSTED");
                snprintf(m->type, sizeof m->type, "demand");
                BlockCtx bc = { m, NULL, 0 };
                Cur bc2 = { body_start, body_end };
                parse_assignments(&bc2, body_end, block_cb, &bc);
                (void)bc2;
            } else {
                if (mf->nregions >= 8) die("too many regions");
                RegionDef *r = &mf->regions[mf->nregions++];
                snprintf(r->name, sizeof r->name, "%s", name);
                BlockCtx bc = { NULL, r, 1 };
                Cur rc = { body_start, body_end };
                parse_assignments(&rc, body_end, block_cb, &bc);
            }
        } else if (strcmp(w, "kernel_arch") == 0) {
            char v[64];
            if (read_word(&c, v, sizeof v)) { v[sizeof mf->kernel_arch - 1] = 0; snprintf(mf->kernel_arch, sizeof mf->kernel_arch, "%s", v); }
        } else if (strcmp(w, "abi_version") == 0) {
            char v[64];
            if (read_word(&c, v, sizeof v)) mf->abi_version = (int)parse_u64(v);
        } else {

            while (c.p < c.end && *c.p != '\n') c.p++;
        }
    }
    free(text);

    typedef struct { uint64_t lo, hi; const char *name; } Slot;
    Slot slots[32];
    int nslots = 0;
    for (int i = 0; i < mf->nmods; i++) {
        ModDef *m = &mf->mods[i];
        if (m->size == 0) die("module %s: bad size", m->name);
        if (m->size & (m->size - 1))
            die("module %s: size 0x%llX is not a power of two (§4)",
                m->name, (unsigned long long)m->size);
        if (m->base % m->size)
            die("module %s: base 0x%llX %% size 0x%llX != 0 (§4)",
                m->name, (unsigned long long)m->base, (unsigned long long)m->size);
        m->mask = m->size - 1;
        slots[nslots].lo = m->base;
        slots[nslots].hi = m->base + m->size;
        slots[nslots].name = m->name;
        nslots++;
    }

    for (int i = 1; i < nslots; i++) {
        Slot t = slots[i];
        int j = i - 1;
        while (j >= 0 && slots[j].lo > t.lo) { slots[j + 1] = slots[j]; j--; }
        slots[j + 1] = t;
    }
    for (int i = 1; i < nslots; i++)
        if (slots[i].lo < slots[i - 1].hi)
            die("slot overlap: %s and %s (§94)", slots[i - 1].name, slots[i].name);

    for (int i = 0; i < mf->nregions; i++) {
        RegionDef *r = &mf->regions[i];
        if (r->size == 0) die("region %s: bad size", r->name);
        for (int j = 0; j < nslots; j++) {
            int inside = r->base >= slots[j].lo && (r->base + r->size) <= slots[j].hi;
            if (!inside && r->base < slots[j].hi && slots[j].lo < r->base + r->size)
                die("region %s overlaps module slot %s", r->name, slots[j].name);
        }
    }

    int mid = 1;
    for (int i = 0; i < mf->nmods; i++) {
        if (strcmp(mf->mods[i].name, "core") == 0) mf->mods[i].id = 0;
        else mf->mods[i].id = mid++;
    }

    for (int i = 0; i < mf->nmods; i++)
        for (int d = 0; d < mf->mods[i].ndeps; d++)
            if (!manifest_mod(mf, mf->mods[i].deps[d]))
                die("module %s: unknown dependency %s", mf->mods[i].name, mf->mods[i].deps[d]);

    g_cycle = 0;
    SSet *perm = sset_new(), *temp = sset_new();
    for (int i = 0; i < mf->nmods; i++) {
        dag_visit(mf, mf->mods[i].name, perm, temp);
        if (g_cycle) die("dependency cycle through %s (§48)", g_cycle_node);
    }

    SSet *stack = sset_new();
    char seen[32][64];
    int nseen = 0;
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < mf->nmods; i++) {
        topo_visit(mf, mf->mods[i].name, stack, seen, &nseen);
        if (g_cycle) die("dependency cycle through %s (§48)", g_cycle_node);
    }
    mf->nlink_order = nseen;
    memcpy(mf->link_order, seen, sizeof seen);
}

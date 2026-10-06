/* human-facing JSON reports (manifest.json, module_report.json) */
#define _GNU_SOURCE
#include "rmmgen.h"

void json_escape(SBuf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  sb_puts(b, "\\\""); break;
        case '\\': sb_puts(b, "\\\\"); break;
        case '\n': sb_puts(b, "\\n");  break;
        case '\r': sb_puts(b, "\\r");  break;
        case '\t': sb_puts(b, "\\t");  break;
        default:
            if (c < 0x20) sb_add(b, "\\u%04x", c);
            else sb_putc(b, (char)c);
        }
    }
}

/* machine dump of the parsed modules.map */
void write_manifest_json(Manifest *mf) {
    SBuf j; sb_init(&j);
    sb_puts(&j, "{\n");
    sb_puts(&j, " \"kernel_arch\": \"");
    json_escape(&j, mf->kernel_arch);
    sb_puts(&j, "\",\n");
    sb_add(&j, " \"abi_version\": %d,\n", mf->abi_version);
    sb_puts(&j, " \"modules\": {\n");
    for (int i = 0; i < mf->nmods; i++) {
        ModDef *m = &mf->mods[i];
        sb_add(&j, "  \"%s\": {\n", m->name);
        sb_add(&j, "   \"id\": %d,\n", m->id);
        sb_add(&j, "   \"base\": %llu,\n", (unsigned long long)m->base);
        sb_add(&j, "   \"size\": %llu,\n", (unsigned long long)m->size);
        sb_add(&j, "   \"mask\": %llu,\n", (unsigned long long)m->mask);
        sb_puts(&j, "   \"image\": \"");
        json_escape(&j, m->image);
        sb_puts(&j, "\",\n");
        if (m->ndeps) {
            sb_puts(&j, "   \"deps\": [\n");
            for (int d = 0; d < m->ndeps; d++) {
                sb_add(&j, "    \"%s\"", m->deps[d]);
                sb_puts(&j, d + 1 < m->ndeps ? ",\n" : "\n   ],\n");
            }
        } else {
            sb_puts(&j, "   \"deps\": [],\n");
        }
        sb_puts(&j, "   \"class\": \"");
        json_escape(&j, m->klass);
        sb_puts(&j, "\",\n");
        sb_puts(&j, "   \"type\": \"");
        json_escape(&j, m->type);
        sb_puts(&j, "\",\n");
        sb_add(&j, "   \"replaceable\": %s,\n", m->replaceable ? "true" : "false");
        sb_add(&j, "   \"optional\": %s\n", m->optional ? "true" : "false");
        sb_puts(&j, i + 1 < mf->nmods ? "  },\n" : "  }\n");
    }
    sb_puts(&j, " },\n");
    sb_puts(&j, " \"regions\": {\n");
    for (int i = 0; i < mf->nregions; i++) {
        RegionDef *r = &mf->regions[i];
        sb_add(&j, "  \"%s\": {\n   \"base\": %llu,\n   \"size\": %llu\n  }%s\n",
               r->name, (unsigned long long)r->base,
               (unsigned long long)r->size,
               i + 1 < mf->nregions ? "," : "");
    }
    sb_puts(&j, " }\n}\n");
    char p[1200];
    gen_path2(p, sizeof p, "%s/manifest.json", g_gen);
    write_sbuf(p, &j);
    sb_free(&j);
}


void write_module_report_json(int nmods, char (*modnames)[64], SBuf *per_mod,
                              long archive_bytes, SBuf *validator_json) {
    SBuf j; sb_init(&j);
    sb_puts(&j, "{\n \"modules\": {\n");
    char recs[32][256];
    int nrec = 0;
    {
        const char *p = per_mod->s;
        while (*p && nrec < 32) {
            const char *nl = strchr(p, '\n');
            char tmp[256];
            const char *end = nl ? nl : p + strlen(p);
            size_t n = (size_t)(end - p);
            if (n >= sizeof tmp) n = sizeof tmp - 1;
            memcpy(tmp, p, n); tmp[n] = 0;
            if (*tmp) snprintf(recs[nrec++], 256, "%s", tmp);
            p = end;
            while (*p == '\n') p++;
        }
    }
    for (int r = 0; r < nrec; r++) {
        unsigned long img, code; int exports; char name[64];
        if (sscanf(recs[r], "%lu:%lu:%d:%63s", &img, &code, &exports, name) != 4)
            continue;
        (void)modnames;
        sb_add(&j, "  \"%s\": {\n", name);
        sb_add(&j, "   \"image_bytes\": %lu,\n", img);
        sb_add(&j, "   \"code_bytes\": %lu,\n", code);
        sb_add(&j, "   \"exports\": %d\n", exports);
        sb_puts(&j, r + 1 < nrec ? "  },\n" : "  }\n");
    }
    sb_puts(&j, " },\n");
    sb_add(&j, " \"archive_bytes\": %ld,\n", archive_bytes);
    sb_puts(&j, " \"validator\": ");
    sb_add_sbuf(&j, validator_json);
    sb_puts(&j, "\n}\n");
    char p[1200];
    gen_path2(p, sizeof p, "%s/module_report.json", g_gen);
    write_sbuf(p, &j);
    sb_free(&j);
}

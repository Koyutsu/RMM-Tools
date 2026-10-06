/* build phases:
 * offsets, manifest, prelink, core-syms, module, final, check-stubs. */
#define _GNU_SOURCE
#include "rmmgen.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static void upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

static void emit_generated_h(Manifest *mf, StubTab *stubs) {
    SBuf h; sb_init(&h);
    sb_puts(&h, "/* generated from modules.map DO NOT EDIT*/\n"
                "#pragma once\n#include <rmm/rmm.h>\n#include <rmm/module.h>\n\n");
    for (int i = 0; i < mf->nmods; i++) {
        ModDef *m = &mf->mods[i];
        char u[96]; snprintf(u, sizeof u, "%s", m->name); upper(u);
        sb_add(&h, "#define RMM_%s_BASE   0x%016llXull\n", u,
               (unsigned long long)m->base);
        sb_add(&h, "#define RMM_%s_SIZE   0x%llXull\n", u,
               (unsigned long long)m->size);
        sb_add(&h, "#define RMM_%s_MASK   0x%llXull\n", u,
               (unsigned long long)m->mask);
        sb_add(&h, "#define RMM_MODID_%s  %du\n", u, m->id);
    }
    for (int i = 0; i < stubs->n; i++) {
        Stub *st = &stubs->v[i];
        sb_add(&h, "#define RMM_STUB_%s_%s  0x%llXull\n", st->mod, st->sym,
               (unsigned long long)st->addr);
    }
    sb_puts(&h, "\nstruct rmm_module_descriptor;\n");
    sb_add(&h, "#define RMM_MODULE_COUNT %du\n", mf->nmods);
    sb_puts(&h, "extern struct rmm_module_descriptor *const rmm_module_table[RMM_MODULE_COUNT];\n\n");
    for (int i = 0; i < mf->nmods; i++)
        if (mf->mods[i].ndeps)
            sb_add(&h, "extern const uint32_t rmm_dep_%s[%d];\n",
                   mf->mods[i].name, mf->mods[i].ndeps);
    char p[1200];
    gen_path2(p, sizeof p, "%s/rmm/modules.generated.h", g_gen);
    write_sbuf(p, &h);
    sb_free(&h);
}

static void emit_generated_c(Manifest *mf) {
    SBuf c; sb_init(&c);
    sb_puts(&c, "/* generated from modules.map, DO NOT EDIT */\n"
                "#include \"rmm/modules.generated.h\"\n\n"
                "#define RMM_STATE_UNLOADED 0\n\n");
    for (int i = 0; i < mf->nmods; i++) {
        ModDef *m = &mf->mods[i];
        if (!m->ndeps) continue;
        sb_add(&c, "const uint32_t rmm_dep_%s[%d] = { ", m->name, m->ndeps);
        for (int d = 0; d < m->ndeps; d++) {
            if (d) sb_puts(&c, ", ");
            char u[96]; snprintf(u, sizeof u, "%s", m->deps[d]); upper(u);
            sb_add(&c, "RMM_MODID_%s", u);
        }
        sb_puts(&c, " };\n");
    }
    sb_puts(&c, "\n");
    for (int i = 0; i < mf->nmods; i++) {
        ModDef *m = &mf->mods[i];
        char u[96]; snprintf(u, sizeof u, "%s", m->name); upper(u);
        int flags = 0;
        if (m->resident_flag) flags |= 1;                /* FLAG_RESIDENT    */
        if (m->replaceable)   flags |= 2;                /* FLAG_REPLACEABLE */
        if (m->optional)      flags |= 4;                /* FLAG_OPTIONAL    */
        sb_add(&c, "\nstruct rmm_module_descriptor rmm_module_%s = {\n", m->name);
        sb_add(&c, "    .id = RMM_MODID_%s,\n", u);
        sb_add(&c, "    .base = RMM_%s_BASE, .size = RMM_%s_SIZE,\n", u, u);
        sb_puts(&c, "    .code_size = 0, .data_size = 0,\n");
        sb_add(&c, "    .name = \"%s\", .image = \"%s\",\n", m->name, m->image);
        if (m->ndeps)
            sb_add(&c, "    .dependencies = rmm_dep_%s, .dependency_count = %d,\n",
                   m->name, m->ndeps);
        else
            sb_puts(&c, "    .dependencies = 0, .dependency_count = 0,\n");
        sb_add(&c, "    .flags = 0x%X, .class = %d,\n", flags,
               manifest_class_id(m->klass));
        sb_add(&c, "    .domain = { .base = RMM_%s_BASE, .size = RMM_%s_SIZE,\n", u, u);
        sb_add(&c, "                .mask = RMM_%s_MASK, .module_id = RMM_MODID_%s,\n", u, u);
        sb_puts(&c, "                .generation = 0, .state = 0,\n");
        sb_add(&c, "                .lock = { 0, \"%s.load\" } },\n", m->name);
        sb_puts(&c, "    .active_calls = 0, .failure_count = 0, .last_failure_ms = 0,\n");
        sb_puts(&c, "    .reload_count = 0, .irq_users = false,\n};\n");
    }
    sb_puts(&c, "\nstruct rmm_module_descriptor *const rmm_module_table[RMM_MODULE_COUNT] = { ");
    for (int i = 0; i < mf->nmods; i++) {
        if (i) sb_puts(&c, ", ");
        sb_add(&c, "&rmm_module_%s", mf->mods[i].name);
    }
    sb_puts(&c, " };\n");
    char p[1200];
    gen_path2(p, sizeof p, "%s/modules.generated.c", g_gen);
    write_sbuf(p, &c);
    sb_free(&c);
}

/* phase: offsets */
static const char *OFFSETS_PROBE =
"#include <stddef.h>\n"
"#include <stdio.h>\n"
"#include <stdint.h>\n"
"#include \"rmm/module.h\"\n"
"struct probe_percpu {\n"
"    uint32_t cpu_id, apic_id;\n"
"    void *current; uint64_t kernel_stack, irq_nesting, ticks, self;\n"
"    uint64_t syscall_ksp;\n"
"    struct rmm_cpu_context rmm;\n"
"};\n"
"#define P(f) printf(\"%s %zu\\n\", #f, offsetof(struct probe_percpu, f))\n"
"#define R(f) printf(\"%s %zu\\n\", #f, offsetof(struct rmm_cpu_context, f))\n"
"#define F(f) printf(\"%s %zu\\n\", #f, offsetof(struct rmm_call_frame, f))\n"
"#define D(f) printf(\"%s %zu\\n\", #f, offsetof(struct rmm_module_descriptor, f))\n"
"#define DN(f) printf(\"%s %zu\\n\", #f, offsetof(struct rmm_module_descriptor, domain.f))\n"
"int main(void) {\n"
"    P(rmm); P(syscall_ksp);\n"
"    R(current_module); R(module_id); R(module_generation); R(exception_depth);\n"
"    R(interrupt_depth); R(preempt_disable); R(active_trap);\n"
"    R(current_call_frame); R(recovery_sp); R(active_syscall_frame);\n"
"    F(caller_rbp); F(r15); F(r14); F(r13); F(r12); F(rbx); F(prev);\n"
"    F(callee); F(callee_generation); F(return_address); F(caller_rsp);\n"
"    D(id); D(active_calls); D(failure_count); D(last_failure_ms);\n"
"    DN(state); DN(generation); DN(base); DN(lock);\n"
"    return 0;\n"
"}\n";

void phase_offsets(void) {
    char src[1200], exe[1200], cmd[2600], inc[1200];
    snprintf(src, sizeof src, "%s/offsets_probe.c", g_build);
    snprintf(exe, sizeof exe, "%s/offsets_probe", g_build);
    write_file(src, OFFSETS_PROBE, strlen(OFFSETS_PROBE));
    snprintf(inc, sizeof inc, "-I%s/include", g_kdir);
    snprintf(cmd, sizeof cmd, "cc -o %s %s %s 2>&1", exe, src, inc);
    if (system(cmd) != 0) die("offsets probe compile failed: %s", cmd);
    FILE *f = popen(exe, "r");
    if (!f) die("cannot run offsets probe");
    SBuf txt; sb_init(&txt);
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char name[128]; unsigned long long v;
        if (sscanf(line, "%127s %llu", name, &v) == 2)
            sb_add(&txt, "%s %llu\n", name, v);
    }
    pclose(f);
    char p[1200];
    snprintf(p, sizeof p, "%s/offsets.txt", g_gen);
    write_sbuf(p, &txt);
    sb_free(&txt);
    printf("offsets: dumped\n");
}

/* phase: manifest */
void phase_manifest(void) {
    Manifest mf;
    manifest_parse(&mf);
    StubTab none = {0};
    emit_generated_h(&mf, &none);
    emit_generated_c(&mf);
    printf("manifest: headers emitted\n");
}

/* object classification + scanning */
static const char *classify_unit(const char *obj) {
    for (int i = 0; g_modnames[i]; i++) {
        char pat[128];
        snprintf(pat, sizeof pat, "/modules/%s/", g_modnames[i]);
        if (strstr(obj, pat)) return g_modnames[i];
        snprintf(pat, sizeof pat, "/obj/%s/", g_modnames[i]);
        if (strstr(obj, pat)) return g_modnames[i];
    }
    if (strstr(obj, "/core/")) return "core";
    return "core";
}

/* nm -g --defined-only: GLOBAL/WEAK defined symbols with letter TDRB */
static void scan_defined(ElfImg *e, SMap *defs) {
    ElfSym s; int it = 0;
    while (elf_sym_next(e, &it, &s)) {
        if (s.bind == STB_LOCAL) continue;
        if (s.shndx == SHN_UNDEF) continue;
        char letter = elf_nm_letter(e, s.shndx);
        if (!letter || !strchr("TDRB", letter)) continue;
        smap_put(defs, s.name, (void *)(uintptr_t)letter);
    }
}
/* nm -u: undefined GLOBAL/WEAK names */
static void scan_undef(ElfImg *e, SSet *undef) {
    ElfSym s; int it = 0;
    while (elf_sym_next(e, &it, &s)) {
        if (s.bind == STB_LOCAL) continue;
        if (s.shndx != SHN_UNDEF) continue;
        sset_add(undef, s.name);
    }
}

static void assign_stubs(Manifest *mf, SMap *defs_by_unit, StubTab *out) {
    uint64_t tbase, tsize;
    if (!manifest_tramp(mf, &tbase, &tsize))
        die("modules.map: missing 'tramp' region");
    uint64_t idx = 0;
    for (int pass = 1; pass < 32; pass++) {
        ModDef *m = NULL;
        for (int i = 0; i < mf->nmods; i++)
            if (mf->mods[i].id == pass) { m = &mf->mods[i]; break; }
        if (!m) continue;
        SMap *defs = smap_get(defs_by_unit, m->name);
        if (!defs) continue;
        int n; const char **syms = smap_sorted(defs, &n);
        for (int i = 0; i < n; i++) {
            char letter = (char)(uintptr_t)smap_get(defs, syms[i]);
            if (letter != 'T') continue;
            uint64_t addr = tbase + idx * TRAMP_STRIDE;
            if (addr >= tbase + tsize)
                die("trampoline region exhausted, enlarge tramp region");
            stubs_add(out, m->name, syms[i], addr);
            idx++;
        }
        free(syms);
    }
}

static int dup_or_merge(SMap *dst, SMap *tmp, const char *unit) {
    int it = 0; void *val; const char *key;
    while ((key = smap_next(tmp, &it, &val))) {
        if (smap_has(dst, key))
            die("duplicate global symbol %s within %s", key, unit);
        smap_put(dst, key, val);
    }
    return 0;
}

void phase_prelink(const char *objlist_path) {
    Manifest mf;
    manifest_parse(&mf);

    SMap *defs   = smap_new();
    SMap *undefs = smap_new();
    smap_put(defs, "core", smap_new());
    smap_put(defs, "tramp", smap_new());
    smap_put(undefs, "core", sset_new());

    size_t olen;
    char *olist = read_file(objlist_path, &olen);
    int nobjs = 0;
    char *save = NULL;
    for (char *ln = strtok_r(olist, "\n", &save); ln;
         ln = strtok_r(NULL, "\n", &save)) {
        while (*ln == ' ' || *ln == '\t') ln++;
        size_t L = strlen(ln);
        while (L && (ln[L-1] == ' ' || ln[L-1] == '\r')) ln[--L] = 0;
        if (!*ln) continue;
        const char *unit = classify_unit(ln);
        if (!smap_has(defs, unit)) {
            smap_put(defs, unit, smap_new());
            smap_put(undefs, unit, sset_new());
        }
        ElfImg e;
        elf_open(&e, ln);
        SMap *tmp = smap_new();
        scan_defined(&e, tmp);
        dup_or_merge(smap_get(defs, unit), tmp, unit);
        scan_undef(&e, smap_get(undefs, unit));
        elf_close(&e);
        nobjs++;
    }
    free(olist);
    (void)nobjs;

    {
        SSet *cu = smap_get(undefs, "core");
        int uit = 0; void *val; const char *sym;
        while ((sym = smap_next(cu, &uit, &val))) {
            const char *owner = NULL; char oletter = 0;
            int dit = 0; const char *unit; void *dv;
            while ((unit = smap_next(defs, &dit, &dv))) {
                if (strcmp(unit, "core") == 0) continue;
                SMap *d = (SMap *)dv;
                void *lv = smap_get(d, sym);
                if (lv) { owner = unit; oletter = (char)(uintptr_t)lv; break; }
            }
            if (owner && oletter != 'T')
                die("core references module DATA '%s' (%s), forbidden; "
                    "add an accessor function to %s", sym, owner, owner);
        }
    }

    {
        SMap *seen = smap_new();
        int dit = 0; const char *unit; void *dv;
        while ((unit = smap_next(defs, &dit, &dv))) {
            SMap *d = (SMap *)dv;
            int sit = 0; const char *s; void *lv;
            while ((s = smap_next(d, &sit, &lv))) {
                if (strcmp(s, "module_entry") == 0) continue;
                if (strcmp(s, "kernel_phys_start") == 0 ||
                    strcmp(s, "kernel_phys_end") == 0) continue;
                if (smap_has(seen, s))
                    die("symbol collision: %s in %s and %s (§94)",
                        s, (const char *)smap_get(seen, s), unit);
                smap_put(seen, s, (void *)unit);
            }
        }
    }

    StubTab stubs = {0};
    assign_stubs(&mf, defs, &stubs);

    emit_generated_h(&mf, &stubs);
    emit_generated_c(&mf);

    {
        SBuf s; sb_init(&s);
        sb_puts(&s, "/* generated stub addresses, DO NOT EDIT */\n");
        for (int i = 0; i < stubs.n; i++)
            sb_add(&s, "__rmm_stub_%s_%s = 0x%llX;\n", stubs.v[i].mod,
                   stubs.v[i].sym, (unsigned long long)stubs.v[i].addr);
        char p[1200];
        gen_path2(p, sizeof p, "%s/rmm_stub.ld", g_gen);
        write_sbuf(p, &s);
        sb_free(&s);
    }

    {
        SBuf L; sb_init(&L);
        sb_puts(&L,
"/* generated core linker script, DO NOT EDIT */\n"
"OUTPUT_ARCH(i386:x86-64)\n"
"ENTRY(start)\n"
"INCLUDE build/generated/rmm_stub.ld\n"
"PHDRS\n"
"{\n"
"    text PT_LOAD FLAGS(7);   /* RWE, boot/bootstrap identity image */\n"
"    rmm  PT_LOAD FLAGS(7);   /* RWE, stubs, archive, ABI tables    */\n"
"}\n"
"SECTIONS\n"
"{\n"
"    . = 1M;\n"
"    kernel_phys_start = .;\n"
"\n"
"    .boot : { KEEP(*(.multiboot_header)) } :text\n"
"\n"
"    .text : { *(.text .text.*) } :text\n"
"    .rodata : { *(.rodata .rodata.*) } :text\n"
"    .data : { *(.data .data.*) } :text\n"
"\n"
"    . = ALIGN(8);\n"
"    __drivers_start = .;\n"
"    .drivers : { KEEP(*(.drivers .drivers.*)) } :text\n"
"    . = ALIGN(8);\n"
"    __drivers_end = .;\n"
"\n"
"    .bss : { *(COMMON) *(.bss .bss.*) } :text\n"
"\n"
"    /* reserved RMM system region (identity) */\n"
"    . = 0xE00000;\n"
"    .rmm.stubs : { *(.rmm.stubs) } :rmm\n"
"    . = 0xE40000;\n"
"    PROVIDE(__rmm_archive_start = 0xE40000);\n"
"    PROVIDE(__rmm_archive_end = 0xE40000);\n"
"    .rmm.archive : { *(.rmm.archive) } :rmm\n"
"    /* ABI + entry tables are members of .rmm.archive (emitted by\n"
"     * abi_table.S / entry_table.S); pass-1 placeholders resolve only\n"
"     * if the real objects are absent */\n"
"    PROVIDE(rmm_abi_tables = 0xF50000);\n"
"    PROVIDE(rmm_abi_table_count = 0);\n"
"    PROVIDE(rmm_entry_stubs = 0xF50000);\n"
"    PROVIDE(rmm_entry_stub_count = 0);\n"
"    . = 0x1000000;\n"
"    kernel_phys_end = .;\n"
"\n");

        SMap *mod_funcs = smap_new();
        int dit = 0; const char *unit; void *dv;
        while ((unit = smap_next(defs, &dit, &dv))) {
            if (strcmp(unit, "core") == 0) continue;
            SMap *d = (SMap *)dv;
            int sit = 0; const char *s2; void *lv;
            while ((s2 = smap_next(d, &sit, &lv)))
                if ((char)(uintptr_t)lv == 'T')
                    smap_put(mod_funcs, s2, (void *)unit);
        }
        int n; const char **syms = smap_sorted(mod_funcs, &n);
        sb_puts(&L, "    /* core -> module function aliases: calls land on stubs (§7/§8) */\n");
        for (int i = 0; i < n; i++) {
            const char *s2 = syms[i];
            if (strcmp(s2, "module_entry") == 0) continue;
            const char *mod = (const char *)smap_get(mod_funcs, s2);
            sb_add(&L, "PROVIDE(%s = __rmm_stub_%s_%s);\n", s2, mod, s2);
        }
        free(syms);
        sb_puts(&L, "}\n");
        char p[1200];
        gen_path2(p, sizeof p, "%s/core.ld", g_gen);
        write_sbuf(p, &L);
        sb_free(&L);
    }

    /* entry_table.S (spec §40) */
    {
        int with_entry[32], nwe = 0;
        for (int i = 0; i < mf.nmods; i++) {
            if (strcmp(mf.mods[i].name, "core") == 0) continue;
            SMap *d = smap_get(defs, mf.mods[i].name);
            if (!d) continue;
            if ((char)(uintptr_t)smap_get(d, "module_entry") == 'T')
                with_entry[nwe++] = i;
        }
        SBuf e; sb_init(&e);
        sb_puts(&e, ".section .rmm.archive\n"
                    ".globl rmm_entry_stubs, rmm_entry_stub_count\n");
        sb_add(&e, "rmm_entry_stub_count: .long %d\n", nwe);
        sb_puts(&e, ".balign 8\nrmm_entry_stubs:\n");
        for (int i = 0; i < nwe; i++)
            sb_add(&e, "  .quad .Lname_%s\n  .quad __rmm_stub_%s_module_entry\n",
                   mf.mods[with_entry[i]].name, mf.mods[with_entry[i]].name);
        for (int i = 0; i < nwe; i++)
            sb_add(&e, ".Lname_%s: .asciz \"%s\"\n  .balign 8\n",
                   mf.mods[with_entry[i]].name, mf.mods[with_entry[i]].name);
        char p[1200];
        gen_path2(p, sizeof p, "%s/entry_table.S", g_gen);
        write_sbuf(p, &e);
        sb_free(&e);
    }

    stubs_save(&stubs, NULL);
    write_manifest_json(&mf);

    int nwe = 0;
    for (int i = 0; i < mf.nmods; i++) {
        if (strcmp(mf.mods[i].name, "core") == 0) continue;
        SMap *d = smap_get(defs, mf.mods[i].name);
        if (d && (char)(uintptr_t)smap_get(d, "module_entry") == 'T') nwe++;
    }
    SMap *core_defs = smap_get(defs, "core");
    printf("prelink: %d stubs, %d module entries, %d core globals\n",
           stubs.n, nwe, smap_count(core_defs));
}

/* ── phase: core-syms (nm core pass-1 ELF) ───────────────────────────── */
typedef struct { uint64_t addr; char name[256]; char letter; } CoreSym;
static int cmp_coresym(const void *a, const void *b) {
    return strcmp(((const CoreSym *)a)->name, ((const CoreSym *)b)->name);
}
void phase_core_syms(const char *core_elf) {
    ElfImg e;
    elf_open(&e, core_elf);
    SBuf ld; sb_init(&ld);
    SBuf ty; sb_init(&ty);
    sb_puts(&ld, "/* generated: core pass-1 symbol addresses, DO NOT EDIT */\n");
    /* nm sorts by name (default); mirror that so the script is stable */
    CoreSym *cs = xmalloc(sizeof(CoreSym) * 4096);
    int count = 0, cap = 4096;
    ElfSym s; int it = 0;
    while (elf_sym_next(&e, &it, &s)) {
        if (s.bind == STB_LOCAL) continue;
        if (s.shndx == SHN_UNDEF) continue;
        char letter = elf_nm_letter(&e, s.shndx);
        if (!letter || !strchr("TDRB", letter)) continue;
        if (count == cap) {
            cap *= 2;
            cs = xrealloc(cs, sizeof(CoreSym) * (size_t)cap);
        }
        cs[count].addr = s.addr;
        snprintf(cs[count].name, sizeof cs[count].name, "%s", s.name);
        cs[count].letter = letter;
        count++;
    }
    elf_close(&e);
    qsort(cs, (size_t)count, sizeof *cs, cmp_coresym);
    for (int i = 0; i < count; i++) {
        sb_add(&ld, "%s = 0x%llX;\n", cs[i].name, (unsigned long long)cs[i].addr);
        sb_add(&ty, "%s %c\n", cs[i].name, cs[i].letter);
    }
    free(cs);
    char p[1200];
    gen_path2(p, sizeof p, "%s/core_syms.ld", g_gen);
    write_sbuf(p, &ld);
    sb_free(&ld);
    gen_path2(p, sizeof p, "%s/core_syms_types.txt", g_gen);
    write_sbuf(p, &ty);
    sb_free(&ty);
    printf("core-syms: %d symbols\n", count);
}

/* phase: module */
typedef struct { char sym[256]; char target[320]; } FuncImp;
typedef struct { char sym[256]; char owner[64]; uint64_t addr; } DataImp;

static int  g_nfunc, g_ndata;
static FuncImp g_func[4096];
static DataImp g_data[4096];

static void collect_objects(const char *dir, char out[][1024], int *n, int max) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) collect_objects(path, out, n, max);
        else {
            size_t L = strlen(path);
            if (L > 2 && strcmp(path + L - 2, ".o") == 0) {
                if (*n >= max) die("too many objects under %s", dir);
                snprintf(out[(*n)++], 1024, "%s", path);
            }
        }
    }
    closedir(d);
}

void phase_module(const char *mod, char **dep_nms, int ndeps) {
    Manifest mf;
    manifest_parse(&mf);
    ModDef *moddef = manifest_mod(&mf, mod);
    if (!moddef) die("module %s not in modules.map", mod);

    StubTab stubs = {0};
    stubs_load(&stubs, NULL);

    char objs[512][1024];
    int nobjs = 0;
    {
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/build/obj/%s", g_kdir, mod);
        collect_objects(dir, objs, &nobjs, 512);
    }
    SSet *undef = sset_new();
    SMap *defined = smap_new();
    for (int i = 0; i < nobjs; i++) {
        ElfImg e;
        elf_open(&e, objs[i]);
        scan_undef(&e, undef);
        scan_defined(&e, defined);
        elf_close(&e);
    }

    SMap *core_syms = smap_new();
    SMap *core_types = smap_new();
    {
        char p[1200];
        snprintf(p, sizeof p, "%s/core_syms.ld", g_gen);
        size_t len; char *txt = read_file(p, &len);
        char *save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            char name[256], addr[64];
            if (sscanf(ln, "%255s = 0x%63s", name, addr) == 2)
                smap_put(core_syms, name,
                         (void *)(uintptr_t)strtoull(addr, NULL, 16));
        }
        free(txt);
        snprintf(p, sizeof p, "%s/core_syms_types.txt", g_gen);
        txt = read_file(p, &len);
        save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            char name[256]; char typ[8];
            if (sscanf(ln, "%255s %7s", name, typ) == 2)
                smap_put(core_types, name, (void *)(uintptr_t)typ[0]);
        }
        free(txt);
    }

    SMap *dep_syms = smap_new();
    typedef struct { char owner[64]; char type; uint64_t addr; } DynSym;
    for (int i = 0; i < ndeps; i++) {
        const char *nmf = dep_nms[i];
        const char *base = strrchr(nmf, '/');
        base = base ? base + 1 : nmf;
        char owner[64];
        snprintf(owner, sizeof owner, "%s", base);
        char *dot = strrchr(owner, '.');
        if (dot && strcmp(dot, ".nm") == 0) *dot = 0;
        size_t len; char *txt = read_file(nmf, &len);
        char *save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            char name[256], typ[8], addr[64];
            if (sscanf(ln, "%63s %7s %255s", addr, typ, name) == 3) {
                if (strlen(typ) != 1 || !strchr("TDRB", typ[0])) continue;
                DynSym *ds = xmalloc(sizeof *ds);
                snprintf(ds->owner, sizeof ds->owner, "%s", owner);
                ds->type = typ[0];
                ds->addr = strtoull(addr, NULL, 16);
                smap_put(dep_syms, name, ds);
            }
        }
        free(txt);
    }

    g_nfunc = g_ndata = 0;
    int nundef; const char **usyms = smap_sorted((SMap *)undef, &nundef);
    for (int i = 0; i < nundef; i++) {
        const char *sym = usyms[i];
        if (smap_has(defined, sym)) continue;
        if (strncmp(sym, "__rmm_", 6) == 0) continue;
        if (strcmp(sym, "__drivers_start") == 0 ||
            strcmp(sym, "__drivers_end") == 0) continue;
        Stub *st = stubs_by_sym(&stubs, sym);
        if (st) {
            snprintf(g_func[g_nfunc].sym, 256, "%s", sym);
            snprintf(g_func[g_nfunc].target, 320, "__rmm_stub_%s_%s",
                     st->mod, sym);
            g_nfunc++;
        } else if (smap_has(core_syms, sym)) {
            uint64_t addr = (uint64_t)(uintptr_t)smap_get(core_syms, sym);
            char letter = (char)(uintptr_t)smap_get(core_types, sym);
            if (letter == 0) letter = 'T';
            if (letter == 'T') {
                snprintf(g_func[g_nfunc].sym, 256, "%s", sym);
                snprintf(g_func[g_nfunc].target, 320, "0x%llX",
                         (unsigned long long)addr);
                g_nfunc++;
            } else {
                snprintf(g_data[g_ndata].sym, 256, "%s", sym);
                snprintf(g_data[g_ndata].owner, 64, "core");
                g_data[g_ndata].addr = addr;
                g_ndata++;
            }
        } else {
            DynSym *ds = smap_get(dep_syms, sym);
            if (ds) {
                if (ds->type == 'T') {
                    snprintf(g_func[g_nfunc].sym, 256, "%s", sym);
                    snprintf(g_func[g_nfunc].target, 320, "__rmm_stub_%s_%s",
                             ds->owner, sym);
                    g_nfunc++;
                } else {
                    snprintf(g_data[g_ndata].sym, 256, "%s", sym);
                    snprintf(g_data[g_ndata].owner, 64, "%s", ds->owner);
                    g_data[g_ndata].addr = ds->addr;
                    g_ndata++;
                }
            } else {
                die("module %s: unresolved import '%s' (not core/stub/dep)",
                    mod, sym);
            }
        }
    }
    free(usyms);

    {
        SBuf v; sb_init(&v);
        sb_puts(&v, "/* generated import veneers, DO NOT EDIT */\n"
                    ".section .rmm.veneers, \"ax\"\n.balign 16\n");
        for (int i = 0; i < g_nfunc; i++) {
            sb_add(&v, ".globl __rmm_veneer_%s_%s\n", mod, g_func[i].sym);
            sb_add(&v, "__rmm_veneer_%s_%s:\n", mod, g_func[i].sym);
            sb_add(&v, "  movabs $%s, %%r11\n", g_func[i].target);
            sb_puts(&v, "  jmp *%r11\n  .balign 16\n");
        }
        char p[1200];
        gen_path2(p, sizeof p, "%s/%s_veneers.S", g_gen, mod);
        write_sbuf(p, &v);
        sb_free(&v);
    }


    {
        SBuf s; sb_init(&s);
        sb_add(&s, "/* generated linker script for module %s, DO NOT EDIT */\n", mod);
        sb_puts(&s, "OUTPUT_ARCH(i386:x86-64)\n");
        sb_puts(&s, "INCLUDE build/generated/core_syms.ld\n");
        sb_puts(&s, "INCLUDE build/generated/rmm_stub.ld\n");
        sb_puts(&s, "SECTIONS\n{\n");
        sb_add(&s, "    . = 0x%llX;\n", (unsigned long long)moddef->base);
        sb_puts(&s, "    __rmm_slot_base = .;\n");
        sb_puts(&s, "    .veneers : { *(.rmm.veneers) }\n\n");
        sb_puts(&s, "    . = ALIGN(8);\n");
        sb_puts(&s, "    __rmm_mod_drivers_start = .;\n");
        sb_puts(&s, "    .drivers : { KEEP(*(.drivers .drivers.*)) }\n");
        sb_puts(&s, "    . = ALIGN(8);\n");
        sb_puts(&s, "    __rmm_mod_drivers_end = .;\n\n");
        sb_puts(&s, "    . = ALIGN(0x1000);\n");
        sb_puts(&s, "    __rmm_code_start = .;\n");
        sb_puts(&s, "    .text : { *(.text .text.*) }\n");
        sb_puts(&s, "    __rmm_code_raw_end = .;\n");
        sb_puts(&s, "    . = ALIGN(0x1000);\n");
        sb_puts(&s, "    __rmm_code_end = .;\n\n");
        sb_puts(&s, "    . += 0x1000;                       /* guard page (§33) */\n");
        sb_puts(&s, "    __rmm_rodata_start = .;\n");
        sb_puts(&s, "    .rodata : { *(.rodata .rodata.*) }\n");
        sb_puts(&s, "    __rmm_rodata_raw_end = .;\n");
        sb_puts(&s, "    . = ALIGN(0x1000);\n");
        sb_puts(&s, "    __rmm_rodata_end = .;\n\n");
        sb_puts(&s, "    . += 0x1000;                       /* guard page */\n");
        sb_puts(&s, "    __rmm_data_start = .;\n");
        sb_puts(&s, "    .data : { *(.data .data.*) }\n");
        sb_puts(&s, "    __rmm_data_raw_end = .;\n");
        sb_puts(&s, "    . = ALIGN(0x1000);\n");
        sb_puts(&s, "    __rmm_data_end = .;\n\n");
        sb_puts(&s, "    . += 0x1000;                       /* guard page */\n");
        sb_puts(&s, "    __rmm_bss_start = .;\n");
        sb_puts(&s, "    .bss : { *(COMMON) *(.bss .bss.*) }\n");
        sb_puts(&s, "    . = ALIGN(0x1000);\n");
        sb_puts(&s, "    __rmm_bss_end = .;\n\n");
        sb_puts(&s, "    __rmm_slot_end = .;\n\n");
        sb_puts(&s, "    /DISCARD/ : { *(.note .note.*) *(.comment) *(.eh_frame .eh_frame.*) }\n\n");
        for (int i = 0; i < g_nfunc; i++)
            sb_add(&s, "%s%s = __rmm_veneer_%s_%s;\n",
                   i == 0 ? "        " : "    ", g_func[i].sym,
                   mod, g_func[i].sym);
        for (int i = 0; i < g_ndata; i++)
            sb_add(&s, "%s%s = 0x%llX;\n",
                   (g_nfunc == 0 && i == 0) ? "        " : "    ",
                   g_data[i].sym,
                   (unsigned long long)g_data[i].addr);
        sb_puts(&s, "}\n");
        char p[1200];
        gen_path2(p, sizeof p, "%s/%s.ld", g_gen, mod);
        write_sbuf(p, &s);
        sb_free(&s);
    }
    printf("module %s: %d veneers, %d data imports\n", mod, g_nfunc, g_ndata);
}

void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
void sb_add_bytes(SBuf *b, const void *data, size_t n) {
    sb_reserve(b, n);
    memcpy(b->s + b->len, data, n);
    b->len += n; b->s[b->len] = 0;
}
void sb_add_sbuf(SBuf *b, const SBuf *o) { sb_add_bytes(b, o->s, o->len); }
uint16_t arch_id_for(const char *arch) {
    if (!strcmp(arch, "x86_64")) return 1;
    if (!strcmp(arch, "arm64"))  return 2;
    if (!strcmp(arch, "riscv"))  return 3;
    if (!strcmp(arch, "ppc64"))  return 4;
    die("unknown kernel_arch '%s'", arch);
    return 0;
}
int count_findings(int *nfind, int nmods) {
    int t = 0;
    for (int i = 0; i < nmods; i++) t += nfind[i];
    return t;
}

/* phase: final */
typedef struct { uint64_t addr; char letter; } RawSym;

static void load_raw(const char *elf, SMap *raw) {
    ElfImg e;
    elf_open(&e, elf);
    ElfSym s; int it = 0;
    while (elf_sym_next(&e, &it, &s)) {
        if (s.bind == STB_LOCAL) continue;
        if (s.shndx == SHN_UNDEF) continue;
        char letter = elf_nm_letter(&e, s.shndx);
        if (!letter || !strchr("TDRB", letter)) continue;
        RawSym *rs = xmalloc(sizeof *rs);
        rs->addr = s.addr;
        rs->letter = letter;
        smap_put(raw, s.name, rs);
    }
    elf_close(&e);
}

static uint64_t full_sym_addr(ElfImg *e, const char *name, int *found) {
    ElfSym s; int it = 0;
    while (elf_sym_next(e, &it, &s)) {
        if (strcmp(s.name, name) == 0) { *found = 1; return s.addr; }
    }
    *found = 0;
    return 0;
}

static uint64_t offget(SMap *offs, const char *k) {
    uint64_t *v = smap_get(offs, k);
    if (!v) die("offsets.txt missing '%s' (run offsets phase)", k);
    return *v;
}

void phase_final(char **elves, int nelves) {
    Manifest mf;
    manifest_parse(&mf);
    StubTab stubs = {0};
    stubs_load(&stubs, NULL);

    SMap *offs = smap_new();
    {
        char p[1200];
        gen_path2(p, sizeof p, "%s/offsets.txt", g_gen);
        size_t len; char *txt = read_file(p, &len);
        char *save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln;
             ln = strtok_r(NULL, "\n", &save)) {
            char name[128], val[64];
            if (sscanf(ln, "%127s %63s", name, val) == 2) {
                uint64_t *pv = xmalloc(sizeof *pv);
                *pv = strtoull(val, NULL, 10);
                smap_put(offs, name, pv);
            }
        }
        free(txt);
    }

    SMap *raw_by_mod = smap_new();
    char modnames[32][64];
    char elfpaths[32][1024];
    int nmods = 0;
    for (int i = 0; i < nelves; i++) {
        char name[64], path[1024];
        if (sscanf(elves[i], "%63[^=]=%1023s", name, path) != 2)
            die("final: bad spec '%s' (want name=path)", elves[i]);
        SMap *raw = smap_new();
        load_raw(path, raw);
        snprintf(modnames[nmods], 64, "%s", name);
        snprintf(elfpaths[nmods], 1024, "%s", path);
        smap_put(raw_by_mod, modnames[nmods], raw);
        nmods++;
    }


    for (int i = 0; i < stubs.n; i++) {
        SMap *raw = smap_get(raw_by_mod, stubs.v[i].mod);
        if (!raw || !smap_has(raw, stubs.v[i].sym))
            die("stub %s::%s: symbol missing from linked %s",
                stubs.v[i].mod, stubs.v[i].sym, stubs.v[i].mod);
    }

    uint64_t RM = offget(offs, "rmm");
    {
        SBuf L; sb_init(&L);
        sb_puts(&L, "/* generated trampolines, DO NOT EDIT (spec §7-§11) */\n"
                    ".section .rmm.stubs, \"ax\"\n");
        for (int i = 0; i < stubs.n; i++) {
            Stub *st = &stubs.v[i];
            SMap *raw = smap_get(raw_by_mod, st->mod);
            RawSym *rs = smap_get(raw, st->sym);
            uint64_t target = rs->addr;
            sb_add(&L, "/* %s::%s -> 0x%llX */\n", st->mod, st->sym,
                   (unsigned long long)target);
            sb_puts(&L, ".balign 256\n");
            sb_add(&L, "__rmm_stub_%s_%s:\n", st->mod, st->sym);
            sb_puts(&L,
"    push %rbp\n"
"    mov  %rsp, %rbp\n"
"    push %r15\n"
"    push %r14\n"
"    push %r13\n"
"    push %r12\n"
"    push %rbx\n"
"    sub  $40, %rsp\n");
            sb_add(&L, "    movq %%gs:%llu, %%r10\n",
                   (unsigned long long)(RM + offget(offs, "current_call_frame")));
            sb_puts(&L, "    movq %r10, -48(%rbp)\n");
            sb_add(&L, "    movabs $rmm_module_%s, %%r10\n", st->mod);
            sb_puts(&L, "    movq %r10, -56(%rbp)\n");
            sb_add(&L, "    movl %llu(%%r10), %%r11d\n",
                   (unsigned long long)offget(offs, "generation"));
            sb_puts(&L, "    movl %r11d, -64(%rbp)\n");
            sb_puts(&L, "    movq 8(%rbp), %r11\n"
                        "    movq %r11, -72(%rbp)\n"
                        "    leaq 16(%rbp), %r11\n"
                        "    movq %r11, -80(%rbp)\n");
            sb_add(&L, "    movl %llu(%%r10), %%r11d\n",
                   (unsigned long long)offget(offs, "state"));
            sb_puts(&L,
"    cmpl $2, %r11d\n"
"    je 11f\n"
"    cmpl $3, %r11d\n"
"    je 11f\n"
"    /* slow path: preserve SysV args, call core helper (§13/§14) */\n"
"    push %r9\n"
"    push %r8\n"
"    push %rcx\n"
"    push %rdx\n"
"    push %rsi\n"
"    push %rdi\n"
"    movq %r10, %rdi\n"
"    leaq -80(%rbp), %rsi\n"
"    call rmm_stub_enter_slow\n"
"    testl %eax, %eax\n"
"    jne 19f\n"
"    pop %rdi\n"
"    pop %rsi\n"
"    pop %rdx\n"
"    pop %rcx\n"
"    pop %r8\n"
"    pop %r9\n"
"    movq -56(%rbp), %r10\n"
"11:\n");
            sb_add(&L, "    lock incl %llu(%%r10)\n",
                   (unsigned long long)offget(offs, "active_calls"));
            sb_puts(&L, "    leaq -80(%rbp), %r11\n");
            sb_add(&L, "    movq %%r11, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "current_call_frame")));
            sb_add(&L, "    movq %%r10, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "current_module")));
            sb_add(&L, "    movl %llu(%%r10), %%r11d\n",
                   (unsigned long long)offget(offs, "id"));
            sb_add(&L, "    movl %%r11d, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "module_id")));
            sb_add(&L, "    movl %llu(%%r10), %%r11d\n",
                   (unsigned long long)offget(offs, "generation"));
            sb_add(&L, "    movl %%r11d, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "module_generation")));
            sb_add(&L, "    movabs $0x%llX, %%r11\n",
                   (unsigned long long)target);
            sb_puts(&L,
"    call *%r11\n"
"    /* leave: ctx restore from prev frame; r10/r11 only */\n"
"    movq -56(%rbp), %r10\n");
            sb_add(&L, "    lock decl %llu(%%r10)\n",
                   (unsigned long long)offget(offs, "active_calls"));
            sb_puts(&L,
"    movq -48(%rbp), %r10\n"
"    testq %r10, %r10\n"
"    jz 13f\n");
            sb_add(&L, "    movq %llu(%%r10), %%r11\n",
                   (unsigned long long)offget(offs, "callee"));
            sb_add(&L, "    movq %%r11, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "current_module")));
            sb_add(&L, "    movl %llu(%%r10), %%r11d\n",
                   (unsigned long long)offget(offs, "callee_generation"));
            sb_add(&L, "    movl %%r11d, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "module_generation")));
            sb_add(&L, "    movq %%r10, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "current_call_frame")));
            sb_puts(&L,
"    jmp 14f\n"
"13:\n"
"    xorl %r11d, %r11d\n");
            sb_add(&L, "    movq %%r11, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "current_module")));
            sb_add(&L, "    movq %%r11, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "module_id")));
            sb_add(&L, "    movq %%r11, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "module_generation")));
            sb_add(&L, "    movq %%r11, %%gs:%llu\n",
                   (unsigned long long)(RM + offget(offs, "current_call_frame")));
            sb_puts(&L,
"14:\n"
"    add  $40, %rsp\n"
"    pop  %rbx\n"
"    pop  %r12\n"
"    pop  %r13\n"
"    pop  %r14\n"
"    pop  %r15\n"
"    pop  %rbp\n"
"    ret\n"
"19: /* slow path returned an RMM_ERR_*, caller never entered */\n"
"    add  $88, %rsp      /* 40 frame + 48 preserved SysV args */\n"
"    pop  %rbx\n"
"    pop  %r12\n"
"    pop  %r13\n"
"    pop  %r14\n"
"    pop  %r15\n"
"    pop  %rbp\n"
"    ret\n");
        }
        char p[1200];
        gen_path2(p, sizeof p, "%s/trampolines.S", g_gen);
        write_sbuf(p, &L);
        sb_free(&L);
    }

    {
        SBuf a; sb_init(&a);
        sb_puts(&a, "# generated ABI offset tables, DO NOT EDIT (spec §8/§41)\n"
                    ".section .rmm.archive\n"
                    ".globl rmm_abi_tables, rmm_abi_table_count\n");
        int ntab = 0;

        int idxs[32];
        for (int i = 0; i < nmods; i++) idxs[i] = i;
        for (int i = 1; i < nmods; i++) {
            int t = idxs[i]; int j = i - 1;
            while (j >= 0 && strcmp(modnames[idxs[j]], modnames[t]) > 0) {
                idxs[j + 1] = idxs[j]; j--;
            }
            idxs[j + 1] = t;
        }
        for (int k = 0; k < nmods; k++) {
            int mi = idxs[k];
            const char *mod = modnames[mi];
            SMap *raw = smap_get(raw_by_mod, mod);
            uint64_t base = manifest_mod(&mf, mod)->base;

            int n; const char **syms = smap_sorted(raw, &n);
            int count = 0;
            for (int i = 0; i < n; i++)
                if (((RawSym *)smap_get(raw, syms[i]))->letter == 'T') count++;
            sb_add(&a, ".globl rmm_abi_count_%s, rmm_abi_entries_%s\n", mod, mod);
            sb_add(&a, "rmm_abi_count_%s: .long %d\n", mod, count);
            sb_add(&a, ".balign 8\nrmm_abi_entries_%s:\n", mod);
            for (int i = 0; i < n; i++) {
                const char *s2 = syms[i];
                RawSym *rs = smap_get(raw, s2);
                if (rs->letter != 'T') continue;
                uint64_t off = rs->addr - base;
                sb_add(&a, "  .balign 8\n.Labi_%s_%s: .asciz \"%s\"\n", mod, s2, s2);
                sb_add(&a, "  .balign 8\n  .quad .Labi_%s_%s, 0x%llX\n",
                       mod, s2, (unsigned long long)off);
            }
            sb_add(&a, "rmm_abi_entries_end_%s:\n", mod);
            ntab++;
            free(syms);
        }
        sb_puts(&a, ".balign 8\n");
        sb_add(&a, "rmm_abi_table_count: .long %d\n", ntab);
        sb_puts(&a, "rmm_abi_tables:\n");
        for (int k = 0; k < nmods; k++) {
            const char *mod = modnames[idxs[k]];
            sb_add(&a, "  .balign 8\n"
                       "  .quad .Labi_modname_%s, rmm_abi_count_%s, rmm_abi_entries_%s\n"
                       ".Labi_modname_%s: .asciz \"%s\"\n",
                   mod, mod, mod, mod, mod);
        }
        sb_puts(&a, "rmm_abi_tables_end:\n");
        char p[1200];
        gen_path2(p, sizeof p, "%s/abi_table.S", g_gen);
        write_sbuf(p, &a);
        sb_free(&a);
    }

    SBuf val_json; sb_init(&val_json);
    SBuf per_mod; sb_init(&per_mod);
    long total_archive = 0;
    {
        SBuf blobs; sb_init(&blobs);
        struct { char name[16]; uint64_t off, sz, img_start; } index[32];
        int nidx = 0;
        char findings[32][64][256];
        int nfind[32];
        memset(nfind, 0, sizeof nfind);

        for (int k = 0; k < nmods; k++) {
            const char *mod = modnames[k];
            const char *elf = elfpaths[k];
            ModDef *mdef = manifest_mod(&mf, mod);
            uint64_t base = mdef->base;
            char binp[1200];
            gen_path2(binp, sizeof binp, "%s/modules/%s.bin", g_build, mod);
            ElfImg e;
            elf_open(&e, elf);
            size_t img_len; uint64_t min_addr;
            uint8_t *img = elf_flat_image(&e, &img_len, &min_addr);
            elf_close(&e);
            if (min_addr != base)
                die("module %s: image starts at 0x%llX, not slot base 0x%llX",
                    mod, (unsigned long long)min_addr, (unsigned long long)base);
            uint64_t img_start = 0;

            ElfImg e2;
            elf_open(&e2, elf);
            const char *segnames[11] = {
                "__rmm_code_start", "__rmm_code_end", "__rmm_code_raw_end",
                "__rmm_rodata_start", "__rmm_rodata_end", "__rmm_rodata_raw_end",
                "__rmm_data_start", "__rmm_data_end", "__rmm_data_raw_end",
                "__rmm_bss_start", "__rmm_bss_end" };
            uint64_t seg[11];
            int nseg = 0;
            for (int i = 0; i < 11; i++) {
                int found;
                seg[i] = full_sym_addr(&e2, segnames[i], &found);
                if (found) nseg++;
            }
            int found_entry = 0;
            uint64_t entry_addr = full_sym_addr(&e2, "module_entry", &found_entry);
            elf_close(&e2);
            if (nseg != 11)
                die("module %s: segment symbol set mismatch (%d)", mod, nseg);
            uint64_t entry_off = entry_addr - base;

            uint8_t h[RMM_H_SIZE];
            memset(h, 0, sizeof h);
            wr32(h + 0, 0x524D4D32);
            wr16(h + 4, 1);
            wr16(h + 6, arch_id_for(mf.kernel_arch));
            wr32(h + 8, (uint32_t)mdef->id);
            wr64(h + 16, seg[0] - base - img_start);
            wr64(h + 24, seg[2] - seg[0]);                        /* code_size */
            wr64(h + 32, seg[3] - base - img_start);              /* rod_off   */
            wr64(h + 40, seg[5] - seg[3]);                        /* rod_size  */
            wr64(h + 48, seg[6] - base - img_start);              /* data_off  */
            wr64(h + 56, seg[8] - seg[6]);                        /* data_size */
            wr64(h + 64, seg[10] - seg[9]);                       /* bss_size  */
            wr64(h + 72, entry_off);
            wr64(h + 80, (uint64_t)mf.abi_version);
            wr64(h + 88, 0);                                      /* flags     */
            sha256(img, img_len, h + 96);                         /* build_id  */
            snprintf((char *)h + 128, 16, "%s", mod);

            write_file(binp, (const char *)img, img_len);
            sb_add_bytes(&blobs, h, RMM_H_SIZE);
            sb_add_bytes(&blobs, img, img_len);
            snprintf(index[nidx].name, 16, "%s", mod);
            index[nidx].sz = RMM_H_SIZE + img_len;
            index[nidx].img_start = img_start;
            nidx++;

            /* validator on the freshly written flat image */
            char klass[16];
            snprintf(klass, sizeof klass, "%s",
                     mdef->klass[0] ? mdef->klass : "TRUSTED");
            nfind[k] = validate_module(mod, klass, img, img_len, base,
                                       seg[0], seg[1], mdef->size,
                                       findings[k], 64);
            if (nfind[k] && strcmp(klass, "SFI") == 0) {
                die("module %s (SFI class) failed validation: %s",
                    mod, findings[k][0]);
            }

            /* report data */
            uint64_t code_bytes = seg[1] - seg[0];
            SMap *raw = smap_get(raw_by_mod, mod);
            int n; const char **syms = smap_sorted(raw, &n);
            int exports = 0;
            for (int i = 0; i < n; i++)
                if (((RawSym *)smap_get(raw, syms[i]))->letter == 'T') exports++;
            free(syms);
            sb_add(&per_mod, "%lu:%lu:%d:%s\n", (unsigned long)img_len,
                   (unsigned long)code_bytes, exports, mod);
        }

        /* final archive: count, index, blobs */
        int index_size = 4 + nidx * 40;
        SBuf arch; sb_init(&arch);
        uint8_t cnt[4]; wr32(cnt, nidx);
        sb_add_bytes(&arch, cnt, 4);
        uint64_t cursor = index_size;
        for (int i = 0; i < nidx; i++) {
            index[i].off = cursor;
            cursor += index[i].sz;
        }
        for (int i = 0; i < nidx; i++) {
            uint8_t ent[40];
            memset(ent, 0, sizeof ent);
            snprintf((char *)ent, 16, "%s", index[i].name);
            wr64(ent + 16, index[i].off);
            wr64(ent + 24, index[i].sz);
            wr64(ent + 32, index[i].img_start);
            sb_add_bytes(&arch, ent, 40);
        }
        sb_add_sbuf(&arch, &blobs);
        total_archive = (long)arch.len;
        {
            char p[1200];
            gen_path2(p, sizeof p, "%s/modules/archive.bin", g_build);
            write_sbuf(p, &arch);
        }
        {
            char p[1200];
            gen_path2(p, sizeof p, "%s/archive.S", g_gen);
            const char *as =
                ".section .rmm.archive, \"a\"\n"
                ".globl __rmm_archive_start\n"
                "__rmm_archive_start:\n"
                ".incbin \"build/modules/archive.bin\"\n"
                ".globl __rmm_archive_end\n"
                "__rmm_archive_end:\n"
                ".balign 8\n";
            write_file(p, as, strlen(as));
        }

        /* validator JSON + report */
        sb_puts(&val_json, "{");
        for (int k = 0; k < nmods; k++) {
            if (k) sb_puts(&val_json, ",");
            sb_add(&val_json, "\"%s\":[", modnames[k]);
            for (int f = 0; f < nfind[k]; f++) {
                if (f) sb_puts(&val_json, ",");
                sb_puts(&val_json, "\"");
                json_escape(&val_json, findings[k][f]);
                sb_puts(&val_json, "\"");
            }
            sb_puts(&val_json, "]");
        }
        sb_puts(&val_json, "}");

        write_module_report_json(nmods, modnames, &per_mod,
                                 total_archive, &val_json);
        sb_free(&blobs);
        sb_free(&arch);

        printf("final: archive %ld bytes, %d modules, %d validator findings\n",
               total_archive, nmods, count_findings(nfind, nmods));
    }
}

/* phase: check-stubs */
void phase_check_stubs(const char *obj) {
    Manifest mf;
    manifest_parse(&mf);
    uint64_t tbase = 0;
    manifest_tramp(&mf, &tbase, &(uint64_t){0});
    StubTab stubs = {0};
    stubs_load(&stubs, NULL);
    ElfImg e;
    elf_open(&e, obj);

    int is_rel = e.is_rel;
    SMap *got = smap_new();
    ElfSym s; int it = 0;
    while (elf_sym_next(&e, &it, &s)) {
        if (strncmp(s.name, "__rmm_stub_", 11) == 0)
            smap_put(got, s.name, (void *)(uintptr_t)s.addr);
    }
    elf_close(&e);
    int bad = 0;
    for (int i = 0; i < stubs.n; i++) {
        char want[320];
        snprintf(want, sizeof want, "__rmm_stub_%s_%s", stubs.v[i].mod,
                 stubs.v[i].sym);
        uint64_t have = (uint64_t)(uintptr_t)smap_get(got, want);
        uint64_t expect = stubs.v[i].addr;
        if (is_rel) expect -= tbase;
        if (have != expect) {
            printf("stub %s: want 0x%llX got 0x%llX\n", want,
                   (unsigned long long)expect,
                   (unsigned long long)have);
            bad++;
        }
    }
    if (bad)
        die("%d stubs misplaced, tramp stride too small?", bad);
    printf("check-stubs: %d stubs at deterministic addresses\n", stubs.n);
}

/* phase: check-core */
void phase_check_core(const char *a, const char *b) {
    (void)a; (void)b;
}

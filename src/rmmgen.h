/* RMM build tool
 *
 * RMM build tooling: parses linker/modules.map, scans ELF object
 * symbol tables directly (no external nm/objcopy calls), assigns
 * deterministic trampoline addresses, emits linker scripts, veneers,
 * trampolines, ABI tables and the module archive, and validates the
 * linked module binaries.
 */
#ifndef RMMGEN_H
#define RMMGEN_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* globals */
extern const char *g_kdir;     /* kernel repo root */
extern char g_build[1024];     /* $(kdir)/build */
extern char g_gen[1024];       /* $(build)/generated */

#define TRAMP_STRIDE 512
#define RMM_H_SIZE   144
#define ARCH_ID_X86_64 1

void die(const char *fmt, ...);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

typedef struct { char *s; size_t len, cap; } SBuf;
void sb_init(SBuf *b);
void sb_free(SBuf *b);
void sb_add(SBuf *b, const char *fmt, ...);
void sb_puts(SBuf *b, const char *s);
void sb_putc(SBuf *b, char c);

/* file helpers */
char  *read_file(const char *path, size_t *len_out);
void   write_file(const char *path, const char *data, size_t len);
void   write_sbuf(const char *path, SBuf *b);
int    path_exists(const char *path);
void   mkdirs_for(const char *path);
/* printf-style join into a fixed buffer */
void   gen_path2(char *out, size_t cap, const char *fmt, ...);
void   sb_reserve(SBuf *b, size_t need);
void   sb_add_bytes(SBuf *b, const void *data, size_t n);
void   sb_add_sbuf(SBuf *b, const SBuf *o);
/* JSON string escape */
void   json_escape(SBuf *b, const char *s);

/* string->value hash map (preserves insertion order) */
typedef struct SMapEnt SMapEnt;
struct SMapEnt { char *key; void *val; int used; int seq; };
typedef struct SMap SMap;
struct SMap { SMapEnt *tab; size_t cap, cnt; int *order; size_t ocnt; };
SMap  *smap_new(void);
void  *smap_get(SMap *m, const char *key);
int    smap_has(SMap *m, const char *key);
void   smap_put(SMap *m, const char *key, void *val);
int    smap_count(SMap *m);

const char *smap_next(SMap *m, int *it, void **val);

const char **smap_sorted(SMap *m, int *n_out);

/* string set */
typedef SMap SSet;
SSet  *sset_new(void);
int    sset_add(SSet *s, const char *key);
int    sset_has(SSet *s, const char *key);
int    sset_count(SSet *s);

/* modules.map manifest */
typedef struct {
    char    name[64];
    uint64_t base, size, mask;
    int     id;
    char    image[128];
    char    klass[16];
    char    type[16];
    int     replaceable, optional;
    char    deps[16][64];
    int     ndeps;
    int     resident_flag;
} ModDef;

typedef struct {
    char    name[64];
    uint64_t base, size;
} RegionDef;

typedef struct {
    char     kernel_arch[32];
    int      abi_version;
    ModDef   mods[32];
    int      nmods;
    RegionDef regions[8];
    int      nregions;
    char     link_order[32][64];
    int      nlink_order;
} Manifest;


ModDef *manifest_mod(Manifest *mf, const char *name);
int     manifest_class_id(const char *klass);
void    manifest_parse(Manifest *mf);


int     manifest_tramp(Manifest *mf, uint64_t *base, uint64_t *size);

/* JSON reports */
void    write_manifest_json(Manifest *mf);
void    write_module_report_json(int nmods, char (*modnames)[64], SBuf *per_mod,
                                 long archive_bytes, SBuf *validator_json);

/* minimal ELF64 reader */
#ifndef RMM_ELF_CONSTS
#define RMM_ELF_CONSTS
#define STB_LOCAL     0
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_NOBITS   8
#define SHF_WRITE     1u
#define SHF_ALLOC     2u
#define SHF_EXECINSTR 4u
#define SHN_UNDEF     0
#define SHN_ABS       0xfff1
#define SHN_COMMON    0xfff2
#endif

typedef struct {
    uint8_t *data;
    size_t   len;
    uint64_t shoff; uint16_t shentsize, shnum, shstrndx;
    uint64_t sym_off; uint64_t sym_cnt; uint64_t sym_entsize;
    uint64_t sym_size;
    uint64_t str_off; uint64_t str_size;
    int      is_rel;
} ElfImg;


int  elf_open(ElfImg *e, const char *path);
void elf_close(ElfImg *e);

typedef struct { const char *name; uint64_t addr; int bind, type, shndx; } ElfSym;
int  elf_sym_next(ElfImg *e, int *it, ElfSym *out);
const char *elf_str(ElfImg *e, uint64_t off);

char elf_nm_letter(ElfImg *e, int shndx);

uint8_t *elf_flat_image(ElfImg *e, size_t *len_out, uint64_t *min_addr);

void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

int validate_module(const char *mod, const char *klass,
                    const uint8_t *img, size_t img_len,
                    uint64_t base, uint64_t code_start, uint64_t code_end,
                    uint64_t slot_size,
                    char findings[][256], int max_findings);

typedef struct { char mod[64]; char sym[192]; uint64_t addr; int idx; } Stub;
typedef struct { Stub *v; int n, cap; } StubTab;
void stubs_add(StubTab *t, const char *mod, const char *sym, uint64_t addr);
Stub *stubs_find(StubTab *t, const char *mod, const char *sym);
Stub *stubs_by_sym(StubTab *t, const char *sym);
void stubs_save(const StubTab *t, const char *path);
void stubs_load(StubTab *t, const char *path);

void phase_offsets(void);
void phase_manifest(void);
void phase_prelink(const char *objlist);
void phase_core_syms(const char *core_elf);
void phase_module(const char *mod, char **dep_nms, int ndeps);
void phase_final(char **elves, int nelves);
void phase_check_stubs(const char *obj);
void phase_check_core(const char *a, const char *b);

extern const char *g_modnames[];

#endif

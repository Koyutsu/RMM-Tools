/* minimal ELF64 reader: symbol tables (nm equivalent) and flat image extraction (objcopy -O binary equivalent).
 * Little-endian ELF64 only */
#define _GNU_SOURCE
#include "rmmgen.h"
#include <errno.h>

#define EI_NIDENT 16
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Elf64_Sym;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

int elf_open(ElfImg *e, const char *path) {
    memset(e, 0, sizeof *e);
    e->data = (uint8_t *)read_file(path, &e->len);
    if (e->len < sizeof(Elf64_Ehdr)) die("%s: too small for ELF64", path);
    const uint8_t *d = e->data;
    if (memcmp(d, "\x7f""ELF", 4) != 0) die("%s: not an ELF file", path);
    if (d[4] != 2) die("%s: not ELF64", path);         /* EI_CLASS = 64 */
    if (d[5] != 1) die("%s: not little-endian", path); /* EI_DATA  = LE */
    uint16_t e_type = rd16(d + EI_NIDENT + 0);
    e->is_rel = (e_type == 1);
    e->shoff     = rd64(d + 0x28);
    e->shentsize = rd16(d + 0x3A);
    e->shnum     = rd16(d + 0x3C);
    e->shstrndx  = rd16(d + 0x3E);
    if (!e->shoff || !e->shnum) die("%s: no section headers", path);

    for (int i = 0; i < e->shnum; i++) {
        const uint8_t *sp = e->data + e->shoff + (size_t)i * e->shentsize;
        if (rd32(sp + 4) == SHT_SYMTAB) {
            e->sym_off     = rd64(sp + 0x18);
            e->sym_size    = rd64(sp + 0x20);
            e->sym_entsize = rd64(sp + 0x38);
            uint32_t link  = rd32(sp + 0x28);
            if (link >= e->shnum) die("%s: symtab strtab link out of range", path);
            const uint8_t *stp = e->data + e->shoff + (size_t)link * e->shentsize;
            e->str_off  = rd64(stp + 0x18);
            e->str_size = rd64(stp + 0x20);
        }
    }
    if (!e->sym_off) die("%s: no symbol table (SHT_SYMTAB)", path);
    if (!e->sym_entsize) e->sym_entsize = 24;
    e->sym_cnt = e->sym_size / e->sym_entsize;
    return 0;
}
void elf_close(ElfImg *e) { free(e->data); memset(e, 0, sizeof *e); }

const char *elf_str(ElfImg *e, uint64_t off) {
    if (off >= e->str_size) return "";
    return (const char *)e->data + e->str_off + off;
}

int elf_sym_next(ElfImg *e, int *it, ElfSym *out) {
    while (*it < (int)e->sym_cnt) {
        const uint8_t *sp = e->data + e->sym_off + (size_t)(*it) * e->sym_entsize;
        (*it)++;
        out->name  = elf_str(e, rd32(sp + 0));
        out->bind  = sp[4] >> 4;
        out->type  = sp[4] & 0xf;
        out->shndx = (int)rd16(sp + 6);
        out->addr  = rd64(sp + 8);
        return 1;
    }
    return 0;
}

char elf_nm_letter(ElfImg *e, int shndx) {
    if (shndx == SHN_ABS) return 'A';
    if (shndx == SHN_UNDEF || shndx == SHN_COMMON) return 0;
    if (shndx >= e->shnum) return 0;
    const uint8_t *sp = e->data + e->shoff + (size_t)shndx * e->shentsize;
    uint64_t flags = rd64(sp + 8);
    uint32_t type  = rd32(sp + 4);
    if (flags & SHF_EXECINSTR) return 'T';
    if (type == SHT_NOBITS)    return 'B';
    if (flags & SHF_WRITE)     return 'D';
    if (flags & SHF_ALLOC)     return 'R';
    return 0;
}

uint8_t *elf_flat_image(ElfImg *e, size_t *len_out, uint64_t *min_addr) {
    uint64_t lo = 0, hi = 0;
    int first = 1;
    for (int i = 0; i < e->shnum; i++) {
        const uint8_t *sp = e->data + e->shoff + (size_t)i * e->shentsize;
        uint32_t type  = rd32(sp + 4);
        uint64_t flags = rd64(sp + 8);
        if (!(flags & SHF_ALLOC) || type == SHT_NOBITS) continue;
        uint64_t addr = rd64(sp + 0x10);
        uint64_t size = rd64(sp + 0x20);
        if (size == 0) continue;
        if (first || addr < lo) { lo = addr; first = 0; }
        if (addr + size > hi) hi = addr + size;
    }
    if (first) die("no allocated sections, nothing to embed");
    size_t n = (size_t)(hi - lo);
    uint8_t *img = xmalloc(n);
    for (int i = 0; i < e->shnum; i++) {
        const uint8_t *sp = e->data + e->shoff + (size_t)i * e->shentsize;
        uint32_t type  = rd32(sp + 4);
        uint64_t flags = rd64(sp + 8);
        if (!(flags & SHF_ALLOC) || type == SHT_NOBITS) continue;
        uint64_t addr = rd64(sp + 0x10);
        uint64_t size = rd64(sp + 0x20);
        uint64_t off  = rd64(sp + 0x18);
        if (size == 0 || addr + size > hi) continue;
        if (off + size > e->len) die("section data beyond EOF");
        memcpy(img + (addr - lo), e->data + off, size);
    }
    *len_out = n;
    if (min_addr) *min_addr = lo;
    return img;
}

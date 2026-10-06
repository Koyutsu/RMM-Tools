/* RMM post-link binary validator
 */
#include "rmmgen.h"

struct priv_pat { const char *bytes; size_t len; const char *name; };
static const struct priv_pat PRIV[] = {
    { "\xfa",       1, "CLI" },
    { "\xfb",       1, "STI" },
    { "\xf4",       1, "HLT" },
    { "\x0f\x22",   2, "MOV to CRx" },
    { "\x0f\x23",   2, "MOV from CRx" },
    { "\x0f\x30",   2, "WRMSR" },
    { "\x0f\x32",   2, "RDMSR" },
    { "\x0f\x01",   2, "LGDT/LIDT/LLDT group" },
    { "\x0f\x08",   2, "INVD" },
    { "\x0f\x09",   2, "WBINVD" },
    { "\x0f\x05",   2, "SYSCALL" },
    { "\x0f\x07",   2, "SYSRET" },
    { "\x0f\x34",   2, "SYSENTER" },
    { "\x0f\x35",   2, "SYSEXIT" },
    { "\xe4",       1, "IN imm" },
    { "\xe5",       1, "IN imm" },
    { "\xe6",       1, "OUT imm" },
    { "\xe7",       1, "OUT imm" },
    { "\xec",       1, "IN dx" },
    { "\xed",       1, "IN dx" },
    { "\xee",       1, "OUT dx" },
    { "\xef",       1, "OUT dx" },
};
#define NPRIV (int)(sizeof PRIV / sizeof PRIV[0])

static const uint8_t *find_pat(const uint8_t *hay, size_t hn,
                               const uint8_t *nd, size_t nn) {
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == nd[0] && memcmp(hay + i, nd, nn) == 0) return hay + i;
    return NULL;
}

int validate_module(const char *mod, const char *klass,
                    const uint8_t *img, size_t img_len,
                    uint64_t base, uint64_t code_start, uint64_t code_end,
                    uint64_t slot_size,
                    char findings[][256], int max_findings) {
    int nf = 0;
    if (!img_len) return 0;
    uint64_t code_lo = code_start > base ? code_start - base : 0;
    uint64_t code_hi = code_end > base ? code_end - base : 0;
    if (code_lo < img_len && code_hi > code_lo) {
        size_t clen = (size_t)(code_hi - code_lo);
        if (code_lo + clen > img_len) clen = img_len - code_lo;
        const uint8_t *code = img + code_lo;
        for (int i = 0; i < NPRIV; i++) {
            const uint8_t *hit = find_pat(code, clen,
                                          (const uint8_t *)PRIV[i].bytes,
                                          PRIV[i].len);
            if (hit) {
                size_t pos = (size_t)(hit - code);
                uint64_t va = base + code_lo + pos;
                if (nf < max_findings)
                    snprintf(findings[nf++], 256, "%s @ code+0x%zX (VA 0x%llX)",
                             PRIV[i].name, pos, (unsigned long long)va);
            }
        }
    }
    if (code_hi > slot_size && nf < max_findings)
        snprintf(findings[nf++], 256, "code segment exceeds slot (bounds)");
    (void)mod;
    (void)klass;
    return nf;
}

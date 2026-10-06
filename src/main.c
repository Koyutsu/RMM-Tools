/* rmm-gen entry point and CLI dispatch */
#define _GNU_SOURCE
#include "rmmgen.h"
#include <errno.h>
#include <limits.h>
#include <unistd.h>

const char *g_kdir = ".";
char g_build[1024];
char g_gen[1024];

static void usage(void) {
    fprintf(stderr,
        "usage: rmm-gen [--kdir <kernel-root>] <phase> [args]\n"
        "\n"
        "phases:\n"
        "  offsets                       dump C struct offsets (host cc)\n"
        "  manifest                      bootstrap headers from modules.map\n"
        "  prelink <objlist>             stub table, core.ld, entry table\n"
        "  core-syms <core1.elf>         core pass-1 symbol script\n"
        "  module <mod> [dep.nm ...]     veneers + linker script for a module\n"
        "  final <mod>=<elf> ...         trampolines, ABI table, archive, validate\n"
        "  check-stubs <trampolines.o>   assembled stubs match the plan\n"
        "  check-core [a.elf b.elf]      informational\n");
    exit(2);
}

int main(int argc, char **argv) {
    int i = 1;
    while (i < argc && strncmp(argv[i], "--", 2) == 0) {
        if (strcmp(argv[i], "--kdir") == 0 && i + 1 < argc) {
            g_kdir = argv[i + 1];
            i += 2;
        } else {
            usage();
        }
    }
    if (i >= argc) usage();
    const char *phase = argv[i++];

    char resolved[PATH_MAX];
    if (!realpath(g_kdir, resolved)) die("kernel root %s: %s", g_kdir, strerror(errno));
    g_kdir = strdup(resolved);
    snprintf(g_build, sizeof g_build, "%s/build", g_kdir);
    snprintf(g_gen, sizeof g_gen, "%s/build/generated", g_kdir);

    if (strcmp(phase, "offsets") == 0) {
        phase_offsets();
    } else if (strcmp(phase, "manifest") == 0) {
        phase_manifest();
    } else if (strcmp(phase, "prelink") == 0) {
        if (i >= argc) usage();
        phase_prelink(argv[i]);
    } else if (strcmp(phase, "core-syms") == 0) {
        if (i >= argc) usage();
        phase_core_syms(argv[i]);
    } else if (strcmp(phase, "module") == 0) {
        if (i >= argc) usage();
        phase_module(argv[i], &argv[i + 1], argc - i - 1);
    } else if (strcmp(phase, "final") == 0) {
        phase_final(&argv[i], argc - i);
    } else if (strcmp(phase, "check-stubs") == 0) {
        if (i >= argc) usage();
        phase_check_stubs(argv[i]);
    } else if (strcmp(phase, "check-core") == 0) {
        phase_check_core(i < argc ? argv[i] : NULL,
                         i + 1 < argc ? argv[i + 1] : NULL);
    } else {
        die("unknown phase '%s'", phase);
    }
    return 0;
}

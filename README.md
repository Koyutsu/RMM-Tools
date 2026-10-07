# RMM build tooling for the Lumina Kernel

Build-time tooling for the Lumina module system

It reads ELF64 object/executable symbol tables directly, computes module image headers with an embedded
SHA-256, and runs the post-link over every linked module.

## Manifest rules enforced

- slot sizes are powers of two, `base % size == 0`
- slots never overlap
- dependency graph is a DAG
- trampoline region capacity: 512 B per exported function

## Build

    make            # bin/rmm-gen
    make test       # build + smoke tests (sha256 known-answer, ELF reader)

## CLI

    rmm-gen [--kdir <kernel-root>] <phase> [args]

    `--kdir` defaults to the current directory
    `/build` and `build/generated/` are derived from it.  
    The kernel repo invokes it as `tools/bin/rmm-gen` from its root (see the kernel Makefile).

## Relationship to the kernel repo

This repo is consumed by the kernel as a git **submodule** at `tools/`:

    git submodule add https://github.com/Koyutsu/RMM-Tools.git tools

The kernel Makefile builds it on first use (`make -C tools`) and calls
`tools/bin/rmm-gen <phase>` for every generation step.  The authoritative
slot map lives in the kernel repo (`linker/modules.map`)

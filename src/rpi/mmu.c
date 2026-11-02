/**
 * @file mmu.c
 * @brief A flat 1 MB-section map for the ARM1176 (ARMv6 descriptors), the
 * L1 caches and branch prediction. Without the caches the ARM1176 runs
 * BASIC several times slower.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "rpi.h"

static uint32_t ttb[4096] __attribute__((aligned(16384)));

#define SECT 0x2u
#define SECT_B (1u << 2)
#define SECT_C (1u << 3)
#define SECT_XN (1u << 4)
#define SECT_AP_RW (3u << 10)
#define SECT_TEX(x) ((uint32_t)(x) << 12)

/// Normal memory, write-back write-allocate (TEX 001, C, B).
#define MEM_NORMAL (SECT | SECT_AP_RW | SECT_TEX(1) | SECT_C | SECT_B)
/// Normal memory, not cached (TEX 001): the GPU's part, framebuffers.
#define MEM_UNCACHED (SECT | SECT_AP_RW | SECT_TEX(1))
/// Shared device (B only): the peripherals.
#define MEM_DEVICE (SECT | SECT_AP_RW | SECT_B | SECT_XN)

void mmu_init(uint32_t ram_top) {
    for (uint32_t i = 0; i < 4096; i++) {
        uint32_t addr = i << 20;
        uint32_t d = 0;  // fault
        if (addr < ram_top)
            d = MEM_NORMAL;
        else if (addr < PERI_BASE)
            d = MEM_UNCACHED;
        else if (addr < PERI_BASE + 0x01000000u)
            d = MEM_DEVICE;
        ttb[i] = d ? (addr | d) : 0;
    }

    uint32_t z = 0;
    __asm volatile(
        "mcr p15, 0, %0, c7, c7, 0\n"   // invalidate both caches
        "mcr p15, 0, %0, c8, c7, 0\n"   // invalidate TLBs
        "mcr p15, 0, %0, c7, c10, 4\n"  // DSB
        "mcr p15, 0, %0, c2, c0, 2\n"   // TTBCR = 0: TTBR0 for everything
        "mcr p15, 0, %1, c2, c0, 0\n"   // TTBR0
        "mcr p15, 0, %2, c3, c0, 0\n"   // domain 0: client
        ::"r"(z), "r"((uint32_t)(uintptr_t)ttb), "r"(1u)
        : "memory");

    uint32_t cr;
    __asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(cr));
    cr |= (1u << 0)     // MMU
        | (1u << 2)     // D-cache
        | (1u << 11)    // branch prediction
        | (1u << 12)    // I-cache
        | (1u << 22)    // unaligned access
        | (1u << 23);   // ARMv6 descriptors (XP)
    cr &= ~(1u << 1);   // no alignment faults
    __asm volatile(
        "mcr p15, 0, %0, c1, c0, 0\n"
        "mcr p15, 0, %1, c7, c5, 4\n"   // flush prefetch buffer
        ::"r"(cr), "r"(z)
        : "memory");
}

/// Normal memory, write-through (TEX 000, C): the framebuffer, so drawing
/// reads from the cache and writes reach the GPU at once.
#define MEM_WRITE_THROUGH (SECT | SECT_AP_RW | SECT_C | SECT_XN)

void *mmu_map_write_through(uint32_t virt, uint32_t phys, uint32_t len) {
    virt &= ~0xFFFFFu;
    uint32_t first = phys & ~0xFFFFFu, end = phys + len;
    if (!len || end < phys || virt + (end - first) < virt)
        return 0;
    for (uint32_t mb = first; mb < end; mb += 0x100000u) {
        ttb[(virt + (mb - first)) >> 20] = mb | MEM_WRITE_THROUGH;
        if (mb + 0x100000u < mb)
            break;
    }
    // The table walk doesn't look in the data cache: write the table out.
    dcache_clean_invalidate(&ttb[virt >> 20], ((end - first + 0xFFFFFu) >> 20) * 4);
    __asm volatile(
        "mcr p15, 0, %0, c8, c7, 0\n"   // invalidate TLBs
        "mcr p15, 0, %0, c7, c5, 4\n"   // flush prefetch buffer
        ::"r"(0) : "memory");
    return (void *)(uintptr_t)(virt + (phys - first));
}

void dcache_clean_invalidate(const void *addr, size_t len) {
    uint32_t a = (uint32_t)(uintptr_t)addr & ~31u;
    uint32_t end = (uint32_t)(uintptr_t)addr + len;
    for (; a < end; a += 32)
        __asm volatile("mcr p15, 0, %0, c7, c14, 1" ::"r"(a) : "memory");
    dsb();
}

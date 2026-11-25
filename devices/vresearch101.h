#ifndef HENGEVM_VRESEARCH101_H
#define HENGEVM_VRESEARCH101_H

#include "disk.h"
#include "vpiop.h"

/* vphone-cli's virtual iPhone: vmapple's memory map plus these (qemu-vphone
   hw/vmapple/vresearch101.c). */
#define VRESEARCH_SCRATCH      0x00500000ull /* RAM up to 0x700000; 0x410000..0x500000 is the cfg window's */
#define VRESEARCH_SCRATCH_SIZE 0x00200000ull
#define VRESEARCH_SEP_MBOX     0x30250000ull
#define VRESEARCH_IRQ_SEP      0x14          /* SPI: inbox not empty; +1 outbox empty */

/* The SEP core's own address space: these, over everything of the AP's but its GIC. */
#define SEP_ROM_BASE   0x02000000ull /* OpenSEP */
#define SEP_ROM_SIZE   0x00080000ull
#define SEP_SRAM_BASE  0x04000000ull
#define SEP_SRAM_SIZE  0x00800000ull
#define SEP_TZ0_BASE   0x06000000ull /* where the SEP ROM loads sepOS */
#define SEP_TZ0_SIZE   0x02000000ull
#define SEP_VSTD_BASE  0x08000000ull /* SEPD's storage request buffer */
#define SEP_VSTD_SIZE  0x00004000ull
#define SEP_VSTR_BASE  0x30020000ull /* SEP storage registers */
#define SEP_UNIMP_BASE 0x30220000ull /* a SEP-only device sepOS maps (the AP's AES there) */

struct HvVmapple;
struct HvSepStorage;
struct HvSharedMmio;

typedef struct HvVresearch101 {
    uint8_t *scratch;
    HvVpiop *sep_mbox;
    /* The SEP: a second machine with one CPU (hv_vresearch101_add_sep). */
    HvMachine *sep_m;
    HvCpu *sep_cpu;
    HvGic *sep_gic;
    uint8_t *sep_rom, *sep_sram, *sep_tz0, *sep_vstd;
    struct HvSepStorage *sep_storage;
    struct HvSharedMmio *shared;
} HvVresearch101;

/* Adds vresearch101's devices to a board hv_vmapple_create just built. */
int hv_vresearch101_add(struct HvVmapple *b);
/* Builds the SEP in sep_m (initialized, empty) with its CPU, after the board:
   OpenSEP from rom_path, SEPStorage on storage (NULL: none). */
int hv_vresearch101_add_sep(struct HvVmapple *b, HvMachine *sep_m, HvCpu *sep_cpu, const char *rom_path,
                            HvDisk *storage);
/* CPU reset: EL1, MMU off, at the SEP ROM. */
void hv_vresearch101_reset_sep(struct HvVmapple *b);
void hv_vresearch101_destroy(struct HvVmapple *b);

#endif

#ifndef HENGEVM_VRESEARCH101_H
#define HENGEVM_VRESEARCH101_H

#include "vpiop.h"

/* vphone-cli's virtual iPhone: vmapple's memory map plus these (qemu-vphone
   hw/vmapple/vresearch101.c). */
#define VRESEARCH_SCRATCH      0x00500000ull /* RAM up to 0x700000; 0x410000..0x500000 is the cfg window's */
#define VRESEARCH_SCRATCH_SIZE 0x00200000ull
#define VRESEARCH_SEP_MBOX     0x30250000ull
#define VRESEARCH_IRQ_SEP      0x14          /* SPI: inbox not empty; +1 outbox empty */

struct HvVmapple;

typedef struct HvVresearch101 {
    uint8_t *scratch;
    HvVpiop *sep_mbox;
} HvVresearch101;

/* Adds vresearch101's devices to a board hv_vmapple_create just built. */
int hv_vresearch101_add(struct HvVmapple *b);
void hv_vresearch101_destroy(struct HvVmapple *b);

#endif

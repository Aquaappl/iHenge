#ifndef HENGEVM_CFG_H
#define HENGEVM_CFG_H

#include <stdint.h>

#include "machine.h"

typedef enum HvMachineKind {
    HV_MACHINE_VMAPPLE,      /* the virtual Mac */
    HV_MACHINE_VRESEARCH101, /* vphone-cli's virtual iPhone */
} HvMachineKind;

/* The vmapple configuration region: 64 KB of RAM the boot ROM reads, laid
   out as QEMU's hw/vmapple/cfg.c describes it. */
typedef struct HvCfgParams {
    HvMachineKind machine;
    uint32_t nr_cpus;
    uint64_t ecid;
    uint64_t ram_size;
    uint32_t run_installer1, run_installer2;
    uint32_t rnd;
    uint8_t mac_en0[6], mac_en1[6], mac_wifi0[6], mac_bt0[6];
    const char *serial, *model, *soc_name; /* NULL: QEMU's defaults */
    uint64_t seed;  /* nonzero: `rnd` gives a reproducible sequence (for debugging) */
} HvCfgParams;

/* Returns the host copy of the region (64 KB). */
uint8_t *hv_cfg_create(HvMachine *m, uint64_t pa, const HvCfgParams *p);

#endif

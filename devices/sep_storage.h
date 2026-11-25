#ifndef HENGEVM_SEP_STORAGE_H
#define HENGEVM_SEP_STORAGE_H

#include <stdint.h>

#include "disk.h"
#include "machine.h"

/* vresearch101's SEP storage ("VSTR"): the VM bundle's SEPStorage file, which
   sepOS's SEPD reaches through a register window and a request buffer (VSTD). */
typedef struct HvSepStorage {
    HvMachine *m;   /* the SEP's: descriptors and data are in its address space */
    HvDisk *disk;   /* NULL: every request fails */
    uint32_t enable, status, done;
    uint64_t notify;
} HvSepStorage;

HvSepStorage *hv_sep_storage_create(HvMachine *m, uint64_t pa, HvDisk *disk);

#endif

#include "vresearch101.h"
#include "vmapple.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* avp-ctrr: qemu-vphone leaves it unimplemented (reads 0, writes ignored), and
   its iOS guests boot that way. Logged, so the guest's use of it shows. */
static uint64_t ctrr_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    if (getenv("HV_AVP_LOG")) fprintf(stderr, "hengevm: avp-ctrr read +%llx (size %u)\n", (unsigned long long)off, size);
    return 0;
}

static void ctrr_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque;
    if (getenv("HV_AVP_LOG"))
        fprintf(stderr, "hengevm: avp-ctrr write +%llx = %llx (size %u)\n", (unsigned long long)off, (unsigned long long)v, size);
}

int hv_vresearch101_add(HvVmapple *b) {
    HvVresearch101 *v = &b->vr;
    v->scratch = mmap(NULL, VRESEARCH_SCRATCH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (v->scratch == MAP_FAILED) {
        v->scratch = NULL;
        return -1;
    }
    hv_machine_add_ram(b->m, VRESEARCH_SCRATCH, VRESEARCH_SCRATCH_SIZE, v->scratch, 0);
    hv_machine_add_mmio(b->m, VMAPPLE_CTRR, 0x10000, b, ctrr_read, ctrr_write, "avp-ctrr");
    v->sep_mbox = hv_vpiop_create(b->m, b->gic, VRESEARCH_SEP_MBOX, 32 + VRESEARCH_IRQ_SEP, 32 + VRESEARCH_IRQ_SEP + 1);
    return v->sep_mbox ? 0 : -1;
}

void hv_vresearch101_destroy(HvVmapple *b) {
    HvVresearch101 *v = &b->vr;
    if (v->scratch) munmap(v->scratch, VRESEARCH_SCRATCH_SIZE);
    free(v->sep_mbox);
    memset(v, 0, sizeof *v);
}

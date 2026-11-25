#include "vresearch101.h"
#include "sep_storage.h"
#include "timer.h"
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
    b->m->impdef_store = 1;
    b->m->gxf = 1;
    b->m->gxf_log = getenv("HENGE_GXF_LOG") != NULL;
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

/* One of the AP's devices as the SEP sees it: under the AP machine's device
   lock (the SEP's own is never held here), the UART's writes tagged as the SEP's. */
typedef struct HvSharedMmio {
    HvMachine *owner;
    HvMmio dev;
    HvPl011 *uart;
} HvSharedMmio;

static uint64_t shared_read(void *opaque, uint64_t off, unsigned size) {
    HvSharedMmio *s = opaque;
    if (!s->dev.self_locked) hv_dev_lock(s->owner);
    if (s->uart) s->uart->source = 1;
    uint64_t r = s->dev.read(s->dev.opaque, off, size);
    if (s->uart) s->uart->source = 0;
    if (!s->dev.self_locked) hv_dev_unlock(s->owner);
    return r;
}

static void shared_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvSharedMmio *s = opaque;
    if (!s->dev.self_locked) hv_dev_lock(s->owner);
    if (s->uart) s->uart->source = 1;
    s->dev.write(s->dev.opaque, off, val, size);
    if (s->uart) s->uart->source = 0;
    if (!s->dev.self_locked) hv_dev_unlock(s->owner);
}

/* sep-dev-30220000: a SEP-only device sepOS maps; not known yet (qemu-vphone: unimplemented). */
static uint64_t unimp_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque;
    fprintf(stderr, "hengevm: sep: read of unimplemented %llx (size %u)\n", (unsigned long long)(SEP_UNIMP_BASE + off), size);
    return 0;
}

static void unimp_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)opaque;
    fprintf(stderr, "hengevm: sep: write of unimplemented %llx = %llx (size %u)\n",
            (unsigned long long)(SEP_UNIMP_BASE + off), (unsigned long long)v, size);
}

static uint64_t sep_tick(HvMachine *m, HvCpu *cpu) {
    HvVmapple *b = m->tick_opaque;
    return hv_timer_update(b->vr.sep_gic, cpu);
}

static uint8_t *sep_region(HvMachine *m, uint64_t pa, uint64_t size, int readonly) {
    uint8_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (hv_machine_add_ram(m, pa, size, p, readonly) != 0) {
        munmap(p, size);
        return NULL;
    }
    return p;
}

int hv_vresearch101_add_sep(HvVmapple *b, HvMachine *sep_m, HvCpu *sep_cpu, const char *rom_path, HvDisk *storage) {
    HvVresearch101 *v = &b->vr;
    HvMachine *ap = b->m;
    v->sep_m = sep_m;
    v->sep_cpu = sep_cpu;
    sep_m->log = ap->log;
    sep_m->wfe_sleep = 1; /* OpenSEP waits for the mailbox in WFE */
    sep_m->impdef_store = 1;
    sep_m->gxf = 1;
    sep_m->gxf_log = ap->gxf_log;

    /* The AP's memory, the same host pages; then the SEP's own. */
    for (int i = 0; i < ap->nram; i++)
        if (hv_machine_add_ram(sep_m, ap->ram[i].pa, ap->ram[i].size, ap->ram[i].host, ap->ram[i].readonly) != 0) return -1;
    v->sep_rom = sep_region(sep_m, SEP_ROM_BASE, SEP_ROM_SIZE, 1);
    v->sep_sram = sep_region(sep_m, SEP_SRAM_BASE, SEP_SRAM_SIZE, 0);
    v->sep_tz0 = sep_region(sep_m, SEP_TZ0_BASE, SEP_TZ0_SIZE, 0);
    v->sep_vstd = sep_region(sep_m, SEP_VSTD_BASE, SEP_VSTD_SIZE, 0);
    if (!v->sep_rom || !v->sep_sram || !v->sep_tz0 || !v->sep_vstd) return -1;
    FILE *f = fopen(rom_path, "rb");
    if (!f) return -1;
    size_t n = fread(v->sep_rom, 1, SEP_ROM_SIZE, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (!n || more) return -1;

    /* Its own devices first: the first match wins, so they cover the AP's at the same addresses. */
    v->sep_gic = hv_gicv3_create(sep_m, VMAPPLE_GIC_DIST, VMAPPLE_GIC_REDIST);
    v->sep_storage = hv_sep_storage_create(sep_m, SEP_VSTR_BASE, storage);
    hv_machine_add_mmio(sep_m, SEP_UNIMP_BASE, 0x10000, NULL, unimp_read, unimp_write, "sep-unimp");
    hv_vpiop_attach_sep(v->sep_mbox, sep_m, v->sep_gic, sep_cpu, VRESEARCH_SEP_MBOX, 32 + VRESEARCH_IRQ_SEP,
                        32 + VRESEARCH_IRQ_SEP + 1);
    if (!v->sep_gic || !v->sep_storage) return -1;

    v->shared = calloc((size_t)ap->nmmio, sizeof *v->shared);
    if (!v->shared) return -1;
    for (int i = 0; i < ap->nmmio; i++) {
        HvMmio *d = &ap->mmio[i];
        if (hv_mmio_lookup(sep_m, d->pa)) continue;
        HvSharedMmio *s = &v->shared[i];
        *s = (HvSharedMmio){ap, *d, d->opaque == b->uart ? b->uart : NULL};
        if (hv_machine_add_mmio(sep_m, d->pa, d->size, s, shared_read, shared_write, d->name) != 0) return -1;
        sep_m->mmio[sep_m->nmmio - 1].self_locked = 1; /* the wrapper takes the AP's lock */
    }
    sep_m->tick = sep_tick;
    sep_m->tick_opaque = b;
    return 0;
}

void hv_vresearch101_reset_sep(HvVmapple *b) {
    HvVresearch101 *v = &b->vr;
    if (!v->sep_m) return;
    v->sep_m->cntvoff = b->m->cntvoff; /* one counter for the whole system */
    hv_cpu_reset_index(v->sep_cpu, v->sep_m, 0);
    v->sep_cpu->mpidr = 0x80000000u; /* affinity 0: the only core on its GIC */
    v->sep_cpu->pc = SEP_ROM_BASE;
}

void hv_vresearch101_destroy(HvVmapple *b) {
    HvVresearch101 *v = &b->vr;
    if (v->scratch) munmap(v->scratch, VRESEARCH_SCRATCH_SIZE);
    if (v->sep_rom) munmap(v->sep_rom, SEP_ROM_SIZE);
    if (v->sep_sram) munmap(v->sep_sram, SEP_SRAM_SIZE);
    if (v->sep_tz0) munmap(v->sep_tz0, SEP_TZ0_SIZE);
    if (v->sep_vstd) munmap(v->sep_vstd, SEP_VSTD_SIZE);
    if (v->sep_gic) {
        pthread_mutex_destroy(&v->sep_gic->mu);
        free(v->sep_gic);
    }
    free(v->sep_storage);
    free(v->shared);
    free(v->sep_mbox);
    memset(v, 0, sizeof *v);
}

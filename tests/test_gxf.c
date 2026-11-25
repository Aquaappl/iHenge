#include "dispatch.h"

#include <stdio.h>
#include <stdlib.h>

/* Apple GXF on a machine with m->gxf: GENTER into GL1 at GXF_ENTER_EL1, the GL1
   registers and SP_GL1 while guarded, GEXIT back; GEXIT outside GL1 is undefined. */
static HvMachine machine;
static HvCpu cpu;
static int failures;

#define SYSREG(op1, crn, crm, op2) ((uint32_t)(op1) << 16 | (uint32_t)(crn) << 12 | (uint32_t)(crm) << 8 | (uint32_t)(op2) << 5)
#define MRS(rt, op1, crn, crm, op2) (0xd5380000u | SYSREG(op1, crn, crm, op2) | (rt))
#define MSR(op1, crn, crm, op2, rt) (0xd5180000u | SYSREG(op1, crn, crm, op2) | (rt))
#define GENTER 0x00201420u
#define GEXIT  0x00201400u
#define HVC0   0xd4000002u

static void check(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: %llx, want %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint32_t *setup(void) {
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&machine, 4 << 20)) exit(1);
    for (int i = 0; i < 4096; i++) code[i] = HVC0;
    hv_machine_add_ram(&machine, 0x10000, 16384, code, 1);
    machine.gxf = 1;
    hv_cpu_reset_index(&cpu, &machine, 0);
    cpu.pc = 0x10000;
    cpu.x[0] = 0x84000008; /* SYSTEM_OFF: the hvc ends the run */
    return code;
}

static void genter_gexit(void) {
    uint32_t *code = setup();
    code[0] = MSR(6, 15, 8, 1, 1);    /* GXF_ENTER_EL1 = x1 */
    code[1] = GENTER;
    code[2] = MRS(8, 6, 15, 8, 0);    /* back: GXF_STATUS_EL1 */
    code[3] = 0x910003e7;             /* mov x7, sp */
    code[4] = HVC0;
    code[64] = MRS(2, 6, 15, 8, 0);   /* guarded, at 0x10100: GXF_STATUS_EL1 */
    code[65] = MRS(3, 6, 15, 10, 6);  /* ELR_GL1 */
    code[66] = MRS(4, 6, 15, 10, 5);  /* ESR_GL1 */
    code[67] = MRS(5, 6, 15, 10, 3);  /* SPSR_GL1 */
    code[68] = 0x910003e6;            /* mov x6, sp: SP_GL1 */
    code[69] = MRS(9, 0, 4, 0, 1);    /* ELR_EL1: still EL1's own in the M1 layout */
    code[70] = GEXIT;
    cpu.x[1] = 0x10100;
    cpu.sp = 0x1000;
    cpu.sp_gl1 = 0x2000;
    cpu.elr_el1 = 0x5555;
    int r = hv_run(&cpu);
    check("the run ends in SYSTEM_OFF", r, HV_EXIT_HALT);
    check("guarded: GXF_STATUS_EL1", cpu.x[2], 1);
    check("ELR_GL1: after the GENTER", cpu.x[3], 0x10008);
    check("ESR_GL1: GENTER's syndrome", cpu.x[4], 0xfe010000);
    check("SPSR_GL1: EL1h, all masked", cpu.x[5], 0x3c5);
    check("SP while guarded: SP_GL1", cpu.x[6], 0x2000);
    check("ELR_EL1 while guarded", cpu.x[9], 0x5555);
    check("after GEXIT: GXF_STATUS_EL1", cpu.x[8], 0);
    check("after GEXIT: SP_EL1 again", cpu.x[7], 0x1000);
    check("after GEXIT: not guarded", cpu.gl, 0);
    hv_machine_free(&machine);
    free(code);
}

/* GEXIT outside GL1, and an SP_GL1 read, are undefined: the CPU goes to VBAR_EL1 + 0x200. */
static void undefined_outside_gl(void) {
    uint32_t *code = setup();
    code[0] = 0xd518c000u | 2;        /* msr vbar_el1, x2 */
    code[1] = GEXIT;
    code[512] = MRS(3, 0, 5, 2, 0);   /* the vector at 0x10800 + 0x200: ESR_EL1 */
    code[513] = HVC0;
    cpu.x[2] = 0x10600;               /* VBAR: a sync exception from EL1h lands at +0x200, code[512] */
    int r = hv_run(&cpu);
    check("undefined GEXIT: run ends", r, HV_EXIT_HALT);
    check("undefined GEXIT: EC 0 in ESR_EL1", cpu.x[3] >> 26, 0);
    check("undefined GEXIT: not guarded", cpu.gl, 0);
    hv_machine_free(&machine);
    free(code);
}

int main(void) {
    genter_gexit();
    undefined_outside_gl();
    printf("GXF: %d failures\n", failures);
    return failures != 0;
}

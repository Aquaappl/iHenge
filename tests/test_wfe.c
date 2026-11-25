#include "dispatch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* A machine with wfe_sleep (the SEP): a WFE with no watched word sleeps until
   hv_cpu_wake or, as a safety net, 1 ms (24000 ticks at 24 MHz). */
static HvMachine machine;
static HvCpu cpu;
static int failures;

static void check(const char *what, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static uint64_t no_deadline(HvMachine *m, HvCpu *c) {
    (void)m, (void)c;
    return 0;
}

static void *run_cpu(void *arg) { return (void *)(intptr_t)hv_run(arg); }

static void wfe_once(int wake_after_us, const char *name) {
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&machine, 4 << 20)) exit(1);
    code[0] = 0xd503205f; /* wfe */
    code[1] = 0xd4000002; /* hvc #0: SYSTEM_OFF ends the run */
    hv_machine_add_ram(&machine, 0x10000, 16384, code, 1);
    machine.tick = no_deadline;
    machine.wfe_sleep = 1;
    hv_cpu_reset_index(&cpu, &machine, 0);
    cpu.pc = 0x10000;
    cpu.x[0] = 0x84000008;
    pthread_t t;
    pthread_create(&t, NULL, run_cpu, &cpu);
    if (wake_after_us) {
        usleep((useconds_t)wake_after_us);
        hv_cpu_wake(&cpu);
    }
    void *result;
    pthread_join(t, &result);
    char what[128];
    snprintf(what, sizeof what, "%s: the run ends in SYSTEM_OFF", name);
    check(what, (intptr_t)result == HV_EXIT_HALT);
    snprintf(what, sizeof what, "%s: the WFE slept once, not spun", name);
    check(what, machine.stats.wfi == 1);
    if (wake_after_us) {
        snprintf(what, sizeof what, "%s: the wake ended the sleep before the 1 ms net (%llu ticks)", name,
                 (unsigned long long)machine.stats.wfi_ticks);
        check(what, machine.stats.wfi_ticks < 24000);
    }
    hv_machine_free(&machine);
    free(code);
}

int main(void) {
    wfe_once(0, "no wake");
    wfe_once(200, "woken");
    printf("WFE sleep: %d failures\n", failures);
    return failures != 0;
}

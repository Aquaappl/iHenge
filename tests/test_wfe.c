#include "dispatch.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* A machine with wfe_sleep (the SEP): a WFE with no watched word sleeps until
   hv_cpu_wake or the net (wfe_net_ticks). No check depends on how fast the
   host schedules threads: a lost wake shows as a sleep of the whole net. */
static HvMachine machine;
static HvCpu cpu;
static int failures;

#define TICKS_PER_S 24000000ull

static void check(const char *what, int ok) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; }
}

static uint64_t no_deadline(HvMachine *m, HvCpu *c) {
    (void)m, (void)c;
    return 0;
}

static void *run_cpu(void *arg) { return (void *)(intptr_t)hv_run(arg); }

enum { NO_WAKE, WAKE_WHILE_RUNNING, WAKE_BEFORE };

static void wfe_once(int how, const char *name) {
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&machine, 4 << 20)) exit(1);
    code[0] = 0xd503205f; /* wfe */
    code[1] = 0xd4000002; /* hvc #0: SYSTEM_OFF ends the run */
    hv_machine_add_ram(&machine, 0x10000, 16384, code, 1);
    machine.tick = no_deadline;
    machine.wfe_sleep = 1;
    if (how != NO_WAKE) machine.wfe_net_ticks = 10 * TICKS_PER_S; /* only a lost wake would wait this long */
    hv_cpu_reset_index(&cpu, &machine, 0);
    cpu.pc = 0x10000;
    cpu.x[0] = 0x84000008;
    if (how == WAKE_BEFORE) hv_cpu_wake(&cpu); /* the event is kept until the WFE */
    pthread_t t;
    pthread_create(&t, NULL, run_cpu, &cpu);
    if (how == WAKE_WHILE_RUNNING) {
        usleep(2000);
        hv_cpu_wake(&cpu);
    }
    void *result;
    pthread_join(t, &result);
    char what[160];
    snprintf(what, sizeof what, "%s: the run ends in SYSTEM_OFF", name);
    check(what, (intptr_t)result == HV_EXIT_HALT);
    snprintf(what, sizeof what, "%s: the WFE went to sleep once, not spun", name);
    check(what, machine.stats.wfi == 1);
    snprintf(what, sizeof what, "%s: the sleep ended %s (%llu ticks)", name,
             how == NO_WAKE ? "at the 1 ms net" : "by the wake, not the 10 s net", (unsigned long long)machine.stats.wfi_ticks);
    check(what, how == NO_WAKE ? machine.stats.wfi_ticks >= 20000 && machine.stats.wfi_ticks < 5 * TICKS_PER_S
                               : machine.stats.wfi_ticks < 5 * TICKS_PER_S);
    hv_machine_free(&machine);
    free(code);
}

int main(void) {
    wfe_once(NO_WAKE, "no wake");
    wfe_once(WAKE_WHILE_RUNNING, "woken");
    wfe_once(WAKE_BEFORE, "woken before the WFE");
    printf("WFE sleep: %d failures\n", failures);
    return failures != 0;
}

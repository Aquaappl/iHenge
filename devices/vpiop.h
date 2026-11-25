#ifndef HENGEVM_VPIOP_H
#define HENGEVM_VPIOP_H

#include <stdint.h>

#include "gicv3.h"

/* vresearch101's SEP mailbox ("iop-sep,vpiop"). The AP and the SEP each see
   it at 0x30250000 through their own register bank; one side's outbox is the
   other side's inbox. */
#define HV_VPIOP_DEPTH 16
#define HV_VPIOP_MSG 16

typedef struct HvVpiopQueue {
    uint8_t msg[HV_VPIOP_DEPTH][HV_VPIOP_MSG];
    uint32_t head, count;
} HvVpiopQueue;

typedef struct HvVpiopSide {
    uint32_t ctrl, status, irq_en;
    uint8_t out_msg[HV_VPIOP_MSG];
    HvVpiopQueue inbox; /* messages waiting for this side */
} HvVpiopSide;

typedef struct HvVpiop HvVpiop;

typedef struct HvVpiopBank {
    HvVpiop *v;
    int side;
} HvVpiopBank;

enum { HV_VPIOP_AP, HV_VPIOP_SEP };

/* The state is the AP machine's: guarded by its device lock, which the SEP's
   accesses take too. Neither side takes the SEP machine's device lock: the SEP's
   lines go through its GIC's own mutex, and its CPU is woken directly. */
struct HvVpiop {
    HvVpiopSide side[2];
    HvVpiopBank bank[2];
    HvGic *gic[2];
    int intid_inbox[2], intid_outbox[2];
    int ap_inbox_pending, ap_outbox_pending;
    uint32_t ap_inbox_seen;
    HvMachine *ap_m;
    HvCpu *sep_cpu;  /* woken whenever the AP sends: it waits in WFE */
    int log;
};

/* Maps the AP's bank at pa; inbox/outbox are the AP GIC's INTIDs (0x34, 0x35). */
HvVpiop *hv_vpiop_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid_inbox, int intid_outbox);
/* Maps the SEP's bank at pa in its own machine, with its GIC's INTIDs. */
void hv_vpiop_attach_sep(HvVpiop *v, HvMachine *sep_m, HvGic *sep_gic, HvCpu *sep_cpu, uint64_t pa, int intid_inbox,
                         int intid_outbox);

#endif

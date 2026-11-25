/*
 * vresearch101 SEP mailbox ("iop-sep,vpiop").
 * Ported from qemu-vphone hw/vmapple/vpiop.c (https://github.com/YahiaAngelo/qemu-vphone).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Per bank, from AVPBooter's and AVPSEPBooter's drivers:
 *   0x010  CTRL     bit 0 enable
 *   0x014  STATUS   bit 31 ready (set by writing it); bits 3:0 cause, W1C
 *   0x018  IRQ_EN   bit 4 inbox not empty, bit 5 outbox empty
 *   0x01c  INBOX    bit 17 empty, bit 16 full, 7:0 count
 *   0x020  OUTBOX   bit 17 empty, bit 16 full, 7:0 count
 *   0x100  inbox message (16 bytes), popped by reading its last word
 *   0x200  outbox message (16 bytes), pushed by writing its last word
 * XNU's AppleVPIOP takes the inbox level from INBOX bits 23:20 and from bits
 * 23:20 of each message's last word (1 = the last one), so the AP bank gives both.
 */
#include "vpiop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_CTRL 0x010
#define REG_STATUS 0x014
#define REG_IRQ_EN 0x018
#define REG_INBOX 0x01c
#define REG_OUTBOX 0x020
#define REG_INBOX_MSG 0x100
#define REG_OUTBOX_MSG 0x200

#define STATUS_READY (1u << 31)
#define QUEUE_EMPTY (1u << 17)
#define QUEUE_FULL (1u << 16)
#define IRQ_EN_INBOX (1u << 4)
#define IRQ_EN_OUTBOX (1u << 5)
#define LEVEL_SHIFT 20

static const char *side_name(int side) { return side == HV_VPIOP_AP ? "ap" : "sep"; }

static void set_line(HvGic *g, int intid, int level) {
    if (g && intid) hv_gicv3_set_irq(g, intid, level);
}

static void update_irq(HvVpiop *v) {
    HvVpiopSide *ap = &v->side[HV_VPIOP_AP];
    int inbox = (ap->irq_en & IRQ_EN_INBOX) && ap->inbox.count;
    int outbox = (ap->irq_en & IRQ_EN_OUTBOX) && !v->side[HV_VPIOP_SEP].inbox.count;
    /* XNU enables only INTID 0x34, configured edge, and takes both "reply waiting"
       and "room to send" there: every new event gets its own edge. */
    if ((inbox && (!v->ap_inbox_pending || ap->inbox.count > v->ap_inbox_seen)) || (outbox && !v->ap_outbox_pending))
        set_line(v->gic[HV_VPIOP_AP], v->intid_inbox[HV_VPIOP_AP], 0);
    set_line(v->gic[HV_VPIOP_AP], v->intid_inbox[HV_VPIOP_AP], inbox || outbox);
    set_line(v->gic[HV_VPIOP_AP], v->intid_outbox[HV_VPIOP_AP], outbox);
    v->ap_inbox_pending = inbox;
    v->ap_outbox_pending = outbox;
    v->ap_inbox_seen = ap->inbox.count;

    HvVpiopSide *sep = &v->side[HV_VPIOP_SEP];
    set_line(v->gic[HV_VPIOP_SEP], v->intid_inbox[HV_VPIOP_SEP], (sep->irq_en & IRQ_EN_INBOX) && sep->inbox.count);
    set_line(v->gic[HV_VPIOP_SEP], v->intid_outbox[HV_VPIOP_SEP], (sep->irq_en & IRQ_EN_OUTBOX) && !ap->inbox.count);
}

static uint32_t queue_level(const HvVpiopQueue *q) { return (q->count < 0xf ? q->count : 0xf) << LEVEL_SHIFT; }

static uint32_t queue_status(const HvVpiopQueue *q) {
    uint32_t r = q->count & 0xff;
    if (!q->count) r |= QUEUE_EMPTY;
    if (q->count == HV_VPIOP_DEPTH) r |= QUEUE_FULL;
    return r;
}

static void push(HvVpiop *v, int from) {
    HvVpiopSide *src = &v->side[from];
    HvVpiopQueue *q = &v->side[!from].inbox;
    if (v->log) {
        uint64_t lo, hi;
        memcpy(&lo, src->out_msg, 8);
        memcpy(&hi, src->out_msg + 8, 8);
        fprintf(stderr, "hengevm: vpiop %s->%s %016llx %016llx\n", side_name(from), side_name(!from),
                (unsigned long long)lo, (unsigned long long)hi);
    }
    if (q->count == HV_VPIOP_DEPTH) {
        fprintf(stderr, "hengevm: vpiop: %s outbox overflow\n", side_name(from));
        return;
    }
    memcpy(q->msg[(q->head + q->count) % HV_VPIOP_DEPTH], src->out_msg, HV_VPIOP_MSG);
    q->count++;
    memset(src->out_msg, 0, HV_VPIOP_MSG);
    if (from == HV_VPIOP_AP && v->sep_cpu) hv_cpu_wake(v->sep_cpu);
}

static uint64_t vpiop_read(void *opaque, uint64_t off, unsigned size) {
    HvVpiopBank *b = opaque;
    HvVpiop *v = b->v;
    HvVpiopSide *me = &v->side[b->side];
    uint64_t r = 0;
    if (off == REG_CTRL) r = me->ctrl;
    else if (off == REG_STATUS) r = me->status;
    else if (off == REG_IRQ_EN) r = me->irq_en;
    else if (off == REG_INBOX) r = queue_status(&me->inbox) | (b->side == HV_VPIOP_AP ? queue_level(&me->inbox) : 0);
    else if (off == REG_OUTBOX) r = queue_status(&v->side[!b->side].inbox);
    else if (off >= REG_INBOX_MSG && off + size <= REG_INBOX_MSG + HV_VPIOP_MSG) {
        if (me->inbox.count) {
            uint8_t msg[HV_VPIOP_MSG];
            memcpy(msg, me->inbox.msg[me->inbox.head], HV_VPIOP_MSG);
            if (b->side == HV_VPIOP_AP) {
                uint32_t last;
                memcpy(&last, msg + 12, 4);
                last |= queue_level(&me->inbox);
                memcpy(msg + 12, &last, 4);
            }
            memcpy(&r, msg + (off - REG_INBOX_MSG), size);
            if (off + size == REG_INBOX_MSG + HV_VPIOP_MSG) {
                me->inbox.head = (me->inbox.head + 1) % HV_VPIOP_DEPTH;
                me->inbox.count--;
                update_irq(v);
            }
        }
    } else if (off >= REG_OUTBOX_MSG && off + size <= REG_OUTBOX_MSG + HV_VPIOP_MSG) {
        memcpy(&r, me->out_msg + (off - REG_OUTBOX_MSG), size);
    } else if (v->log) {
        fprintf(stderr, "hengevm: vpiop %s read of unknown register +%llx (size %u)\n", side_name(b->side),
                (unsigned long long)off, size);
    }
    return r;
}

static void vpiop_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvVpiopBank *b = opaque;
    HvVpiop *v = b->v;
    HvVpiopSide *me = &v->side[b->side];
    if (off == REG_CTRL) me->ctrl = (uint32_t)val;
    else if (off == REG_STATUS) {
        me->status |= (uint32_t)val & STATUS_READY;
        me->status &= ~((uint32_t)val & 0xf);
    } else if (off == REG_IRQ_EN) me->irq_en = (uint32_t)val;
    else if (off >= REG_OUTBOX_MSG && off + size <= REG_OUTBOX_MSG + HV_VPIOP_MSG) {
        memcpy(me->out_msg + (off - REG_OUTBOX_MSG), &val, size);
        if (off + size == REG_OUTBOX_MSG + HV_VPIOP_MSG) push(v, b->side);
    } else if (v->log) {
        fprintf(stderr, "hengevm: vpiop %s write of unknown register +%llx = %llx (size %u)\n", side_name(b->side),
                (unsigned long long)off, (unsigned long long)val, size);
    }
    update_irq(v);
}

static uint64_t sep_read(void *opaque, uint64_t off, unsigned size) {
    HvVpiop *v = ((HvVpiopBank *)opaque)->v;
    hv_dev_lock(v->ap_m);
    uint64_t r = vpiop_read(opaque, off, size);
    hv_dev_unlock(v->ap_m);
    return r;
}

static void sep_write(void *opaque, uint64_t off, uint64_t val, unsigned size) {
    HvVpiop *v = ((HvVpiopBank *)opaque)->v;
    hv_dev_lock(v->ap_m);
    vpiop_write(opaque, off, val, size);
    hv_dev_unlock(v->ap_m);
}

void hv_vpiop_attach_sep(HvVpiop *v, HvMachine *sep_m, HvGic *sep_gic, HvCpu *sep_cpu, uint64_t pa, int intid_inbox,
                         int intid_outbox) {
    hv_dev_lock(v->ap_m);
    v->gic[HV_VPIOP_SEP] = sep_gic;
    v->intid_inbox[HV_VPIOP_SEP] = intid_inbox;
    v->intid_outbox[HV_VPIOP_SEP] = intid_outbox;
    v->sep_cpu = sep_cpu;
    hv_dev_unlock(v->ap_m);
    hv_machine_add_mmio(sep_m, pa, 0x1000, &v->bank[HV_VPIOP_SEP], sep_read, sep_write, "vpiop-sep");
    sep_m->mmio[sep_m->nmmio - 1].self_locked = 1; /* it takes the AP's lock, not the SEP's */
}

HvVpiop *hv_vpiop_create(HvMachine *m, HvGic *gic, uint64_t pa, int intid_inbox, int intid_outbox) {
    HvVpiop *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->ap_m = m;
    v->log = getenv("HV_VPIOP_LOG") != NULL;
    for (int i = 0; i < 2; i++) v->bank[i] = (HvVpiopBank){v, i};
    v->gic[HV_VPIOP_AP] = gic;
    v->intid_inbox[HV_VPIOP_AP] = intid_inbox;
    v->intid_outbox[HV_VPIOP_AP] = intid_outbox;
    hv_machine_add_mmio(m, pa, 0x1000, &v->bank[HV_VPIOP_AP], vpiop_read, vpiop_write, "vpiop-ap");
    return v;
}

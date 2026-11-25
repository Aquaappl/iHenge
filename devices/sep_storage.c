/*
 * vresearch101 SEP storage ("VSTR").
 * Ported from qemu-vphone hw/vmapple/sep-storage.c (https://github.com/YahiaAngelo/qemu-vphone).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The layout, read from SEPD's driver:
 *   0x000  ID      reads 1
 *   0x004  TYPE    reads 2 (the virtio block device ID)
 *   0x008  CFG     offset of a virtio_blk_config (here 0x100)
 *   0x010  DONE    non-zero once the last request has completed
 *   0x400  ENABLE  written 1, read back
 *   0x404  STATUS  driver status (written 3)
 *   0x408  NOTIFY  64-bit write: physical address of a descriptor chain
 * Descriptors are 16 bytes: addr (64), len (32), a 16-bit word SEPD sets to 1,
 * then flags (bit 0 next, bit 1 device-writable). Requests complete at once.
 */
#include "sep_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 512
#define REG_CFG_BASE 0x100
#define REG_CFG_END 0x140
#define MAX_DESC 16
#define DESC_NEXT 1
#define BLK_T_IN 0
#define BLK_T_OUT 1
#define BLK_T_FLUSH 4
#define BLK_S_OK 0
#define BLK_S_IOERR 1
#define BLK_S_UNSUPP 2

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
} Desc;

static uint64_t sectors(HvSepStorage *s) { return s->disk ? s->disk->size / SECTOR : 0; }

static uint8_t rw(HvSepStorage *s, int write, uint64_t sector, const Desc *d, int n) {
    uint64_t off = sector * SECTOR, end = sectors(s) * SECTOR;
    for (int i = 0; i < n; i++) {
        void *buf = hv_phys_ptr(s->m, d[i].addr, d[i].len);
        if (!buf || off + d[i].len > end) {
            fprintf(stderr, "hengevm: sep-storage: bad %s (off %llx len %x)\n", write ? "write" : "read",
                    (unsigned long long)off, d[i].len);
            return BLK_S_IOERR;
        }
        if ((write ? hv_disk_write(s->disk, off, buf, d[i].len) : hv_disk_read(s->disk, off, buf, d[i].len)) != 0)
            return BLK_S_IOERR;
        off += d[i].len;
    }
    return BLK_S_OK;
}

static void request(HvSepStorage *s, uint64_t table) {
    Desc d[MAX_DESC];
    int n = 0;
    do {
        uint8_t raw[16];
        if (n == MAX_DESC || hv_phys_read(s->m, table + (uint64_t)n * 16, raw, 16) != 0) {
            fprintf(stderr, "hengevm: sep-storage: bad descriptor chain at %llx\n", (unsigned long long)table);
            return;
        }
        memcpy(&d[n].addr, raw, 8);
        memcpy(&d[n].len, raw + 8, 4);
        memcpy(&d[n].flags, raw + 14, 2);
        n++;
    } while (d[n - 1].flags & DESC_NEXT);
    uint8_t hdr[16];
    if (n < 2 || d[0].len < sizeof hdr || d[n - 1].len < 1 || hv_phys_read(s->m, d[0].addr, hdr, sizeof hdr) != 0) {
        fprintf(stderr, "hengevm: sep-storage: short request (%d descriptors)\n", n);
        return;
    }
    uint32_t type;
    uint64_t sector;
    memcpy(&type, hdr, 4);
    memcpy(&sector, hdr + 8, 8);
    uint8_t status;
    if (type == BLK_T_IN || type == BLK_T_OUT) status = s->disk ? rw(s, type == BLK_T_OUT, sector, &d[1], n - 2) : BLK_S_IOERR;
    else if (type == BLK_T_FLUSH) status = s->disk && hv_disk_flush(s->disk) != 0 ? BLK_S_IOERR : BLK_S_OK;
    else {
        fprintf(stderr, "hengevm: sep-storage: request type %u\n", type);
        status = BLK_S_UNSUPP;
    }
    hv_phys_write(s->m, d[n - 1].addr, &status, 1);
}

static uint64_t storage_read(void *opaque, uint64_t off, unsigned size) {
    HvSepStorage *s = opaque;
    switch (off) {
    case 0x000: return 1;
    case 0x004: return 2;
    case 0x008: return REG_CFG_BASE;
    case 0x010: return s->done;
    case 0x400: return s->enable;
    case 0x404: return s->status;
    default:
        if (off >= REG_CFG_BASE && off + size <= REG_CFG_END) {
            uint8_t cfg[REG_CFG_END - REG_CFG_BASE] = {0};
            uint64_t cap = sectors(s);
            uint32_t blk = SECTOR;
            memcpy(cfg, &cap, 8);          /* capacity */
            memcpy(cfg + 0x14, &blk, 4);   /* blk_size */
            uint64_t v = 0;
            memcpy(&v, cfg + (off - REG_CFG_BASE), size);
            return v;
        }
        fprintf(stderr, "hengevm: sep-storage: read of unknown register +%llx\n", (unsigned long long)off);
        return 0;
    }
}

static void storage_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvSepStorage *s = opaque;
    if (off == 0x400) s->enable = (uint32_t)v;
    else if (off == 0x404) s->status = (uint32_t)v;
    else if (off == 0x408) {
        s->notify = size == 8 ? v : (s->notify & ~0xffffffffull) | (uint32_t)v;
        if (size == 8) {
            s->done = 0;
            request(s, s->notify);
            s->done = 1;
        }
    } else if (off == 0x40c) {
        s->notify = (s->notify & 0xffffffffull) | v << 32;
        s->done = 0;
        request(s, s->notify);
        s->done = 1;
    } else {
        fprintf(stderr, "hengevm: sep-storage: write of unknown register +%llx = %llx\n", (unsigned long long)off,
                (unsigned long long)v);
    }
}

HvSepStorage *hv_sep_storage_create(HvMachine *m, uint64_t pa, HvDisk *disk) {
    HvSepStorage *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m;
    s->disk = disk;
    hv_machine_add_mmio(m, pa, 0x4000, s, storage_read, storage_write, "sep-storage");
    return s;
}

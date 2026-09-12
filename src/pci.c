/*
 * pci.c - see pci.h for an explanation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of the RTL8169P DOS Packet Driver project.
 * Copyright (C) 2026 Yoghoo
 * See pci.h for the full license notice.
 */

#include <i86.h>
#include <string.h>
#include "pci.h"

/* PCI BIOS config-space registers we need */
#define PCI_REG_COMMAND     0x04
#define PCI_REG_BAR0        0x10
#define PCI_REG_BAR1        0x14
#define PCI_REG_INTLINE     0x3C

#define PCICMD_IO_ENABLE    0x0001
#define PCICMD_MEM_ENABLE   0x0002
#define PCICMD_BUSMASTER    0x0004

/* Low level: one call to the PCI BIOS, AH=B1h. Returns AH (status: 0=ok). */
static unsigned char pci_bios_call(union REGS *r)
{
    int86(0x1A, r, r);
    return (unsigned char)((r->w.ax >> 8) & 0xFF);
}

int pci_find_device(unsigned vendor_id, unsigned device_id, pci_device_t *dev)
{
    union REGS r;
    unsigned char status;
    unsigned char bus, devfn;
    unsigned long bar0, bar1;

    memset(dev, 0, sizeof(*dev));

    /* Function B101h: PCI BIOS presence check */
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB101;
    int86(0x1A, &r, &r);
    if (r.x.cflag != 0 || r.h.ah != 0x00) {
        return 0; /* no PCI BIOS found */
    }

    /* Function B102h: Find PCI Device, CX=device id, DX=vendor id, SI=index 0 */
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB102;
    r.w.cx = device_id;
    r.w.dx = vendor_id;
    r.w.si = 0; /* first instance */
    status = pci_bios_call(&r);
    if (status != 0) {
        return 0; /* not found */
    }

    bus   = r.h.bh;
    devfn = r.h.bl;

    dev->bus      = bus;
    dev->dev_func = devfn;
    dev->irq_line = pci_read_cfg8(bus, devfn, PCI_REG_INTLINE);

    bar0 = pci_read_cfg32(bus, devfn, PCI_REG_BAR0);
    bar1 = pci_read_cfg32(bus, devfn, PCI_REG_BAR1);

    /* BAR bit0 == 1  ->  I/O space BAR, address in bits 31:2 (mask off the low 2 bits) */
    /* BAR bit0 == 0  ->  Memory space BAR, address in bits 31:4 (mask off the low 4 bits) */
    if (bar0 & 0x1) {
        dev->io_base = bar0 & 0xFFFFFFFCUL;
    } else {
        dev->mem_base = bar0 & 0xFFFFFFF0UL;
    }
    if (bar1 & 0x1) {
        dev->io_base = bar1 & 0xFFFFFFFCUL;
    } else if (dev->mem_base == 0) {
        dev->mem_base = bar1 & 0xFFFFFFF0UL;
    }

    dev->found = 1;
    return 1;
}

unsigned long pci_read_cfg32(unsigned char bus, unsigned char devfunc, unsigned char reg)
{
    /* PCI BIOS AH=B10Ah (Read Configuration Dword) returns its result in
     * the full 32-bit ECX register. union REGS (16-bit) only exposes the
     * lower half, so this needs inline assembly targeting a 386+ CPU.
     * Compile with -3 (or higher) so 32-bit registers are available. */
    unsigned long result;
    unsigned char b = bus, df = devfunc;
    unsigned rg = reg;  /* 16-bit: goes into DI, not into an 8-bit register */

    _asm {
        push eax
        push ebx
        push ecx
        push edx
        push edi

        mov  ax, 0B10Ah
        mov  bh, b
        mov  bl, df
        xor  edi, edi
        mov  di, rg
        int  1Ah
        mov  dword ptr result, ecx

        pop  edi
        pop  edx
        pop  ecx
        pop  ebx
        pop  eax
    }
    return result;
}

unsigned short pci_read_cfg16(unsigned char bus, unsigned char devfunc, unsigned char reg)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB109;           /* Read Configuration Word */
    r.h.bh = bus;
    r.h.bl = devfunc;
    r.w.di = reg;
    int86(0x1A, &r, &r);
    return r.w.cx;
}

unsigned char pci_read_cfg8(unsigned char bus, unsigned char devfunc, unsigned char reg)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB108;           /* Read Configuration Byte */
    r.h.bh = bus;
    r.h.bl = devfunc;
    r.w.di = reg;
    int86(0x1A, &r, &r);
    return r.h.cl;
}

void pci_write_cfg32(unsigned char bus, unsigned char devfunc, unsigned char reg, unsigned long val)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB10D;           /* Write Configuration Dword */
    r.h.bh = bus;
    r.h.bl = devfunc;
    r.w.di = reg;
    r.w.cx = (unsigned short)(val >> 16);
    r.w.dx = (unsigned short)(val & 0xFFFF);
    int86(0x1A, &r, &r);
}

void pci_write_cfg16(unsigned char bus, unsigned char devfunc, unsigned char reg, unsigned short val)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB10C;           /* Write Configuration Word */
    r.h.bh = bus;
    r.h.bl = devfunc;
    r.w.di = reg;
    r.w.cx = val;
    int86(0x1A, &r, &r);
}

void pci_write_cfg8(unsigned char bus, unsigned char devfunc, unsigned char reg, unsigned char val)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.w.ax = 0xB10B;           /* Write Configuration Byte */
    r.h.bh = bus;
    r.h.bl = devfunc;
    r.w.di = reg;
    r.w.cx = val;
    int86(0x1A, &r, &r);
}

void pci_enable_busmaster(unsigned char bus, unsigned char devfunc)
{
    unsigned short cmd = pci_read_cfg16(bus, devfunc, PCI_REG_COMMAND);
    cmd |= (PCICMD_IO_ENABLE | PCICMD_MEM_ENABLE | PCICMD_BUSMASTER);
    pci_write_cfg16(bus, devfunc, PCI_REG_COMMAND, cmd);
}

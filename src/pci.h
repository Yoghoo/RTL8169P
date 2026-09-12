/*
 * pci.h - Minimal PCI BIOS (INT 1Ah, AH=B1h) wrapper for DOS real mode.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Part of the RTL8169P DOS Packet Driver project.
 * Copyright (C) 2026 Yoghoo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Only what is needed to find and configure the RTL8169:
 *  - pci_find_device()    : search for a vendor/device ID, return bus/dev/func
 *  - pci_read_cfg32/16/8  : read configuration space
 *  - pci_write_cfg32/16/8 : write configuration space
 *  - pci_enable_busmaster : set command register bit2 (bus master) + bit0
 *                           (I/O space) + bit1 (mem space)
 *
 * Based on the PCI BIOS 2.1 specification (int 1Ah, AH=B1h functions).
 * Targets a 16-bit real-mode DOS compiler (Open Watcom: tested with
 * `wcl -3 -ms -bt=dos`).
 */

#ifndef PCI_H
#define PCI_H

#include <dos.h>

#define PCI_VENDOR_REALTEK   0x10ECu
#define PCI_DEVICE_RTL8169   0x8169u

typedef struct {
    unsigned char  bus;
    unsigned char  dev_func;   /* bits 7-3 = device, bits 2-0 = function */
    unsigned char  irq_line;
    unsigned long  io_base;    /* BAR that is I/O space (low bit=1), masked */
    unsigned long  mem_base;   /* BAR that is memory space (low bit=0), masked */
    int            found;
} pci_device_t;

/* Searches the PCI bus for a vendor/device ID. Returns 1 if found, 0 if
 * not present or no PCI BIOS is available. Fills in dev on success. */
int  pci_find_device(unsigned vendor_id, unsigned device_id, pci_device_t *dev);

unsigned long  pci_read_cfg32(unsigned char bus, unsigned char devfunc, unsigned char reg);
unsigned short pci_read_cfg16(unsigned char bus, unsigned char devfunc, unsigned char reg);
unsigned char  pci_read_cfg8 (unsigned char bus, unsigned char devfunc, unsigned char reg);

void pci_write_cfg32(unsigned char bus, unsigned char devfunc, unsigned char reg, unsigned long val);
void pci_write_cfg16(unsigned char bus, unsigned char devfunc, unsigned char reg, unsigned short val);
void pci_write_cfg8 (unsigned char bus, unsigned char devfunc, unsigned char reg, unsigned char val);

/* Sets command register bits: I/O space enable, mem space enable, bus master. */
void pci_enable_busmaster(unsigned char bus, unsigned char devfunc);

#endif /* PCI_H */

/*
 * pktdrv.h - Minimal implementation of the FTP Software / Crynwr "Packet
 * Driver Specification" (v1.11) INT 60h API, for exactly one Ethernet
 * type/handle at a time (sufficient for a single TCP/IP stack such as
 * mTCP or WatTCP - the most common DOS use case).
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
 * NOTE: this is a pragmatic subset, not a full implementation of the
 * whole spec (no multicast list management, no multiple interfaces, no
 * "chaining" to a further INT 60h handler for multiple drivers sharing
 * one interrupt). See README.md "Known limitations".
 */

#ifndef PKTDRV_H
#define PKTDRV_H

#include "rtl8169p.h"

/* Function numbers as the spec expects them in AH for "int <intno>" */
#define PD_FUNC_DRIVER_INFO   0x01
#define PD_FUNC_ACCESS_TYPE   0x02
#define PD_FUNC_RELEASE_TYPE  0x03
#define PD_FUNC_SEND_PKT      0x04
#define PD_FUNC_TERMINATE     0x05
#define PD_FUNC_GET_ADDRESS   0x06
#define PD_FUNC_RESET_IF      0x07
#define PD_FUNC_GET_PARAMS    0x14  /* 20 decimal */
#define PD_FUNC_SET_RCV_MODE  0x20  /* 32 decimal - note: these are the decimal spec numbers, not a hex look-alike */
#define PD_FUNC_GET_STATISTICS 0x18 /* 24 decimal, extended driver function */

/* struct statistics from the spec (section 6.16). "in"/"out" totals are
 * tracked across the driver's single handle (see pktdrv.c); errors_in
 * comes from rtl_dev_t.stat_rx_errors (hardware-level RX drops, see
 * rtl8169p.c). packets_lost covers both "no buffer from receiver()" and
 * RX ring overflow, per the spec's own definition of that field. */
typedef struct {
    unsigned long packets_in;
    unsigned long packets_out;
    unsigned long bytes_in;
    unsigned long bytes_out;
    unsigned long errors_in;
    unsigned long errors_out;
    unsigned long packets_lost;
} pd_statistics_t;

/* Private extension for safely removing THIS driver (not part of the
 * official spec - the spec explicitly reserves AH 0x80-0xFF for
 * "user-developed extensions", see Appendix B). Requires two magic
 * values in BX/CX so a stray AH=0x80 call from something unrelated
 * cannot accidentally remove the driver.
 *
 * On success (carry clear): AX = the driver's PSP segment, ES:DI = a far
 * pointer to an rtl_free_list_t (see below) listing every other DOS
 * segment the caller still needs to free.
 *
 * WHY THIS IS SO ROUNDABOUT: an earlier version had the resident driver
 * itself (i.e. from INSIDE the INT 60h interrupt handler) call
 * INT 21h AH=49h to free its own descriptor rings/buffers. In practice
 * that had NO effect (confirmed with an MCB-chain dump: after
 * "unloading", tx_ring, rx_ring and all packet buffers simply remained
 * owned by the driver's PSP), while freeing the MAIN block (which
 * happens from the separate, freshly started "-u" process, not from the
 * interrupt handler) worked fine. DOS calls made from inside one's own
 * software interrupt handler are apparently not reliable for this kind
 * of operation. Solution: the driver no longer frees anything itself -
 * it only reports which segments need freeing, and the caller (which
 * does run in a normal, safe process context) performs every actual
 * INT 21h AH=49h call itself, exactly as already happened for the main
 * block. */
typedef struct {
    unsigned num_tx_desc;   /* how many of tx_buf_seg[] below are actually valid */
    unsigned num_rx_desc;   /* how many of rx_buf_seg[] below are actually valid */
    unsigned tx_ring_seg;
    unsigned rx_ring_seg;
    unsigned cap_seg;        /* capture ring buffer segment, 0 if capture was never enabled */
    unsigned tx_buf_seg[RTL_MAX_TX_DESC];
    unsigned rx_buf_seg[RTL_MAX_RX_DESC];
} rtl_free_list_t;

/* Private extension for reading the optional raw-frame capture ring
 * buffer (see "-c"/"-d" in main.c, and CAP_* / rtl_capture_*() in
 * rtl8169p.h) from a freshly started "-d" process, without needing a
 * second packet-driver handle - same reasoning and same magic-value
 * safety mechanism as PKTDRV_UNLOAD_AH above. Unlike unloading, this
 * does NOT require the driver to be idle - it only reads state, so it
 * works fine even while an application (e.g. mTCP) still holds the
 * handle, which is the whole point: dump right after a transfer stalls,
 * without needing to disturb whatever is still running.
 *
 * On success (carry clear): ES:DI = far pointer to a cap_dump_info_t
 * (below) describing where the ring buffer lives and how much of it is
 * valid; the "-d" process reads the raw cap_entry_t slots directly from
 * that (still-resident) memory itself rather than copying them through
 * registers one at a time.
 * On failure (carry set, DH=PD_ERR_BAD_COMMAND): capture was never
 * enabled at load time (no "-c" was given). */
#define PKTDRV_CAPDUMP_AH      0x82
#define PKTDRV_CAPDUMP_MAGIC_BX 0xCAF0
#define PKTDRV_CAPDUMP_MAGIC_CX 0xE1D1

typedef struct {
    unsigned cap_seg;
    unsigned cap_num_slots;
    unsigned cap_write_idx;
    unsigned cap_count;
} cap_dump_info_t;

/* On failure (carry set, DH=PD_ERR_CANT_TERMINATE): an application is
 * still registered (access_type() without a matching release_type()) -
 * the driver then refuses to go away, just like reset_interface() does
 * in that situation. */
#define PKTDRV_UNLOAD_AH      0x80
#define PKTDRV_UNLOAD_MAGIC_BX 0x1234
#define PKTDRV_UNLOAD_MAGIC_CX 0x5678

/* Error codes (in DH when carry is set), subset from the spec */
#define PD_ERR_BAD_HANDLE     1
#define PD_ERR_NO_CLASS       2
#define PD_ERR_NO_TYPE        3
#define PD_ERR_NO_NUMBER      4
#define PD_ERR_BAD_TYPE       5
#define PD_ERR_NO_MULTICAST   6
#define PD_ERR_CANT_TERMINATE 7
#define PD_ERR_BAD_MODE       8
#define PD_ERR_NO_SPACE       9
#define PD_ERR_TYPE_INUSE     10
#define PD_ERR_BAD_COMMAND    11
#define PD_ERR_CANT_SEND      12
#define PD_ERR_CANT_SET       13
#define PD_ERR_BAD_ADDRESS    14
#define PD_ERR_CANT_RESET     15

/* if_class value for Ethernet (class 1) - the only one we support */
#define PD_CLASS_ETHERNET     1

/* Installs the driver: hooks both the software interrupt (int_no) and
 * the card's hardware IRQ. Returns 1 on success. */
int  pktdrv_install(rtl_dev_t *rd, unsigned char int_no);

/* Unhooks everything again (used for a clean 'terminate' or if init fails). */
void pktdrv_uninstall(void);

#endif /* PKTDRV_H */

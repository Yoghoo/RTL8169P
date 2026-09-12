/*
 * rtl8169p.h - Register map and descriptor layout for the Realtek RTL8169
 * gigabit ethernet chip.
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
 * Register offsets and initialisation sequence based on:
 *   - Linux drivers/net/ethernet/realtek/r8169_main.c (register names/offsets)
 *   - U-Boot drivers/net/rtl8169.c (init sequence, closest to bare metal)
 *   - OSDev.org RTL8169 wiki page
 *   - Realtek RTL8110S/8169S datasheet v1.3
 */

#ifndef RTL8169_H
#define RTL8169_H

/* ---- I/O registers (offsets from io_base) ------------------------------ */
#define RTL_MAC0            0x00   /* through 0x05, 6-byte MAC address */
#define RTL_MAR0            0x08   /* multicast filter, 8 bytes */
#define RTL_TXDESC_LOW      0x20   /* TX ring physical start address, low dword */
#define RTL_TXDESC_HIGH     0x24
#define RTL_CHIPCMD         0x37   /* byte: reset/RX-enable/TX-enable */
#define RTL_TXPOLL          0x38   /* byte: write 0x40 to kick the TX ring (NPQ) */
#define RTL_INTRMASK        0x3C   /* word */
#define RTL_INTRSTATUS      0x3E   /* word */
#define RTL_TXCONFIG        0x40   /* dword */
#define RTL_RXCONFIG        0x44   /* dword */
#define RTL_EARLYTXTHRES    0xEC   /* byte: "early transmit" threshold, unit of 32 bytes
                                     * (genuine RTL8169; some later 8168/8101 revisions reuse
                                     * this offset differently - "MaxTxPacketSize" - not
                                     * relevant here since we target the classic 8169). */
#define RTL_NOEARLYTX       0x3F   /* EarlyTxThres value: chip waits for the WHOLE frame
                                     * to be in the TX FIFO before transmitting any of it -
                                     * eliminates a FIFO-underrun risk on large frames when
                                     * combined with a small DMA burst size, see the long
                                     * comment at the TxConfig write in rtl8169p.c. */
#define RTL_EARLYTX_MODERATE 0x20  /* EXPERIMENTAL alternative to RTL_NOEARLYTX: threshold
                                     * of 32*32=1024 bytes, matching the DMA burst size fix
                                     * (also 1024 bytes) - the chip starts transmitting once
                                     * one full burst's worth of data is already loaded,
                                     * giving the (now much faster) DMA engine a head start
                                     * on the rest of the frame instead of racing it from
                                     * byte zero. Try this in place of RTL_NOEARLYTX ONLY
                                     * after re-verifying both large AND small transfers are
                                     * still correct - the DMA burst size fix is what makes
                                     * this plausible now, but it has not been separately
                                     * confirmed safe the way RTL_NOEARLYTX has. */
#define RTL_CFG9346         0x50   /* byte: EEPROM/config lock (0xC0=unlock) */
#define RTL_CONFIG0         0x51
#define RTL_CONFIG1         0x52
#define RTL_CONFIG2         0x53
#define RTL_CONFIG3         0x54
#define RTL_CONFIG4         0x55
#define RTL_CONFIG5         0x56
#define RTL_PHYAR           0x60   /* dword: PHY access (MDIO via register, no discrete MDIO pins) */
#define RTL_PHYSTATUS       0x6C   /* byte: link/speed/duplex status */
#define RTL_RXMAXSIZE       0xDA   /* word: max receive frame size */
#define RTL_CPLUSCMD        0xE0   /* word: C+ mode command register */
#define RTL_INTRMITIGATE    0xE2   /* word: interrupt coalescing, see -m in main.c and
                                     * RTL_DEFAULT_INTR_MITIGATE below (default: enabled,
                                     * 0x5151 - see that comment for why, including the
                                     * Linux kernel history this value comes from). Four
                                     * 4-bit fields: bits 15-12 TX_USECS, 11-8 TX_FRAMES,
                                     * 7-4 RX_USECS, 3-0 RX_FRAMES. FRAMES fields are
                                     * scaled by 4 (max field value 0xF = 60 frames);
                                     * USECS fields are scaled by a time unit that
                                     * depends on BOTH link speed and RTL_CPLUSCMD bits
                                     * [1:0] - see the long comment at
                                     * RTL_DEFAULT_INTR_MITIGATE for the actual table. */
#define RTL_RXDESC_LOW      0xE4   /* RX ring physical start address, low dword */
#define RTL_RXDESC_HIGH     0xE8

/* ChipCmd (0x37) bits */
#define CMD_RESET           0x10
#define CMD_RXENB           0x08
#define CMD_TXENB           0x04

/* IntrStatus/IntrMask (0x3C/0x3E) bits - only the ones we use */
#define INT_ROK             0x0001  /* receive OK */
#define INT_RER             0x0002  /* receive error */
#define INT_TOK             0x0004  /* transmit OK */
#define INT_TER             0x0008  /* transmit error */
#define INT_RXOVW           0x0010  /* rx buffer/ring overflow */
#define INT_RXFIFO_OVW      0x0040
#define INT_SERR            0x8000  /* PCI bus error */

/* RX descriptor status bits (opts1, high half) - confirmed against
 * drivers/net/ethernet/realtek/r8169_main.c. If any of these bits is set
 * on a received frame, the frame is bad and must NOT be handed to the
 * application (only return the buffer to the NIC). */
#define RTL_RX_RWT          0x00400000UL  /* Receive Watchdog Timer expired (frame too long) */
#define RTL_RX_RES          0x00200000UL  /* Receive Error Summary - set if anything went wrong */
#define RTL_RX_RUNT         0x00100000UL  /* Runt frame (too short) */
#define RTL_RX_CRC          0x00080000UL  /* CRC error */
#define RTL_RX_ERROR_MASK   (RTL_RX_RWT | RTL_RX_RES | RTL_RX_RUNT | RTL_RX_CRC)

/* Cfg9346 (0x50) values */
#define CFG9346_LOCK        0x00
#define CFG9346_UNLOCK      0xC0

/* CPlusCmd (0xE0) - only set what is needed for descriptor mode;
 * checksum-offload/VLAN bits are left at 0 (off). */
#define CPCMD_RXCHKSUM      0x0020  /* left off */
#define CPCMD_MULRW         0x0008  /* PCI multiple read/write, recommended on */

/* PHYAR (0x60): bit31=1 write, bit31=0 read after issuing; bits 16-30=result/addr */
#define PHYAR_FLAG          0x80000000UL

/* MII/PHY registers accessed via PHYAR (standard MII register numbers) */
#define MII_PAGE_SELECT     0x1F   /* Vendor-specific page select - MUST be 0
                                     * (default page) to access the standard
                                     * IEEE registers below. Realtek's own
                                     * Linux driver (r8169) explicitly writes
                                     * 0x0000 here right before every speed/
                                     * autoneg configuration step - without
                                     * this, writes to e.g. register 9 (GBCR)
                                     * can land on a completely different,
                                     * vendor-specific register set if the PHY
                                     * happens to be on another page. */
#define MII_BMCR            0x00   /* Basic Mode Control Register */
#define MII_BMSR            0x01   /* Basic Mode Status Register */

/* BMSR bits (standard IEEE 802.3, not RTL8169-specific). Important:
 * "Auto-Negotiation Complete" is the only reliable indicator that the
 * FULL negotiation - including the slower gigabit master/slave training
 * phase - has actually finished. The RTL8169-specific PHYstatus.LinkStatus
 * bit (see PHYSTATUS_LINK below) was observed to go high during an
 * intermediate state, before gigabit training had completed - which is
 * why rtl_wait_for_link() now waits on THIS bit instead of polling
 * PHYstatus directly. */
#define BMSR_AUTONEG_COMPLETE 0x0020u  /* bit5 */
#define BMSR_LINK_STATUS      0x0004u  /* bit2 */
#define MII_ANAR            0x04   /* Auto-Negotiation Advertisement Register */
#define MII_GBCR            0x09   /* 1000BASE-T Control Register (also known
                                     * as CTRL1000) - a SEPARATE register page
                                     * from ANAR; gigabit over copper is
                                     * negotiated via IEEE 802.3 clause 40
                                     * (master/slave resolution), not via the
                                     * plain ANAR which only covers 10/100. */

/* BMCR bits to force a fixed 100 Mbit / full duplex link, autoneg OFF:
 *   bit12 Autoneg Enable = 0
 *   bit13 Speed Select (LSB) = 1  (together with bit6=0 => 100 Mbit)
 *   bit8  Duplex Mode = 1 (full duplex)
 * => value 0x2100
 *
 * KEPT as a standalone option (no longer the default init path) - forcing
 * the speed turned out to carry a duplex-mismatch risk in practice: a
 * switch port that itself auto-negotiates can, via "parallel detection",
 * recognise the speed (100M) but not the duplex mode, and then falls back
 * to half duplex while we force full duplex. See BMCR_AUTONEG_RESTART
 * below for the current default approach. */
#define BMCR_FORCE_100_FULL 0x2100u

/* BMCR bits for auto-negotiation (now the default approach): */
#define BMCR_AUTONEG_ENABLE  0x1000u  /* bit12 */
#define BMCR_AUTONEG_RESTART 0x0200u  /* bit9 - "restart" (not just "enable") forces a fresh negotiation */

/* ANAR value: advertise 10/100 half+full duplex (802.3 selector field +
 * bits 5-8). No pause frames (keeps it simple). Gigabit is advertised
 * separately via MII_GBCR below - it is not covered by this register. */
#define ANAR_10_100_ALL      0x01E1u

/* GBCR value: advertise 1000BASE-T full duplex (bit9). Bit8 (half duplex)
 * is left out - 1000BASE-T half duplex is essentially never supported by
 * real hardware and would only cause confusion if it were ever
 * negotiated by accident. */
#define GBCR_ADV_1000_FULL   0x0200u

/* PHYstatus (0x6C) bits - confirmed against r8169_main.c. Byte register. */
#define PHYSTATUS_1000BPSF   0x10   /* 1000 Mbit full duplex active */
#define PHYSTATUS_100BPS     0x08   /* 100 Mbit active */
#define PHYSTATUS_10BPS      0x04   /* 10 Mbit active */
#define PHYSTATUS_LINK       0x02   /* link up */
#define PHYSTATUS_FULLDUP    0x01   /* full duplex */

/* ---- Descriptor ring ----------------------------------------------------
 * 16 bytes per descriptor. The OWN bit (bit31 of opts1) toggles ownership
 * between the driver (0) and the NIC (1). EOR (bit30) marks the last
 * descriptor in the ring (the chip then wraps back to descriptor 0).
 *
 * IMPORTANT for DOS real mode: this is a PHYSICAL address (segment<<4 +
 * offset), not a "virtual" address - this is precisely why real mode has
 * it easier here than an OS with paging. Make sure the ring itself sits
 * at a physical address that is a multiple of 256 bytes (chip requirement).
 */
#define RTL_DESC_OWN        0x80000000UL
#define RTL_DESC_EOR        0x40000000UL
#define RTL_DESC_FS         0x20000000UL   /* First Segment (TX) */
#define RTL_DESC_LS         0x10000000UL   /* Last Segment (TX) */

/* Confirmed against Linux's drivers/net/ethernet/realtek/r8169_main.c:
 *   - DescOwn/RingEnd/FirstFrag/LastFrag sit at exactly these bit positions.
 *   - "#define RsvdMask 0x3fffc000" reserves bits 14-29, so the length
 *     field occupies bits 0-13 (14 bits) - for BOTH TX and RX.
 *   - Strongest confirmation: the driver sets R8169_RX_BUF_SIZE to
 *     "SZ_16K - 1" = 16383 = 0x3FFF, which only makes sense for a
 *     14-bit field. */
#define RTL_DESC_LEN_MASK   0x00003FFFUL

#pragma pack(push, 1)
typedef struct {
    unsigned long opts1;    /* OWN|EOR|FS|LS|length, or for RX: OWN|EOR|len */
    unsigned long opts2;    /* VLAN tag etc. - unused here, leave at 0 */
    unsigned long addr_lo;  /* physical buffer address, low dword */
    unsigned long addr_hi;  /* physical buffer address, high dword (0 in DOS/<4GB) */
} rtl_desc_t;
#pragma pack(pop)

/* Defaults history: TX is paced by OUR OWN send_pkt() calls, not by
 * uncontrolled arriving traffic - a full TX ring just means rtl_send()'s
 * calibrated wait (up to 2ms) engages a little more often, which sounds
 * harmless, but turned out to matter more than expected: mTCP's own
 * send_pkt() retry budget (see Packet.cpp) is limited (a handful of
 * attempts with a brief random delay) - if the ring stays full long
 * enough to exhaust THAT budget too, mTCP itself reports a real,
 * visible send failure (its own stats, not just this driver's), even
 * though this driver never silently drops anything. Measured directly:
 * -t 2 showed real loss in mTCP's own reporting; -t 4 did not (only a
 * negligible 3 lost out of tens of thousands at the driver level, and
 * nothing in mTCP's own counters) - so 4 is the confirmed default, not
 * 2. RX history is 32 -> 8 (memory optimisation) -> 16 (8 caused
 * measured RX ring overflow under FTP load) -> 12 (re-tested after the
 * TX DMA-burst/EarlyTxThres fix made the whole TX path more efficient)
 * -> 8 (re-tested again after the burst-gap fix - see README - with
 * -t 4 specifically, confirmed negligible loss (3 out of tens of
 * thousands) at both this driver's level and mTCP's own reporting).
 * Deeper is not simply "safer": a deeper ring lets rtl_poll_rx() hand
 * mTCP a LARGER uninterrupted burst per interrupt (it drains the whole
 * ring before returning), and mTCP's own receive-buffer pool is a
 * separate, fixed-size resource from this ring - a big enough burst can
 * exhaust THAT pool even when this driver's own hardware ring never
 * overflows, which is why testing sometimes showed MORE loss at a
 * higher -r than a lower one. If your own traffic pattern needs more
 * headroom either way, pass a higher -t/-r value rather than editing
 * these defaults. */
#define RTL_MAX_TX_DESC      32
#define RTL_MAX_RX_DESC      32
#define RTL_DEFAULT_TX_DESC  4
#define RTL_DEFAULT_RX_DESC  8

/* Delay between bursty, back-to-back RX frames within a single poll -
 * see the long comment at its only use, in rtl_poll_rx() (rtl8169p.c),
 * for what this fixes and why. There was no such delay, at any value,
 * before this fix existed (effectively 0).
 *
 * Value history: 300us was the first value tried, confirmed via ~20
 * repeated real-world transfers of a file that had reliably reproduced
 * the corruption before this fix existed (see DESIGN.md). Lowered to
 * 100us next, also tested and confirmed stable (10us was directly
 * confirmed NOT enough on its own). Lowered again to 0 (this delay
 * effectively OFF) as the new default once paired with
 * RTL_DEFAULT_INTR_MITIGATE below - interrupt mitigation on its own
 * batches RX frames without needing this driver to add its own gap,
 * confirmed stable via the same repeated-transfer rigour (~20+ transfers
 * of the same file, across 6 full power-cycles/reloads). RTL_MAX_BURST_
 * GAP_US is just a sanity ceiling for the "-g" command-line option
 * (main.c), not a tested or meaningful value in itself.
 *
 * IMPORTANT: this 0 default is only safe TOGETHER WITH
 * RTL_DEFAULT_INTR_MITIGATE also being at its own (non-zero) default -
 * see the long comment there. Disabling BOTH protections at once
 * ("-g 0 -m 0") reverts to the original, confirmed-buggy combination.
 * main.c checks for exactly that combination and warns. RTL_BURST_GAP_
 * UNSET is a SEPARATE sentinel, passed by main.c when "-g" was not given
 * at all, to mean "use the default" without colliding with a deliberate
 * "-g 0" (0 is itself a valid, meaningful value here, unlike
 * num_tx_desc/num_rx_desc where 0 is never valid and so safely doubles
 * as "use the default"). */
#define RTL_DEFAULT_BURST_GAP_US 0
#define RTL_MAX_BURST_GAP_US     5000
#define RTL_BURST_GAP_UNSET      0xFFFFu

/* Default 0x5151 (TX_USECS=5, TX_FRAMES=1, RX_USECS=5, RX_FRAMES=1) -
 * the raw 16-bit value written directly to RTL_INTRMITIGATE (0xE2).
 * Paired with RTL_DEFAULT_BURST_GAP_US = 0 above: interrupt mitigation
 * itself now batches RX frames, so this driver's own delay-based
 * protection is no longer needed on top of it. Confirmed stable via
 * ~20+ repeated real-world transfers of a file that had reliably
 * reproduced the original corruption, across 6 full power-cycles/
 * reloads, with zero loss at both the driver level (-s) and mTCP's own
 * reporting.
 *
 * Why mitigation, not just a delay, on weak retro CPUs specifically:
 * every interrupt has a largely CPU-speed-independent fixed overhead
 * (entry/exit, this driver's two-stage upcall, PIC acknowledge) that
 * weighs proportionally far more on something like a 466MHz Celeron
 * than on modern hardware - fewer, larger interrupts is a genuine
 * efficiency win there, which is the whole point of coalescing as a
 * technique. This value was originally found in the older Linux
 * r8169.c's RTL8168 (not RTL8169) init path; upstream later changed it
 * to 0x5100 (RX_USECS/RX_FRAMES zeroed, TX-only) after a report that
 * combining RX coalescing with ASPM (PCIe active-state power
 * management) caused increased packet latency under Linux. That
 * specific concern is Linux/ASPM-driver-state specific and has not been
 * observed to apply here - DOS has no equivalent active ASPM management
 * - and real-world testing on this driver (above) found the RX-
 * inclusive 0x5151 stable, so it is kept as the default rather than the
 * later, RX-excluded 0x5100. 0x5100 remains a documented, known
 * alternative (see the README) for anyone who wants to compare.
 *
 * Setting this to 0 (disabled) requires ALSO setting "-g" to a non-zero
 * value (see RTL_DEFAULT_BURST_GAP_US above) - main.c warns if both
 * protections are disabled at once, since that combination is
 * confirmed to reproduce the original data-corruption bug.
 *
 * Field layout (see the long comment at RTL_INTRMITIGATE above for bit
 * positions): FRAMES fields are scaled by 4 (max 0xF = 60 frames).
 * USECS fields are scaled by a time unit that depends on BOTH link
 * speed and RTL_CPLUSCMD bits [1:0] - for the classic RTL8169
 * specifically (confirmed against the Linux kernel's own coalescing
 * table for this chip):
 *
 *   CPlusCmd[1:0] \ speed   1000M      100M       10M
 *   00                      320ns      2.56us     40.96us
 *   01                      2.56us     20.48us    327.7us
 *   10                      5.12us     40.96us    655.4us
 *   11                      10.24us    81.92us    1.31ms
 *
 * e.g. at 1000Mbit with CPlusCmd[1:0]=00, a USECS field of 0xF (max)
 * means 15*320ns = 4.8us of held-back time before that half of the
 * mitigation timer forces an interrupt (frame count threshold, if hit
 * first, still applies independently). */
#define RTL_DEFAULT_INTR_MITIGATE 0x5151

#define RTL_BUF_SIZE        1536   /* enough for a standard Ethernet frame plus margin */

typedef struct {
    unsigned io_base;                          /* PCI I/O BAR (16-bit port in DOS) */
    unsigned char irq_line;
    unsigned char mac[6];

    unsigned num_tx_desc;   /* actual TX descriptor count in use (<= RTL_MAX_TX_DESC) */
    unsigned num_rx_desc;   /* actual RX descriptor count in use (<= RTL_MAX_RX_DESC) */
    unsigned burst_gap_us;  /* delay between bursty RX frames, see RTL_DEFAULT_BURST_GAP_US */
    unsigned intr_mitigate; /* raw IntrMitigate register value, see RTL_DEFAULT_INTR_MITIGATE */

    /* Descriptor rings and data buffers - MUST sit at a fixed physical
     * address for as long as the card is active (no paging in DOS, so
     * this is simple: plain static/normalised far buffers). Arrays are
     * sized at the fixed MAXIMUM but only the first num_tx_desc/
     * num_rx_desc entries are actually allocated/used - see above. */
    rtl_desc_t far *tx_ring;
    rtl_desc_t far *rx_ring;
    unsigned tx_ring_seg; /* raw DOS segment backing tx_ring - freed by the caller, see PKTDRV_UNLOAD_AH in pktdrv.h */
    unsigned rx_ring_seg; /* same for rx_ring */
    unsigned char far *tx_buf[RTL_MAX_TX_DESC];
    unsigned char far *rx_buf[RTL_MAX_RX_DESC];

    unsigned tx_cur;   /* next TX descriptor index to use */
    unsigned rx_cur;   /* next RX descriptor index expected */

    /* Link status after auto-negotiation (filled in by an explicit
     * rtl_wait_for_link() call - the caller in main.c calls this AFTER
     * pktdrv_install() so the hardware IRQ is unmasked first; rtl_reinit()
     * calls it directly since the IRQ is already unmasked by then).
     * link_up=0 means: no link found within the timeout - the card may
     * still be working fine, there is just physically nothing plugged
     * into the other end of the cable/port. */
    int      link_up;
    unsigned link_speed_mbit;   /* 10, 100 or 1000 - only meaningful if link_up */
    int      link_full_duplex;

    /* Count of RX frames dropped at the hardware level (CRC error, runt,
     * watchdog timeout - see RTL_RX_ERROR_MASK) - exposed to the
     * application via the packet driver's get_statistics() as
     * "errors_in". Incremented in rtl_poll_rx(). */
    unsigned long stat_rx_errors;

    /* Optional raw-frame capture ring buffer (see "-c"/"-d" in main.c
     * and CAP_* below) - only allocated if capture was requested at
     * load time (cap_enabled != 0); costs nothing otherwise. Held as
     * ONE raw DOS segment (not through the normal far-heap machinery -
     * same reasoning as tx_ring_seg/rx_ring_seg, see dos_alloc_seg() in
     * rtl8169p.c) so it can be freed cleanly on unload and addressed
     * with a single, non-normalised far pointer (offset always < 64KB,
     * hence the buffer is capped at CAP_MAX_KB). */
    unsigned cap_enabled;
    unsigned cap_seg;         /* raw DOS segment backing the ring buffer, 0 if not allocated */
    unsigned cap_num_slots;
    unsigned cap_write_idx;   /* next slot to write (also the OLDEST valid entry once wrapped) */
    unsigned cap_count;       /* number of valid entries so far, saturates at cap_num_slots */
} rtl_dev_t;

/* ---- optional raw-frame capture (diagnostic only) -----------------------
 *
 * A small, fixed-size ring of recently seen frames (TX and RX), kept
 * entirely inside the driver so a "-d" dump can be taken from a freshly
 * started process without needing a second packet-driver handle (DOS is
 * single-tasking and this driver only supports one handle at a time
 * anyway - see "Known limitations" in the README). Each slot stores a
 * timestamp (BIOS clock ticks since midnight, ~55ms resolution - good
 * enough to see multi-second stalls, not intended for sub-millisecond
 * timing analysis), which direction, the true frame length, and up to
 * CAP_SNAPLEN bytes of the frame itself.
 *
 * CAP_SNAPLEN covers a FULL standard Ethernet frame (matches
 * RTL_BUF_SIZE), not just the headers. This used to be 128 bytes
 * (headers only, more entries per KB) - raised to capture complete
 * frames after needing to independently verify a TCP segment's checksum
 * against what this driver's own hardware actually received (as
 * opposed to what a THIRD-PARTY capture on the sending machine shows,
 * which cannot rule out a sender-side checksum-offload artifact making
 * an otherwise-valid outgoing packet look "corrupt" in that capture -
 * see the README for the investigation this came out of). The
 * trade-off: fewer entries fit in the same CAP_MAX_KB budget (roughly
 * 42 at the 64KB maximum, vs. ~485 with the old 128-byte snaplen) -
 * still plenty for catching a stall shortly after enabling capture.
 *
 * Capped at a single 64KB DOS segment (CAP_MAX_KB) so every slot can be
 * addressed with one un-normalised far pointer (segment fixed, offset
 * always < 65536) - no multi-segment ring-buffer bookkeeping needed. */
#define CAP_SNAPLEN   1536
#define CAP_MAX_KB    64

#pragma pack(push, 1)
typedef struct {
    unsigned long tick;        /* BIOS tick count (INT 1Ah AH=00h) at capture time */
    unsigned char direction;   /* 0 = RX, 1 = TX */
    unsigned      len;         /* true frame length (may exceed CAP_SNAPLEN) */
    unsigned char data[CAP_SNAPLEN];
} cap_entry_t;
#pragma pack(pop)

/* Allocates the ring buffer (size_kb, 1-CAP_MAX_KB) and enables capture.
 * Safe to call with size_kb==0, which leaves capture disabled (the
 * no-cost default). Returns 1 on success, 0 on allocation failure (out
 * of memory) or an out-of-range size_kb - either way capture stays off,
 * this is never treated as a fatal driver-init error. */
int rtl_capture_init(rtl_dev_t *rd, unsigned size_kb);

/* Records one frame (TX or RX) into the ring buffer if capture is
 * enabled; a silent no-op otherwise. Called from rtl_send() and
 * rtl_poll_rx() - see rtl8169p.c. */
void rtl_capture_frame(rtl_dev_t *rd, unsigned char direction, const void far *data, unsigned len);

/* Callback invoked by the driver whenever a complete, valid frame has
 * been received. buf/len point into the driver's own rx_buf[] - the
 * receiver (pktdrv.c) must copy the data immediately, since the buffer
 * is handed back to the NIC right after the callback returns. */
typedef void (*rtl_rx_callback_t)(unsigned char far *buf, unsigned len);

int  rtl_probe_and_init(rtl_dev_t *rd, unsigned num_tx_desc, unsigned num_rx_desc, unsigned burst_gap_us, unsigned intr_mitigate); /* PCI detection + full init; 0 for any of num_tx_desc/num_rx_desc/burst_gap_us means "use the default"; intr_mitigate is used as-is (0 = disabled, its own natural default - see RTL_DEFAULT_INTR_MITIGATE) */
int  rtl_reinit(rtl_dev_t *rd);                   /* lightweight re-init for reset_interface(), no realloc */

/* Blocks (with a timeout) until the link comes up after auto-negotiation.
 * Fills in rd->link_up/link_speed_mbit/link_full_duplex directly in rd
 * (handy for repeated polling), and returns that same link_up value for
 * convenience. timeout_ms is in milliseconds. */
int  rtl_wait_for_link(rtl_dev_t *rd, unsigned timeout_ms);
int  rtl_send(rtl_dev_t *rd, const void far *data, unsigned len);
void rtl_poll_rx(rtl_dev_t *rd, rtl_rx_callback_t cb); /* call from the ISR or a polling loop */

/* status_out MUST be a far pointer: this function is called from hw_isr()
 * (an __interrupt function), and Watcom treats the address of a local
 * variable inside such a function as far (SS-relative, not necessarily
 * equal to the default data segment). A near parameter here would
 * silently truncate the segment part (compiler warning W112 "Pointer
 * truncated") and could end up writing to the wrong place in memory. */
void rtl_ack_and_get_status(rtl_dev_t *rd, unsigned far *status_out);
void rtl_stop(rtl_dev_t *rd); /* disable RX/TX + mask interrupts - MUST be called before releasing descriptor memory on unload */
void rtl_get_mac(rtl_dev_t *rd, unsigned char mac[6]);

/* No rtl_free_buffers() here: actually freeing tx_ring_seg/rx_ring_seg/
 * tx_buf[]/rx_buf[] now happens exclusively from the calling "-u"
 * instance (main.c), via the rtl_free_list_t returned by pktdrv.c - see
 * PKTDRV_UNLOAD_AH in pktdrv.h. */

#endif /* RTL8169_H */

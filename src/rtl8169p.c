/*
 * rtl8169p.c - see rtl8169p.h for the register map and assumptions.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of the RTL8169P DOS Packet Driver project.
 * Copyright (C) 2026 Yoghoo
 * See rtl8169p.h for the full license notice.
 *
 * Fixed 10/100/1000 Mbit auto-negotiation, no jumbo frames, no offloads.
 * Everything is single-threaded/polled from the packet-driver ISR - there
 * is no multitasking in DOS, so no locking is needed, but the ISR must
 * stay fast (see pktdrv.c).
 */

#include <dos.h>
#include <conio.h>
#include <string.h>
#include "rtl8169p.h"
#include "pci.h"

/* ---- small helpers ------------------------------------------------------ */

/* Physical address (20-32 bit) of a far pointer in real mode. */
static unsigned long phys_addr(void far *p)
{
    return ((unsigned long)FP_SEG(p) << 4) + (unsigned long)FP_OFF(p);
}

/* Reconstructs a far pointer from a physical address, normalised so that
 * offset < 16 (the segment carries almost all of the value). Works for
 * any physical address up to 1MB+15 (plenty for conventional DOS memory). */
static void far *far_from_phys(unsigned long phys)
{
    unsigned seg = (unsigned)(phys >> 4);
    unsigned off = (unsigned)(phys & 0xF);
    return MK_FP(seg, off);
}

/* Calibrated microsecond delay via the 8254 PIT (timer chip), channel 0 -
 * the same chip that drives the system clock tick. We do NOT disturb
 * anything: channel 0 is already counting down at 1.193182 MHz for the
 * 18.2 Hz tick, and the latch command (0x00 to port 0x43) only reads the
 * current count without reprogramming the timer.
 *
 * Needed because Linux's r8169_mdio_write/read use a REAL udelay(25) per
 * polling iteration (20 attempts, so max 500us) - a busy loop without a
 * calibrated duration would give no timing guarantee: on a slow or fast
 * machine that could be respectively too short or needlessly long.
 */
static void udelay_dos(unsigned us)
{
    unsigned long target_ticks = ((unsigned long)us * 1193182UL) / 1000000UL;
    unsigned long elapsed = 0;
    unsigned prev, cur;

    outp(0x43, 0x00);           /* latch the current channel-0 count */
    prev = (unsigned)inp(0x40);
    prev |= (unsigned)inp(0x40) << 8;

    while (elapsed < target_ticks) {
        outp(0x43, 0x00);
        cur = (unsigned)inp(0x40);
        cur |= (unsigned)inp(0x40) << 8;
        /* the counter counts DOWN; compute the delta, with wrap-around
         * handling for when the counter has "rolled over". */
        elapsed += (prev >= cur) ? (prev - cur) : (prev + (65536UL - cur));
        prev = cur;
    }
}

/* ---- Direct DOS memory allocation, instead of Watcom's _fmalloc()/_ffree() -
 *
 * IMPORTANT LESSON (learned after a memory leak that persisted despite
 * correct _ffree() calls on every buffer): Watcom's far-heap manager
 * (_fmalloc/_ffree) apparently keeps its own, internally managed arena -
 * one or a few large DOS memory blocks that it subdivides for individual
 * _fmalloc() requests. _ffree() then only marks a piece as "reusable
 * within that arena"; the underlying DOS block itself is only returned
 * at real process exit - which, for a TSR that stays resident forever,
 * never happens. As a result, unload reported success everywhere, but
 * part of the memory stayed allocated.
 *
 * Fix: no more Watcom abstraction layer - call DOS INT 21h AH=48h
 * (allocate) / AH=49h (free) directly. This way we know EXACTLY which
 * DOS segment we own, and can hand back that exact segment later - no
 * hidden runtime behaviour left in the way.
 */

static unsigned dos_alloc_seg(unsigned paragraphs)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x48;
    r.w.bx = paragraphs;
    int86(0x21, &r, &r);
    return r.x.cflag ? 0 : r.w.ax; /* 0000h is never a valid DOS segment for an allocated block */
}

/* No dos_free_seg() here: the actual freeing now happens exclusively from
 * the caller (main.c's do_unload()), not from the driver itself - see the
 * detailed explanation at PKTDRV_UNLOAD_AH in pktdrv.h for why (DOS calls
 * made from inside our own interrupt handler turned out to have no
 * effect). */

/* Allocates a block whose PHYSICAL address is a multiple of 'align'
 * bytes - needed for the 256-byte-aligned descriptor rings. raw_seg_out
 * receives the RAW, unmodified DOS segment (needed to free the block
 * later); the return value is the ALIGNED far pointer that the hardware
 * actually uses - these are NOT necessarily the same address, so do not
 * confuse them when freeing. */
static void far *dos_alloc_aligned(unsigned long size, unsigned long align, unsigned *raw_seg_out)
{
    unsigned long need_bytes = size + align;
    unsigned paragraphs = (unsigned)((need_bytes + 15UL) / 16UL);
    unsigned raw_seg;
    unsigned long raw_phys, aligned_phys;

    raw_seg = dos_alloc_seg(paragraphs);
    if (raw_seg == 0) {
        if (raw_seg_out) *raw_seg_out = 0;
        return NULL;
    }
    if (raw_seg_out) *raw_seg_out = raw_seg;

    raw_phys = (unsigned long)raw_seg << 4;
    aligned_phys = (raw_phys + (align - 1)) & ~(align - 1);

    return far_from_phys(aligned_phys);
}

/* Simple buffer with no alignment requirement (offset is always 0 in the
 * returned segment) - freeing it only needs FP_SEG() on the returned
 * pointer, so no separate "raw" storage is needed like with
 * dos_alloc_aligned(). */
static unsigned char far *dos_alloc_buf(unsigned size_bytes)
{
    unsigned paragraphs = (unsigned)(((unsigned long)size_bytes + 15UL) / 16UL);
    unsigned seg = dos_alloc_seg(paragraphs);
    if (seg == 0) return NULL;
    return (unsigned char far *)MK_FP(seg, 0);
}

/* ---- optional raw-frame capture (diagnostic only, see rtl8169p.h) -------- */

/* BIOS timer tick count since midnight (INT 1Ah AH=00h), ~18.2065 ticks/
 * second - a plain DOS API call, safe to use from normal (non-interrupt)
 * context. Used only for capture timestamps; ~55ms resolution is coarse
 * but entirely adequate for spotting multi-second stalls, which is what
 * this feature is for. */
static unsigned long get_bios_ticks(void)
{
    /* Direct read from the BIOS Data Area (0040:006C) instead of calling
     * INT 1Ah AH=00h. This function is called from rtl_capture_frame(),
     * itself called from rtl_send()/rtl_poll_rx() while running INSIDE
     * this driver's own interrupt handlers (pktdrv_isr/hw_isr) - and
     * this project already learned the hard way, elsewhere, that DOS/
     * BIOS calls made from inside our own interrupt handler are NOT
     * reliable (see the long comment at PKTDRV_UNLOAD_AH in pktdrv.h for
     * the INT 21h case that started that investigation). Calling
     * INT 1Ah from in here is exactly the same category of mistake -
     * this is almost certainly the real explanation for the corrupted
     * capture timestamps seen in testing (an earlier _fmemcpy()-based
     * "alignment" fix did not help, which fits: the problem was never
     * about how the value was stored, but about the value being wrong
     * to begin with at the point it was read). A direct memory read is
     * not a software interrupt at all, so it sidesteps this whole class
     * of problem entirely. Read as a single 32-bit access (this project
     * already requires a 386+ target, see -3 in the Makefile) rather
     * than two separate 16-bit halves, which also avoids a possible torn
     * read if a timer tick lands between two separate reads. */
    return *(unsigned long far *)MK_FP(0x0040, 0x006C);
}

/* TRANSIENT - only called once, from main(), at load time. */
int rtl_capture_init(rtl_dev_t *rd, unsigned size_kb)
{
    unsigned num_slots;
    unsigned long total_bytes;

    rd->cap_enabled   = 0;
    rd->cap_seg       = 0;
    rd->cap_num_slots = 0;
    rd->cap_write_idx = 0;
    rd->cap_count     = 0;

    if (size_kb == 0) return 1; /* capture simply not requested - not an error */
    if (size_kb > CAP_MAX_KB) return 0;

    total_bytes = (unsigned long)size_kb * 1024UL;
    num_slots = (unsigned)(total_bytes / sizeof(cap_entry_t));
    if (num_slots == 0) return 0; /* size_kb too small to hold even one slot */

    rd->cap_seg = dos_alloc_seg((unsigned)((num_slots * (unsigned long)sizeof(cap_entry_t) + 15UL) / 16UL));
    if (rd->cap_seg == 0) return 0; /* out of memory - capture stays off, not a fatal driver error */

    rd->cap_num_slots = num_slots;
    rd->cap_enabled = 1;
    return 1;
}

void rtl_capture_frame(rtl_dev_t *rd, unsigned char direction, const void far *data, unsigned len)
{
    cap_entry_t far *slot;
    unsigned copy_len;

    if (!rd->cap_enabled) return;

    /* NOTE: the offset multiplication is forced into 32-bit arithmetic
     * on purpose. cap_write_idx and sizeof(cap_entry_t) are both 16-bit
     * in this small-model build, so a plain "idx * sizeof(...)" is a
     * 16-bit-by-16-bit multiplication that TRUNCATES at 65536 - for a
     * large capture buffer (close to CAP_MAX_KB), idx*slot_size can
     * legitimately approach that boundary (e.g. 484*135=65340) and
     * silently wrap to a small, wrong offset, corrupting/misreading
     * entries near the end of the ring. The final result is still safe
     * to truncate back to unsigned, since the buffer itself is capped
     * at 64KB (see CAP_MAX_KB) and can never need an offset >= 65536. */
    slot = (cap_entry_t far *)MK_FP(rd->cap_seg,
        (unsigned)((unsigned long)rd->cap_write_idx * (unsigned long)sizeof(cap_entry_t)));
    {
        /* Write the 4-byte tick value via _fmemcpy() rather than a
         * direct "slot->tick = ..." far-pointer store. Slots are packed
         * at exactly sizeof(cap_entry_t) (135 bytes, odd) apart, so
         * successive slots alternate between even and odd byte offsets
         * within the segment - real-mode x86 does not fault on
         * misaligned word/dword access, but a direct multi-byte store
         * through a far pointer at an odd offset is exactly the kind of
         * case where a code-generation quirk could silently corrupt the
         * value (observed in practice: captured tick values were wrong
         * in a way the rest of the entry - direction, len, the actual
         * frame bytes - was not, which points at this one multi-byte
         * field specifically rather than the addressing itself). A
         * byte-wise copy has no alignment sensitivity at all, so this
         * sidesteps the whole class of issue regardless of the exact
         * mechanism. */
        unsigned long tick = get_bios_ticks();
        _fmemcpy(&slot->tick, &tick, sizeof(tick));
    }
    slot->direction = direction;
    slot->len = len;
    copy_len = (len < CAP_SNAPLEN) ? len : CAP_SNAPLEN;
    _fmemcpy(slot->data, data, copy_len);

    rd->cap_write_idx = (rd->cap_write_idx + 1) % rd->cap_num_slots;
    if (rd->cap_count < rd->cap_num_slots) rd->cap_count++;
}

/* ---- PHY (MDIO) access via the PHYAR register ---------------------------- */

/* ---- 32-bit port I/O (outpd/inpd) via inline assembly ---------------------
 *
 * Open Watcom's runtime library for the 16-bit real-mode DOS target turned
 * out not to provide a linkable outpd32()/inpd32() (link error "undefined
 * symbol outpd_/inpd_"), even with -3 (386 instructions) enabled. Rather
 * than chase down a library/configuration fix: just write it ourselves,
 * the same way as the 32-bit PCI config read in pci.c.
 *
 * NOTE the same pitfall as in that earlier pci.c fix: 'port' is 16-bit
 * (unsigned), so it goes into DX (16-bit), NOT into EDX - an I/O port in
 * x86 real mode is always at most 16-bit wide, regardless of the width of
 * the data being read/written. Only the DATA (EAX) is 32-bit here.
 */
static void outpd32(unsigned port, unsigned long value)
{
    _asm {
        push eax
        push edx
        mov  dx, port
        mov  eax, dword ptr value
        out  dx, eax
        pop  edx
        pop  eax
    }
}

static unsigned long inpd32(unsigned port)
{
    unsigned long result;
    _asm {
        push eax
        push edx
        mov  dx, port
        in   eax, dx
        mov  dword ptr result, eax
        pop  edx
        pop  eax
    }
    return result;
}

/* Timing parameters (25us per poll, 20 attempts = max 500us, plus a 20us
 * trailing delay) are taken exactly from r8169_mdio_write/r8169_mdio_read
 * in drivers/net/ethernet/realtek/r8169_main.c - calibrated there via a
 * real udelay(), here via udelay_dos() (see above). */
static void phy_write(unsigned io_base, unsigned reg, unsigned value)
{
    unsigned i;
    outpd32(io_base + RTL_PHYAR, 0x80000000UL | ((unsigned long)(reg & 0x1F) << 16) | (value & 0xFFFF));
    for (i = 0; i < 20; i++) {
        udelay_dos(25);
        if (!(inpd32(io_base + RTL_PHYAR) & PHYAR_FLAG)) break;
    }
    udelay_dos(20);
}

static unsigned phy_read(unsigned io_base, unsigned reg)
{
    unsigned i;
    unsigned val = 0;
    outpd32(io_base + RTL_PHYAR, (unsigned long)(reg & 0x1F) << 16);
    for (i = 0; i < 20; i++) {
        udelay_dos(25);
        if (inpd32(io_base + RTL_PHYAR) & PHYAR_FLAG) {
            val = (unsigned)(inpd32(io_base + RTL_PHYAR) & 0xFFFF);
            break;
        }
    }
    udelay_dos(20);
    return val;
}

/* ---- init ---------------------------------------------------------------- */

/* Reset timeout: 100us per poll, 100 attempts (max 10ms) - taken from
 * Linux's rtl_hw_reset()/rtl_chipcmd_cond in r8169_main.c. Returns 1 on
 * success, 0 if the card does not respond within the timeout (broken
 * card, wrong I/O address, or something not having the bus-master bit
 * set correctly yet). */
static int rtl_reset(unsigned io_base)
{
    unsigned tries = 100;
    outp(io_base + RTL_CHIPCMD, CMD_RESET);
    while (inp(io_base + RTL_CHIPCMD) & CMD_RESET) {
        udelay_dos(100);
        if (--tries == 0) return 0; /* timeout - card is not responding */
    }
    return 1;
}

static int rtl_setup_descriptors(rtl_dev_t *rd)
{
    unsigned i;

    /* TX/RX ring: 256-byte alignment is required by the datasheet/r8169. */
    rd->tx_ring = (rtl_desc_t far *)dos_alloc_aligned(
        (unsigned long)sizeof(rtl_desc_t) * rd->num_tx_desc, 256UL, &rd->tx_ring_seg);
    rd->rx_ring = (rtl_desc_t far *)dos_alloc_aligned(
        (unsigned long)sizeof(rtl_desc_t) * rd->num_rx_desc, 256UL, &rd->rx_ring_seg);
    if (rd->tx_ring == NULL || rd->rx_ring == NULL) {
        return 0; /* out of memory */
    }

    for (i = 0; i < rd->num_tx_desc; i++) {
        rd->tx_buf[i] = dos_alloc_buf(RTL_BUF_SIZE);
        if (rd->tx_buf[i] == NULL) return 0;
        rd->tx_ring[i].opts1   = 0; /* owned by the driver, nothing to send yet */
        rd->tx_ring[i].opts2   = 0;
        rd->tx_ring[i].addr_lo = phys_addr(rd->tx_buf[i]);
        rd->tx_ring[i].addr_hi = 0;
    }
    rd->tx_ring[rd->num_tx_desc - 1].opts1 |= RTL_DESC_EOR;

    for (i = 0; i < rd->num_rx_desc; i++) {
        rd->rx_buf[i] = dos_alloc_buf(RTL_BUF_SIZE);
        if (rd->rx_buf[i] == NULL) return 0;
        rd->rx_ring[i].opts2   = 0;
        rd->rx_ring[i].addr_lo = phys_addr(rd->rx_buf[i]);
        rd->rx_ring[i].addr_hi = 0;
        /* OWN=1 hands the buffer to the NIC right away to fill in */
        rd->rx_ring[i].opts1   = RTL_DESC_OWN | (RTL_BUF_SIZE & RTL_DESC_LEN_MASK);
    }
    rd->rx_ring[rd->num_rx_desc - 1].opts1 |= RTL_DESC_EOR;

    rd->tx_cur = 0;
    rd->rx_cur = 0;
    return 1;
}

/* Resets the descriptor rings to their initial state (all RX descriptors
 * handed back to the NIC, all TX descriptors empty) WITHOUT reallocating
 * memory. Used by both the initial init and the lightweight
 * reset_interface() path below. */
static void rtl_reset_ring_state(rtl_dev_t *rd)
{
    unsigned i;
    for (i = 0; i < rd->num_tx_desc; i++) {
        rd->tx_ring[i].opts1 = 0;
    }
    rd->tx_ring[rd->num_tx_desc - 1].opts1 |= RTL_DESC_EOR;

    for (i = 0; i < rd->num_rx_desc; i++) {
        rd->rx_ring[i].opts1 = RTL_DESC_OWN | (RTL_BUF_SIZE & RTL_DESC_LEN_MASK);
    }
    rd->rx_ring[rd->num_rx_desc - 1].opts1 |= RTL_DESC_EOR;

    rd->tx_cur = 0;
    rd->rx_cur = 0;
}

/* Programs the chip registers (descriptor addresses, Rx/TxConfig, PHY
 * speed, interrupt mask) and enables RX/TX. Requires rd->tx_ring/rx_ring
 * to already be allocated and rd->io_base to be valid - performs NO
 * memory allocation itself, so it can safely be reused by both the
 * initial init and reset_interface(). */
static void rtl_hw_start(rtl_dev_t *rd)
{
    unsigned io_base = rd->io_base;
    unsigned long tx_ring_phys = phys_addr(rd->tx_ring);
    unsigned long rx_ring_phys = phys_addr(rd->rx_ring);

    outp(io_base + RTL_CFG9346, CFG9346_UNLOCK);

    outpd32(io_base + RTL_TXDESC_LOW,  tx_ring_phys);
    outpd32(io_base + RTL_TXDESC_HIGH, 0);
    outpd32(io_base + RTL_RXDESC_LOW,  rx_ring_phys);
    outpd32(io_base + RTL_RXDESC_HIGH, 0);

    /* RxConfig: accept broadcast + our own MAC (unicast) + multicast
     * (bits 1-3, from r8169_main.c AcceptBroadcast|AcceptMulticast|
     * AcceptMyPhys: bit3=AB, bit2=AM, bit1=APM), plus an explicit RX DMA
     * burst size of 1024 bytes (MXDMA field, bits 8-10, value 6) -
     * matching the well-tested value used by both the Linux r8169 driver
     * and U-Boot's rtl8169 driver. The earlier "0x00E00000" value here
     * did NOT target this field (verified against the actual RTL8169
     * datasheet, Realtek doc rev 1.21, which places MXDMA at bits 8-10)
     * and left the burst size at its power-on default of 16 bytes -
     * corrected here for consistency with the TX fix below, even though
     * RX itself was not observed to be affected by this. */
    outpd32(io_base + RTL_RXCONFIG, 0x0000000EUL | (0x6UL << 8));

    /* TxConfig: InterFrameGap=3 (shortest, unchanged) PLUS an explicit TX
     * DMA burst size of 1024 bytes (MXDMA field, bits 8-10, value 6).
     *
     * THIS WAS THE ROOT CAUSE of "large TX frames fail, small ones do
     * not" (confirmed via a packet capture showing DOS-transmitted TCP
     * segments at MTU 1500 never arriving at the receiver, while the
     * exact same driver/hardware worked perfectly at MTU 576, and while
     * RECEIVING equally large frames from the peer was never a problem).
     * The previous TxConfig write (0x03000000) only set the
     * InterFrameGap bits and left the burst-size field at its power-on
     * default of 16 bytes (field value 0) - meaning a ~1514-byte frame
     * needed roughly 95 separate 16-byte DMA bursts (each needing its
     * own PCI bus arbitration) to fully load into the chip's TX FIFO.
     * Combined with the RTL8169's "early transmit" feature (see
     * RTL_EARLYTXTHRES below), which can start putting bits on the wire
     * before the whole frame has finished loading, a large frame with
     * such a tiny DMA burst is at real risk of a FIFO underrun (the DMA
     * engine falling behind the transmit rate mid-frame) - a small
     * (~590-byte) frame either finishes loading before transmission
     * starts or needs far fewer bursts either way, which is exactly why
     * MTU 576 worked while MTU 1500 did not. */
    outpd32(io_base + RTL_TXCONFIG, (0x03UL << 24) | (0x6UL << 8));

    /* Belt and braces: also constrain "early transmit" via EarlyTxThres.
     *
     * EXPERIMENTAL: using RTL_EARLYTX_MODERATE (1024-byte threshold,
     * matching the DMA burst fix above) instead of RTL_NOEARLYTX here,
     * to recover some of the latency NoEarlyTx costs (confirmed via
     * real-world testing: uploads were correct but noticeably slow with
     * early transmit fully disabled). If large-frame transfers become
     * unreliable again, revert this one line to RTL_NOEARLYTX - that
     * value is confirmed safe, this one is a reasoned but not yet
     * separately confirmed step back towards better throughput now that
     * the DMA burst size itself is no longer the bottleneck it was. */
    outp(io_base + RTL_EARLYTXTHRES, RTL_EARLYTX_MODERATE);

    /* Max RX frame size: standard Ethernet plus margin, NO jumbo frames. */
    outpw(io_base + RTL_RXMAXSIZE, 1536);

    /* C+ command register: multiple read/write on, checksum offload OFF
     * (keeps things simple: the driver does not compute checksums itself,
     * so it does not ask the card to "help" with that either). */
    outpw(io_base + RTL_CPLUSCMD, CPCMD_MULRW);

    /* Interrupt coalescing/moderation (see "-m" in main.c and the long
     * comment at RTL_DEFAULT_INTR_MITIGATE in rtl8169p.h for the full
     * reasoning and value history). Written right after CPlusCmd since
     * that register's bits [1:0] determine the time unit the USECS
     * fields in this one are scaled by. Defaults to 0x5151, confirmed
     * stable via repeated real-world testing, and paired with the
     * burst-gap default (RTL_DEFAULT_BURST_GAP_US, below) being 0 - the
     * two work together, see that comment before changing either one on
     * its own. */
    outpw(io_base + RTL_INTRMITIGATE, rd->intr_mitigate);

    /* PHY: auto-negotiation (including gigabit). Confirmed via real-
     * world testing (see README - "Known environmental quirk: gigabit
     * negotiation and first-DHCP-attempt failures") that forcing 100
     * Mbit avoids a specific first-DHCP-attempt failure seen with one
     * particular router, but that failure turned out to be a property
     * of that router's handling of a freshly completed GIGABIT
     * negotiation specifically - not a driver bug, and not reproduced
     * on 20+ other NIC/driver combinations on the same network, nor on
     * this same card/driver when physically connected through a
     * 100 Mbit-only switch instead. Auto-negotiation (full capability)
     * is kept as the default; force BMCR_FORCE_100_FULL instead only if
     * you hit that specific symptom on your own network and cannot
     * tolerate the extra delay (see the README workarounds). */
    phy_write(io_base, MII_PAGE_SELECT, 0x0000); /* force the default page - see MII_PAGE_SELECT in rtl8169p.h */
    phy_write(io_base, MII_ANAR, ANAR_10_100_ALL);
    phy_write(io_base, MII_GBCR, GBCR_ADV_1000_FULL); /* gigabit advertisement (separate register, see rtl8169p.h) */
    phy_write(io_base, MII_BMCR, BMCR_AUTONEG_ENABLE | BMCR_AUTONEG_RESTART);

    /* Interrupts: only RX-ok, RX-error, TX-ok, TX-error, ring overflow.
     * No timer/coalescing. */
    outpw(io_base + RTL_INTRMASK, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RXOVW);

    /* Enable RX + TX, then lock the config registers again. */
    outp(io_base + RTL_CHIPCMD, CMD_RXENB | CMD_TXENB);
    outp(io_base + RTL_CFG9346, CFG9346_LOCK);
}

int rtl_probe_and_init(rtl_dev_t *rd, unsigned num_tx_desc, unsigned num_rx_desc, unsigned burst_gap_us, unsigned intr_mitigate)
{
    pci_device_t pdev;
    unsigned io_base;
    unsigned i;

    memset(rd, 0, sizeof(*rd));

    /* 0 (or an out-of-range value) means "use the default"; otherwise
     * validate against the fixed array-sizing maximum. main.c's -t/-r
     * argument parsing already range-checks against these same limits
     * before getting here, so this is a defensive second check, not the
     * primary validation point. */
    rd->num_tx_desc = (num_tx_desc >= 1 && num_tx_desc <= RTL_MAX_TX_DESC)
                       ? num_tx_desc : RTL_DEFAULT_TX_DESC;
    rd->num_rx_desc = (num_rx_desc >= 1 && num_rx_desc <= RTL_MAX_RX_DESC)
                       ? num_rx_desc : RTL_DEFAULT_RX_DESC;

    /* burst_gap_us uses a SEPARATE sentinel (RTL_BURST_GAP_UNSET), not
     * 0, since 0 is itself a valid, deliberate value here (see the long
     * comment at RTL_DEFAULT_BURST_GAP_US in rtl8169p.h) - "not given on
     * the command line" and "explicitly set to 0" must stay
     * distinguishable. */
    rd->burst_gap_us = (burst_gap_us <= RTL_MAX_BURST_GAP_US)
                        ? burst_gap_us : RTL_DEFAULT_BURST_GAP_US;

    /* intr_mitigate needs no UNSET-style sentinel the way burst_gap_us
     * does: main.c initialises its own intr_mitigate_arg directly to
     * RTL_DEFAULT_INTR_MITIGATE (0x5151) and only changes it if "-m" was
     * actually given, so by the time it gets here, "not given" and
     * "given" are already resolved to a single, correct 16-bit value -
     * this function just uses it as-is. Any value 0x0000-0xFFFF is a
     * structurally valid register value (see the field layout comment
     * at RTL_INTRMITIGATE in rtl8169p.h), so no range validation is
     * needed here either. */
    rd->intr_mitigate = intr_mitigate;

    if (!pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8169, &pdev)) {
        return 0; /* no RTL8169 found */
    }
    if (pdev.io_base == 0) {
        return 0; /* no usable I/O BAR - rare, but we stop here in that case */
    }

    pci_enable_busmaster(pdev.bus, pdev.dev_func);

    io_base = (unsigned)(pdev.io_base & 0xFFFF); /* I/O ports are 16-bit in DOS */
    rd->io_base  = io_base;
    rd->irq_line = pdev.irq_line;

    /* MAC address: on most RTL8169 cards the BIOS/firmware has already
     * loaded this into MAC0-5, so we simply read it back. EEPROM
     * bit-banging is deliberately left out (this matches the classic
     * RTL8169's own driver history - see README). */
    for (i = 0; i < 6; i++) {
        rd->mac[i] = inp(io_base + RTL_MAC0 + i);
    }

    if (!rtl_reset(io_base)) {
        return 0; /* card did not respond within 10ms - broken or wrong address */
    }

    if (!rtl_setup_descriptors(rd)) {
        return 0; /* out of memory */
    }

    rtl_hw_start(rd);
    /* Deliberately NOT waiting for link here - see the long comment at
     * the call site in main.c for why: the caller must unmask the
     * hardware IRQ (pktdrv_install()) BEFORE this potentially multi-
     * second wait, not after, to avoid a window where RX is already
     * active but the interrupt is still masked at the PIC. */
    return 1;
}

/* Lightweight re-init for reset_interface() (packet driver function 7):
 * NO PCI re-detection, NO new memory allocation - reuses the already
 * allocated descriptor rings/buffers. Needed because reset_interface()
 * can be called by an application while the driver has long been
 * running; calling rtl_probe_and_init() again would allocate a fresh set
 * of buffers on every reset without freeing the old ones - a memory
 * leak. Returns 0 if the chip does not respond to the reset. */
int rtl_reinit(rtl_dev_t *rd)
{
    unsigned i;

    if (rd->io_base == 0 || rd->tx_ring == NULL || rd->rx_ring == NULL) {
        return 0; /* never successfully initialised */
    }
    if (!rtl_reset(rd->io_base)) {
        return 0;
    }

    rtl_reset_ring_state(rd);

    /* The spec (section 6.9) requires the local network address to be
     * reset to its ROM default - that is simply re-reading MAC0-5. */
    for (i = 0; i < 6; i++) {
        rd->mac[i] = inp(rd->io_base + RTL_MAC0 + i);
    }

    rtl_hw_start(rd);
    rtl_wait_for_link(rd, 8000);
    return 1;
}

/* Blocking wait for FULLY completed auto-negotiation, including the
 * gigabit master/slave training phase - not for the RTL8169-specific
 * PHYstatus.LinkStatus bit, which was observed to go high during an
 * intermediate state before gigabit training had actually finished
 * (visible as: the LEDs briefly go dark, then the final 1GB LED comes
 * on - the driver used to read the speed during that intermediate
 * phase). We therefore first poll the standard IEEE BMSR "Auto-
 * Negotiation Complete" bit, and only THEN (plus a small safety margin)
 * read the final PHYstatus register. */
int rtl_wait_for_link(rtl_dev_t *rd, unsigned timeout_ms)
{
    unsigned i, j;
    unsigned bmsr;
    unsigned char status;

    rd->link_up = 0;
    rd->link_speed_mbit = 0;
    rd->link_full_duplex = 0;

    for (i = 0; i < timeout_ms; i++) {
        bmsr = phy_read(rd->io_base, MII_BMSR);
        if ((bmsr & BMSR_AUTONEG_COMPLETE) && (bmsr & BMSR_LINK_STATUS)) {
            /* Auto-negotiation has now REALLY finished. Small extra
             * margin (100ms) in case PHYstatus internally updates a
             * little later than BMSR - udelay_dos() takes a 16-bit
             * parameter, so this loops instead of a single 100000 call
             * (which would overflow). */
            for (j = 0; j < 100; j++) {
                udelay_dos(1000);
            }

            status = (unsigned char)inp(rd->io_base + RTL_PHYSTATUS);
            if (status & PHYSTATUS_LINK) {
                rd->link_up = 1;
                rd->link_speed_mbit  = (status & PHYSTATUS_1000BPSF) ? 1000 :
                                        (status & PHYSTATUS_100BPS)   ? 100  :
                                        (status & PHYSTATUS_10BPS)    ? 10   : 0;
                rd->link_full_duplex = (status & PHYSTATUS_FULLDUP) ? 1 : 0;
                return 1;
            }
            /* BMSR reports done+linked, but PHYstatus disagrees - a rare
             * edge case; just keep polling until the timeout. */
        }
        udelay_dos(1000); /* ~1ms per iteration */
    }
    return 0;
}

void rtl_get_mac(rtl_dev_t *rd, unsigned char mac[6])
{
    memcpy(mac, rd->mac, 6);
}

int rtl_send(rtl_dev_t *rd, const void far *data, unsigned len)
{
    unsigned idx = rd->tx_cur;
    unsigned long opts1;

    if (len > RTL_BUF_SIZE) return -1;

    /* Give an in-flight transmit a brief, calibrated chance to complete
     * before giving up. An earlier version failed immediately here on
     * the reasoning that "there is no queue, so waiting cannot help" -
     * that reasoning only holds for a genuinely stuck ring (cable pulled,
     * hardware wedged). During ordinary bulk transfers (e.g. an FTP
     * upload keeping several frames in flight) a full ring is a normal,
     * MOMENTARY state: the NIC is actively transmitting and will clear
     * OWN within tens of microseconds at 100/1000 Mbit. Failing
     * immediately in that case handed CANT_SEND back to the application
     * for something that would have succeeded a fraction of a
     * millisecond later - observed in practice as hung FTP uploads and
     * wildly inconsistent ping times (mTCP's own, much coarser TCP
     * retransmission timers took over instead). 40 iterations of 50us
     * (2ms total budget) is generous for real drainage and still fails
     * fast for an actually stuck ring. */
    {
        unsigned tries = 40;
        while ((rd->tx_ring[idx].opts1 & RTL_DESC_OWN) && tries--) {
            udelay_dos(50);
        }
        if (rd->tx_ring[idx].opts1 & RTL_DESC_OWN) {
            return -1; /* still not free after 2ms - genuinely stuck/full */
        }
    }

    _fmemcpy(rd->tx_buf[idx], data, len);
    rtl_capture_frame(rd, 1, data, len); /* 1 = TX; capture what we were actually asked to send */

    opts1 = RTL_DESC_OWN | RTL_DESC_FS | RTL_DESC_LS | (len & RTL_DESC_LEN_MASK);
    if (idx == rd->num_tx_desc - 1) opts1 |= RTL_DESC_EOR;

    rd->tx_ring[idx].opts2 = 0;
    rd->tx_ring[idx].opts1 = opts1;

    /* Kick: set the NPQ bit (0x40) in TxPoll so the card looks at the ring. */
    outp(rd->io_base + RTL_TXPOLL, 0x40);

    rd->tx_cur = (idx + 1) % rd->num_tx_desc;
    return 0;
}

/* Tried and ruled out: an earlier version of this driver called WBINVD
 * (full CPU cache flush) here to test a CPU-cache-staleness hypothesis
 * for the same data-corruption symptom the posted-write flush above now
 * targets. Real-world testing showed WBINVD made no difference, which is
 * exactly what ruling out cache staleness (as opposed to PCI write
 * posting) would look like - see the README for the full investigation
 * and why posted-write ordering is now the leading, evidence-based
 * explanation instead. */

void rtl_poll_rx(rtl_dev_t *rd, rtl_rx_callback_t cb)
{
    unsigned idx = rd->rx_cur;
    unsigned guard = rd->num_rx_desc; /* avoid an infinite loop on odd behaviour */
    unsigned delivered_this_poll = 0; /* see the EXPERIMENTAL comment below */

    while (guard--) {
        unsigned long opts1 = rd->rx_ring[idx].opts1;

        if (opts1 & RTL_DESC_OWN) break; /* still owned by the NIC: nothing more for now */

        {
            unsigned len = (unsigned)(opts1 & RTL_DESC_LEN_MASK);
            /* Bad frames (CRC error, runt, watchdog timeout) are NEVER
             * handed to the application - only return the buffer to the
             * NIC. Confirmed against r8169_main.c
             * (RxRWT/RxRES/RxRUNT/RxCRC). */
            if (opts1 & RTL_RX_ERROR_MASK) {
                rd->stat_rx_errors++;
            }
            /* The RTL8169 appends a 4-byte CRC to every received frame;
             * strip it before handing the frame to the layer above. */
            else if (len > 4 && cb != NULL) {
                /* CONFIRMED FIX (previously an experiment - see the
                 * README for the full investigation): a small gap
                 * between BURSTY, back-to-back RX frames within a single
                 * poll fixes a rare data-corruption pattern found during
                 * testing (byte-level comparison of repeated
                 * retransmissions of the same TCP segment showed a
                 * handful of scattered payload bytes differing between
                 * receptions, despite the NIC's own CRC check passing
                 * each time). Two more specific hypotheses - CPU cache
                 * staleness (WBINVD) and PCI posted-write ordering (the
                 * IntrStatus read-back just below) - were each tested
                 * and ruled out first; this fix targets neither
                 * mechanism directly, only the observation that the
                 * corruption needed back-to-back frames to trigger (the
                 * kind of tight clustering TCP slow-start produces early
                 * in a connection - though this driver has no concept of
                 * TCP connections at all, it only ever sees raw Ethernet
                 * frames, so burst timing is the closest thing to that
                 * idea it can actually detect and act on). Confirmed via
                 * ~20 repeated real-world transfers of the specific file
                 * that had reliably reproduced the corruption before
                 * this fix, across varied conditions (alongside larger
                 * and smaller files, alone, with reboots in between) -
                 * zero recurrences at 300us specifically. The gap is
                 * skipped for a burst's first frame, and for ordinary
                 * non-bursty single-frame arrivals, so normal traffic is
                 * unaffected. RTL_DEFAULT_BURST_GAP_US was later lowered
                 * to 100us, tested and confirmed stable - but 10us was
                 * directly confirmed NOT enough, the corruption came
                 * back at that value, so going lower than 100us is the
                 * untested, risky direction. See the long comment at
                 * RTL_DEFAULT_BURST_GAP_US in rtl8169p.h for the full,
                 * up to date value history and how much confidence each
                 * figure actually has behind it. Configurable via "-g"
                 * (main.c) for further retesting, or to disable this fix
                 * entirely ("-g 0") for comparison. */
                if (rd->burst_gap_us > 0 && delivered_this_poll > 0) {
                    udelay_dos(rd->burst_gap_us);
                }

                /* Force completion of any POSTED PCI writes from this
                 * device before trusting the data - a different class of
                 * hazard from CPU cache staleness (already ruled out via
                 * WBINVD testing, see the README entry this replaces).
                 * The packet-data DMA write and the descriptor's own
                 * OWN-bit write are two SEPARATE PCI write transactions;
                 * PCI write posting means a bridge/chipset can buffer a
                 * write and report it "done" to the CPU before it has
                 * actually reached memory, with no guarantee the data
                 * write and the status write complete in the order they
                 * were issued unless something forces it. PCI ordering
                 * rules guarantee that a READ COMPLETION from a device
                 * cannot overtake that SAME device's own earlier posted
                 * writes - so reading any register back here (IntrStatus
                 * is convenient, already mapped) flushes the real packet
                 * DMA write before we read the buffer, at the cost of one
                 * extra I/O port read per received frame (far cheaper
                 * than WBINVD's full cache flush). */
                (void)inpw(rd->io_base + RTL_INTRSTATUS);
                rtl_capture_frame(rd, 0, rd->rx_buf[idx], len - 4); /* 0 = RX */
                cb(rd->rx_buf[idx], len - 4);
                delivered_this_poll++;
            }
        }

        /* Return the buffer to the NIC for the next frame. */
        rd->rx_ring[idx].opts1 = RTL_DESC_OWN | (RTL_BUF_SIZE & RTL_DESC_LEN_MASK)
                                  | ((idx == rd->num_rx_desc - 1) ? RTL_DESC_EOR : 0);

        idx = (idx + 1) % rd->num_rx_desc;
    }

    rd->rx_cur = idx;
}

void rtl_ack_and_get_status(rtl_dev_t *rd, unsigned far *status_out)
{
    unsigned status = inpw(rd->io_base + RTL_INTRSTATUS);
    outpw(rd->io_base + RTL_INTRSTATUS, status); /* write-1-to-clear */
    if (status_out) *status_out = status;
}

/* Disables RX/TX and masks the card's interrupts - MUST be called on
 * unload, BEFORE the descriptor rings/buffers are handed back to DOS.
 * Without this the card stays active with descriptor registers pointing
 * at memory that is about to be reused for something else entirely - if
 * a packet then arrives (e.g. a broadcast), the card could DMA into that
 * memory, corrupting it. This is plain hardware I/O (outp/inp), NOT a
 * DOS call, so - unlike the earlier failed attempts to free memory from
 * inside the interrupt handler - it is perfectly safe to call this
 * directly from pktdrv_uninstall(). */
void rtl_stop(rtl_dev_t *rd)
{
    if (rd->io_base == 0) return;
    outpw(rd->io_base + RTL_INTRMASK, 0);      /* no more interrupts */
    outp(rd->io_base + RTL_CHIPCMD, 0);        /* RX/TX off */
}

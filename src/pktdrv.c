/*
 * pktdrv.c - INT 60h dispatcher and hardware IRQ ISR.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of the RTL8169P DOS Packet Driver project.
 * Copyright (C) 2026 Yoghoo
 * See pktdrv.h for the full license notice.
 *
 * IMPORTANT WARNING (read this before relying on it):
 * The exact input/output registers for each function number below were
 * reconstructed from the FTP Software / Crynwr Packet Driver
 * Specification (Russ Nelson, ~1990) and verified against the original
 * spec text where noted. This is the most critical and most error-prone
 * part of the whole driver: one wrong register and a TCP/IP stack such
 * as mTCP will fail, often without a clear error message.
 */

#include <dos.h>
#include <conio.h>
#include <string.h>
#include "pktdrv.h"

/* ---- driver identification (function 1: driver_info) --------------------- */
static char g_driver_name[] = "RTL8169P";  /* DS:SI points here for function 1 */
#define PD_VERSION      0x0101   /* "1.01" - arbitrary version number for THIS driver */
#define PD_IFACE_CLASS  PD_CLASS_ETHERNET
#define PD_IFACE_TYPE   0xFFFF  /* "generic"/unknown NIC type ID; the spec allows this */
#define PD_IFACE_NUMBER 0

/* ---- global driver state (1 card, 1 registered handle) -------------------- */
static rtl_dev_t     *g_rd = NULL;
static unsigned char  g_int_no = 0x60;
static unsigned char  g_irq_line = 0;
static unsigned       g_my_psp = 0; /* our own PSP segment, captured at install() - see PKTDRV_UNLOAD_AH */
static rtl_free_list_t g_unload_free_list; /* filled in by PKTDRV_UNLOAD_AH, read by the caller via ES:DI */
static cap_dump_info_t g_cap_dump_info;    /* filled in by PKTDRV_CAPDUMP_AH, read by the caller via ES:DI */

/* ---- packet-driver-level statistics (get_statistics(), function 24) ------
 * "in" counters are updated in deliver_packet() (the RX upcall path);
 * "out" counters are updated in the PD_FUNC_SEND_PKT dispatcher case.
 * errors_in comes from the hardware layer (rtl_dev_t.stat_rx_errors, see
 * rtl8169p.c) and is read directly at get_statistics() time, not tracked
 * here. g_stats_buf is where the spec-shaped struct is assembled on
 * demand, since get_statistics() must return a far pointer to it (DS:SI)
 * that stays valid after the call returns. */
static unsigned long g_stat_packets_in   = 0;
static unsigned long g_stat_packets_out  = 0;
static unsigned long g_stat_bytes_in     = 0;
static unsigned long g_stat_bytes_out    = 0;
static unsigned long g_stat_errors_out   = 0;
static unsigned long g_stat_packets_lost = 0;
static pd_statistics_t g_stats_buf;

/* One explicit typedef for the interrupt-vector type, instead of loose
 * `void (__interrupt __far *)()` declarations scattered around.
 *
 * REASON: Open Watcom's _dos_setvect() literally expects the type
 * "void __interrupt (__far *)( void )". A function that takes union
 * INTPACK as a parameter (like pktdrv_isr) has a DIFFERENT, incompatible
 * type - passing it without a cast produces a hard compiler error, E473
 * "function argument(s) do not match those in prototype". An explicit
 * cast works around that - but it must then be applied consistently
 * everywhere, including _dos_getvect(), or the same kind of mismatch
 * shows up again on assignment. */
typedef void (__interrupt __far *pktdrv_intvect_t)(void);

static pktdrv_intvect_t g_old_pktint = NULL;
static pktdrv_intvect_t g_old_hwint  = NULL;

static int            g_handle_active = 0;
static int            g_handle_id     = 1;   /* the only handle value we ever hand out */
static unsigned char  g_filter_type[2];
static unsigned       g_filter_len    = 0;    /* 0 = "receive all" (per spec, type_len 0 means everything) */
static void (__interrupt __far *g_receiver)() = NULL; /* the application's upcall routine */

/* ---- PIC/IRQ helpers ------------------------------------------------------ */

static unsigned char irq_to_int_vector(unsigned char irq)
{
    return (irq < 8) ? (0x08 + irq) : (0x70 + (irq - 8));
}

#define PIC_CASCADE_IRQ 2   /* IRQ2 on the master PIC = cascade to the slave PIC */

static void pic_unmask(unsigned char irq)
{
    unsigned port = (irq < 8) ? 0x21 : 0xA1;
    unsigned char bit = 1 << (irq < 8 ? irq : (irq - 8));
    unsigned char mask = inp(port);
    outp(port, mask & ~bit);

    /* IRQ 8-15 run through the cascade line (IRQ2) on the master PIC. If
     * that line is masked there, the interrupt never reaches the CPU,
     * regardless of whether it is unmasked on the slave PIC - a common,
     * easy-to-miss cause of "the driver initialises fine, but no RX
     * interrupt ever arrives". */
    if (irq >= 8) {
        unsigned char master_mask = inp(0x21);
        outp(0x21, master_mask & ~(1 << PIC_CASCADE_IRQ));
    }
}

/* No pic_mask() here: with proper IRQ chaining now in place (see
 * hw_isr()), masking the line on uninstall would be actively harmful if
 * another device shares this IRQ - it would silently stop THAT device's
 * interrupts too. The PIC mask bit is a property of the shared line, not
 * of any one driver on it, so we simply leave it alone when we leave. */

static void pic_send_eoi(unsigned char irq)
{
    if (irq >= 8) outp(0xA0, 0x20);
    outp(0x20, 0x20);
}

/* ---- RX upcall to the registered application ------------------------------
 *
 * REGISTER CONVENTION VERIFIED against the original Packet Driver
 * Specification (crynwr.com/packet_driver.html, section 6.4 "receiver
 * call"):
 *
 *   (*receiver)(handle, flag, len [, buffer])
 *       handle   BX   (ALWAYS, on both calls)
 *       flag     AX   (0 = first call, 1 = second call)
 *       len      CX   (on both calls)
 *       buffer   DS:SI (ONLY on the second call, when AX==1)
 *
 * 1) First call (AX=0): "I have a packet of this length for you, give me
 *    a buffer." The application returns ES:DI = buffer, or 0000:0000 to
 *    drop the packet.
 * 2) The driver copies the data into that buffer.
 * 3) Second call (AX=1): "the copy is complete", DS:SI points at the
 *    (now filled) buffer.
 *
 * This MUST happen from the hardware ISR (that is exactly how packet
 * drivers work), so this is the most time-critical/fragile code in the
 * project. Written with inline assembly because the exact register
 * layout cannot be expressed through a normal C function call.
 */
static void deliver_packet(unsigned char far *data, unsigned len)
{
    unsigned buf_seg, buf_off;
    void (__interrupt __far *recv)() = g_receiver;
    unsigned handle = (unsigned)g_handle_id;

    if (!g_handle_active || recv == NULL || len == 0) return;

    /* --- step 1: "give me a buffer" (flag/AX = 0) --- */
    _asm {
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push es
        push ds

        mov  ax, 0              ; flag = 0 (first call)
        mov  bx, handle
        mov  cx, len
        call dword ptr recv
        mov  buf_seg, es
        mov  buf_off, di

        pop  ds
        pop  es
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
    }

    if (buf_seg == 0 && buf_off == 0) {
        g_stat_packets_lost++; /* "no buffer from receiver()" - exactly what the spec's packets_lost field covers */
        return; /* the application does not want this packet (buffer full, etc.) */
    }

    _fmemcpy(MK_FP(buf_seg, buf_off), data, len);
    g_stat_packets_in++;
    g_stat_bytes_in += len;

    /* --- step 2: "packet is ready" (flag/AX = 1) --- */
    _asm {
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push es
        push ds

        mov  ax, 1              ; flag = 1 (second call)
        mov  bx, handle
        mov  cx, len
        mov  dx, buf_seg
        push dx
        pop  ds
        mov  si, buf_off
        call dword ptr recv

        pop  ds
        pop  es
        pop  di
        pop  si
        pop  dx
        pop  cx
        pop  bx
        pop  ax
    }
}

/* ---- the NIC's hardware IRQ ISR -------------------------------------------- */

static void __interrupt __far hw_isr(void)
{
    unsigned status = 0;

    /* Only claim this interrupt if OUR OWN status register actually has
     * something set. A PCI IRQ line is commonly shared between several
     * devices (PCI only has 4 physical interrupt pins for the whole
     * bus), so an interrupt on this line does not necessarily mean the
     * RTL8169 caused it. */
    if (g_rd != NULL) {
        rtl_ack_and_get_status(g_rd, &status);
    }

    if (status != 0) {
        /* It really was us: service it and end the chain here with our
         * own EOI. If another device really is sharing this line and
         * also has a pending interrupt, its own condition is still
         * asserted after our EOI, so the CPU takes the interrupt again
         * immediately and it is picked up by whichever handler now sits
         * "underneath" us (see the chaining branch below) - so nothing
         * is missed even though we do not explicitly chain in this case. */
        if (status & (INT_ROK | INT_RER)) {
            rtl_poll_rx(g_rd, deliver_packet);
        }
        if (status & INT_RXOVW) {
            /* RX ring overflow: this is exactly the "card out of
             * resources" case the spec's packets_lost field describes. */
            g_stat_packets_lost++;
        }
        /* TOK/TER: nothing further is done with these here beyond
         * acknowledging the status (already done above). */
        pic_send_eoi(g_irq_line);
        return;
    }

    /* Not our interrupt (or the driver has not finished initialising
     * yet) - hand off to whatever was hooked on this vector before we
     * installed ourselves, exactly as if we were never here. This is
     * the standard, polite way to share a hardware IRQ line in DOS.
     * _chain_intr() reconstructs the stack so the target's own IRET
     * returns to the ORIGINAL caller of the interrupt, and it never
     * returns to us - the cast matches the same "() vs (void) parameter
     * list" pitfall already documented for _dos_setvect() above. */
    if (g_old_hwint) {
        _chain_intr((void (__interrupt __far *)())g_old_hwint);
    }
    /* No previous handler to chain to (should not normally happen - DOS
     * always has some default handler on every vector) - still send EOI
     * ourselves so the PIC is not left waiting forever. */
    pic_send_eoi(g_irq_line);
}

/* ---- INT 60h dispatcher ----------------------------------------------------
 *
 * Watcom's __interrupt functions with an INTPACK parameter give direct
 * read/write access to the registers as they were on the stack at entry -
 * including the flags, which lets us set/clear the carry flag for the
 * spec-conformant error signal.
 */
static void __interrupt __far pktdrv_isr(union INTPACK r)
{
    switch (r.h.ah) {

    case PD_FUNC_DRIVER_INFO: {
        /* The spec (section 6.3) requires AH==1 AND AL==255 on entry; we
         * are deliberately lenient here (no AL check) since this is the
         * only function on AH=1, so a mismatched AL cannot cause any
         * ambiguity. AL on RETURN is the functionality level, not an
         * "extended on/off" flag: 1=basic functions only, 2=basic+
         * extended, 5=basic+high-performance, 6=all three. This driver
         * implements the basic functions (1 through 7) plus one extended
         * function (get_statistics, 24), so AL=2 is correct here. */
        r.w.bx = PD_VERSION;
        r.h.ch = PD_IFACE_CLASS;
        r.w.dx = PD_IFACE_TYPE;
        r.h.cl = PD_IFACE_NUMBER;
        r.w.ds = FP_SEG(g_driver_name);
        r.w.si = FP_OFF(g_driver_name);
        r.h.al = 2; /* functionality: basic + extended (get_statistics) */
        r.w.flags &= ~1; /* CF=0: success */
        break;
    }

    case PD_FUNC_ACCESS_TYPE: {
        /* Simple, single-handle variant: we only accept one registration
         * at a time, and otherwise ignore if_class/if_type/if_number
         * matching (there is only one card/interface anyway). type_len==0
         * means all ethertypes pass through (sufficient for an IP-only
         * stack). Register roles (type pointer = DS:SI, receiver address
         * = ES:DI) verified against the original spec, section 6.4. */
        if (g_handle_active) {
            r.h.dh = PD_ERR_TYPE_INUSE;
            r.w.flags |= 1;
            break;
        }
        g_filter_len = r.w.cx;
        if (g_filter_len > 2) g_filter_len = 2; /* we only support 0- or 2-byte ethertype filters */
        if (g_filter_len == 2) {
            unsigned char far *tp = MK_FP(r.w.ds, r.w.si); /* type: DS:SI */
            g_filter_type[0] = tp[0];
            g_filter_type[1] = tp[1];
        }
        g_receiver = (void (__interrupt __far *)())MK_FP(r.w.es, r.w.di); /* receiver routine: ES:DI */
        g_handle_active = 1;
        r.w.ax = g_handle_id;
        r.w.flags &= ~1;
        break;
    }

    case PD_FUNC_RELEASE_TYPE: {
        if (!g_handle_active || r.w.bx != (unsigned)g_handle_id) {
            r.h.dh = PD_ERR_BAD_HANDLE;
            r.w.flags |= 1;
            break;
        }
        g_handle_active = 0;
        g_receiver = NULL;
        r.w.flags &= ~1;
        break;
    }

    case PD_FUNC_SEND_PKT: {
        /* Input per the spec (section 6.6): DS:SI = pointer to the frame,
         * CX = length. */
        void far *pkt = MK_FP(r.w.ds, r.w.si);
        if (g_rd == NULL || rtl_send(g_rd, pkt, r.w.cx) != 0) {
            g_stat_errors_out++;
            r.h.dh = PD_ERR_CANT_SEND;
            r.w.flags |= 1;
            break;
        }
        g_stat_packets_out++;
        g_stat_bytes_out += r.w.cx;
        r.w.flags &= ~1;
        break;
    }

    case PD_FUNC_GET_ADDRESS: {
        /* Input: ES:DI = buffer, CX = buffer size (must be >=6).
         * Output: CX = actual length (6), data copied to ES:DI. */
        if (r.w.cx < 6 || g_rd == NULL) {
            r.h.dh = PD_ERR_NO_SPACE;
            r.w.flags |= 1;
            break;
        }
        _fmemcpy(MK_FP(r.w.es, r.w.di), g_rd->mac, 6);
        r.w.cx = 6;
        r.w.flags &= ~1;
        break;
    }

    case PD_FUNC_RESET_IF: {
        /* Spec (section 6.9): handle in BX, possible errors BAD_HANDLE
         * and CANT_RESET. We use the lightweight rtl_reinit() (no
         * realloc/PCI re-detection, see rtl8169p.c) to avoid the memory
         * leak that a fresh rtl_probe_and_init() call would cause. */
        if (g_handle_active && r.w.bx != (unsigned)g_handle_id) {
            r.h.dh = PD_ERR_BAD_HANDLE;
            r.w.flags |= 1;
            break;
        }
        if (g_rd == NULL || !rtl_reinit(g_rd)) {
            r.h.dh = PD_ERR_CANT_RESET;
            r.w.flags |= 1;
            break;
        }
        r.w.flags &= ~1;
        break;
    }

    case PD_FUNC_TERMINATE: {
        if (!g_handle_active || r.w.bx != (unsigned)g_handle_id) {
            r.h.dh = PD_ERR_BAD_HANDLE;
            r.w.flags |= 1;
            break;
        }
        g_handle_active = 0;
        g_receiver = NULL;
        r.w.flags &= ~1;
        break;
    }

    case PD_FUNC_GET_STATISTICS: {
        /* Spec (section 6.16): handle in BX, possible error BAD_HANDLE.
         * Output: DS:SI = far pointer to a struct statistics. The struct
         * is assembled fresh into g_stats_buf (a static, so the pointer
         * stays valid after this call returns, as required).
         *
         * DELIBERATE DEVIATION from strict handle-matching: the spec
         * ties this to a specific handle, but our counters are tracked
         * globally regardless of which handle (if any) is currently
         * registered - this call does not mutate anything, unlike
         * release_type()/terminate()/reset_interface(), where checking
         * the handle actually protects against one application
         * disturbing another's registration. Requiring a live, matching
         * handle here mostly gets in the way in practice: DOS is single-
         * tasking, so there is no second window to run a diagnostic tool
         * from WHILE the application holding the handle is still stuck
         * mid-transfer - by the time you could free up a prompt to check
         * statistics, the handle would usually already be gone. We only
         * require that the driver itself is initialised (g_rd != NULL);
         * a handle mismatch is not treated as an error here. */
        if (g_rd == NULL) {
            r.h.dh = PD_ERR_BAD_HANDLE;
            r.w.flags |= 1;
            break;
        }
        g_stats_buf.packets_in   = g_stat_packets_in;
        g_stats_buf.packets_out  = g_stat_packets_out;
        g_stats_buf.bytes_in     = g_stat_bytes_in;
        g_stats_buf.bytes_out    = g_stat_bytes_out;
        g_stats_buf.errors_in    = g_rd ? g_rd->stat_rx_errors : 0;
        g_stats_buf.errors_out   = g_stat_errors_out;
        g_stats_buf.packets_lost = g_stat_packets_lost;
        r.w.ds = FP_SEG(&g_stats_buf);
        r.w.si = FP_OFF(&g_stats_buf);
        r.w.flags &= ~1;
        break;
    }

    case PKTDRV_UNLOAD_AH: {
        /* Private extension to safely remove the driver - see the
         * explanation at PKTDRV_UNLOAD_AH in pktdrv.h. Check the magic
         * values first so a stray AH=0x80 call from something unrelated
         * cannot accidentally remove the driver. */
        if (r.w.bx != PKTDRV_UNLOAD_MAGIC_BX || r.w.cx != PKTDRV_UNLOAD_MAGIC_CX) {
            r.h.dh = PD_ERR_BAD_COMMAND;
            r.w.flags |= 1;
            break;
        }
        if (g_handle_active) {
            /* An application is still registered - do not just remove
             * ourselves, same caution as reset_interface(). */
            r.h.dh = PD_ERR_CANT_TERMINATE;
            r.w.flags |= 1;
            break;
        }
        /* It is safe to restore our interrupt vectors right now: we are
         * still executing from our own memory (which is only freed
         * AFTER this call, by the caller) - changing the vector table
         * does not affect what is currently executing. */
        pktdrv_uninstall();

        /* No DOS memory-free calls here - INT 21h AH=49h calls made from
         * inside our own interrupt handler turned out to have no effect
         * in practice (confirmed with an MCB-chain dump). Instead we
         * only fill in a list of segments that need freeing, and let the
         * caller (which does run in a normal, safe process context)
         * perform the actual freeing - see the detailed explanation at
         * PKTDRV_UNLOAD_AH in pktdrv.h. */
        if (g_rd) {
            unsigned i;
            g_unload_free_list.num_tx_desc = g_rd->num_tx_desc;
            g_unload_free_list.num_rx_desc = g_rd->num_rx_desc;
            g_unload_free_list.tx_ring_seg = g_rd->tx_ring_seg;
            g_unload_free_list.rx_ring_seg = g_rd->rx_ring_seg;
            g_unload_free_list.cap_seg = g_rd->cap_enabled ? g_rd->cap_seg : 0;
            for (i = 0; i < g_rd->num_tx_desc; i++) {
                g_unload_free_list.tx_buf_seg[i] = g_rd->tx_buf[i] ? FP_SEG(g_rd->tx_buf[i]) : 0;
            }
            for (i = 0; i < g_rd->num_rx_desc; i++) {
                g_unload_free_list.rx_buf_seg[i] = g_rd->rx_buf[i] ? FP_SEG(g_rd->rx_buf[i]) : 0;
            }
        } else {
            memset(&g_unload_free_list, 0, sizeof(g_unload_free_list));
        }

        r.w.ax = g_my_psp; /* the caller needs this to free our main memory block */
        r.w.es = FP_SEG((void far *)&g_unload_free_list);
        r.w.di = FP_OFF((void far *)&g_unload_free_list);
        r.w.flags &= ~1;
        break;
    }

    case PKTDRV_CAPDUMP_AH: {
        /* Private extension to read the optional capture ring buffer -
         * see the explanation at PKTDRV_CAPDUMP_AH in pktdrv.h. Unlike
         * PKTDRV_UNLOAD_AH, this does NOT check g_handle_active - it is
         * meant to be usable while an application is still running. */
        if (r.w.bx != PKTDRV_CAPDUMP_MAGIC_BX || r.w.cx != PKTDRV_CAPDUMP_MAGIC_CX) {
            r.h.dh = PD_ERR_BAD_COMMAND;
            r.w.flags |= 1;
            break;
        }
        if (g_rd == NULL || !g_rd->cap_enabled) {
            r.h.dh = PD_ERR_BAD_COMMAND; /* capture was never enabled at load time (no -c) */
            r.w.flags |= 1;
            break;
        }
        g_cap_dump_info.cap_seg       = g_rd->cap_seg;
        g_cap_dump_info.cap_num_slots = g_rd->cap_num_slots;
        g_cap_dump_info.cap_write_idx = g_rd->cap_write_idx;
        g_cap_dump_info.cap_count     = g_rd->cap_count;
        r.w.es = FP_SEG((void far *)&g_cap_dump_info);
        r.w.di = FP_OFF((void far *)&g_cap_dump_info);
        r.w.flags &= ~1;
        break;
    }

    default:
        r.h.dh = PD_ERR_BAD_COMMAND;
        r.w.flags |= 1;
        break;
    }
}

/* ---- install/uninstall ------------------------------------------------------ */

/* ---- INT 60h identification stub (Packet Driver Specification section 4) --
 *
 * REQUIRED, and initially overlooked: the handler at the configured
 * interrupt must START with a 3-byte jump instruction (the spec
 * explicitly recommends "a 2-byte jump followed by a NOP" rather than a
 * lone 'jmp short'), immediately followed by the null-terminated ASCII
 * text "PKT DRVR". Applications such as mTCP scan for this (or verify it
 * at the configured interrupt) to confirm a real packet driver is
 * present - without this signature, an otherwise perfectly working
 * driver is rejected with exactly the error "Could not setup packet
 * driver, are the configured interrupts correct?".
 *
 * A Watcom __interrupt function (pktdrv_isr) cannot have these exact
 * bytes at its own entry point, since the compiler places its own
 * register-saving prologue there. Solution: build this stub ourselves in
 * a static buffer and install THAT on INT 60h - the stub jumps straight
 * into pktdrv_isr. Because this is a JMP (not a CALL), the FLAGS/CS/IP
 * frame pushed by the CPU on "INT 60h" stays intact on the stack, so
 * pktdrv_isr's own __interrupt epilogue (IRET) works correctly.
 *
 * Layout (17 bytes):
 *   [0]    0xEB          jmp short
 *   [1]    0x0A (10)     displacement to offset 12
 *   [2]    0x90          nop            \_ together: the required 3-byte jump
 *   [3-10] "PKT DRVR"    (8 ASCII bytes, no quotes)
 *   [11]   0x00          null terminator
 *   [12]   0xEA          jmp far ptr16:16
 *   [13-14] offset of pktdrv_isr (low/high byte)
 *   [15-16] segment of pktdrv_isr (low/high byte)
 */
static unsigned char pktdrv_stub[17];

static void build_pktdrv_stub(void)
{
    void far *target = (void far *)pktdrv_isr;
    unsigned seg = FP_SEG(target);
    unsigned off = FP_OFF(target);

    pktdrv_stub[0]  = 0xEB;
    pktdrv_stub[1]  = 12 - 2; /* jmp short is 2 bytes; jump to offset 12 */
    pktdrv_stub[2]  = 0x90;
    memcpy(&pktdrv_stub[3], "PKT DRVR", 8);
    pktdrv_stub[11] = 0x00;
    pktdrv_stub[12] = 0xEA;
    pktdrv_stub[13] = (unsigned char)(off & 0xFF);
    pktdrv_stub[14] = (unsigned char)((off >> 8) & 0xFF);
    pktdrv_stub[15] = (unsigned char)(seg & 0xFF);
    pktdrv_stub[16] = (unsigned char)((seg >> 8) & 0xFF);
}

int pktdrv_install(rtl_dev_t *rd, unsigned char int_no)
{
    unsigned char irq_vec;
    union REGS regs;

    g_rd     = rd;
    g_int_no = int_no;
    g_irq_line = rd->irq_line;
    irq_vec  = irq_to_int_vector(g_irq_line);

    /* Capture our own PSP segment VIA DOS (INT 21h AH=62h "Get PSP
     * Address"), not via a runtime-library global such as _psp - which
     * turned out not to be reliably present in this toolchain. This MUST
     * happen here (during normal, non-resident execution) and not inside
     * the interrupt handler itself: DOS's "current process" pointer does
     * not change just because another program executes a plain "int", so
     * fetching it inside the PKTDRV_UNLOAD_AH handler would return the
     * CALLER's PSP (the "-u" invocation), not our own. */
    memset(&regs, 0, sizeof(regs));
    regs.h.ah = 0x62;
    int86(0x21, &regs, &regs);
    g_my_psp = regs.w.bx;

    build_pktdrv_stub();

    g_old_pktint = (pktdrv_intvect_t)_dos_getvect(g_int_no);
    g_old_hwint  = (pktdrv_intvect_t)_dos_getvect(irq_vec);

    /* NOTE: the STUB goes on INT 60h here, not pktdrv_isr directly - that
     * is precisely the fix for the "PKT DRVR" signature requirement. The
     * hardware IRQ vector has no such requirement (an application never
     * calls it directly), so that one stays on hw_isr unchanged. */
    _dos_setvect(g_int_no, (pktdrv_intvect_t)(void far *)pktdrv_stub);
    _dos_setvect(irq_vec,  (pktdrv_intvect_t)hw_isr);

    pic_unmask(g_irq_line);

    return 1;
}

void pktdrv_uninstall(void)
{
    unsigned char irq_vec = irq_to_int_vector(g_irq_line);
    /* Stop the card first (RX/TX off) - BEFORE the vectors are unhooked
     * and the descriptor memory is handed back to DOS. Plain hardware
     * I/O (outp/inp), not a DOS call, so it is safe to do this directly.
     * This also means our card will not assert the (possibly shared)
     * IRQ line again after this point, without us needing to touch the
     * PIC mask at all - see the note above pic_unmask() removal. */
    if (g_rd) rtl_stop(g_rd);
    if (g_old_pktint) _dos_setvect(g_int_no, g_old_pktint);
    if (g_old_hwint)  _dos_setvect(irq_vec, g_old_hwint);
}

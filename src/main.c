/*
 * main.c - Command-line front end for the RTL8169 DOS packet driver,
 * built as RTL8169P.EXE - the stripped-down "production" build: only
 * load-time options (-t/-r/-c/-g/-m) plus -u (uninstall). Diagnostics
 * against an already-loaded instance (-s/-d) were moved to RTLDEBUG.EXE
 * (rtldebug.c) to keep this executable's own resident memory footprint
 * smaller - see "Design notes" in DESIGN.md for why. -c is still
 * accepted here (the actual capture-recording code must stay resident
 * regardless, called from rtl_send()/rtl_poll_rx() on every frame - see
 * rtl_capture_frame() in rtl8169p.c) but is deliberately left out of
 * this executable's own usage text: without -d here to read a capture
 * back out, advertising -c would offer no real use - load with
 * RTLDEBUG.EXE instead if you want to use -c/-d together.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
 * Usage:
 *   RTL8169P <packet_int_no>
 *   RTL8169P -u <packet_int_no>
 *
 * Finds the RTL8169, initialises it (10/100/1000 Mbit, auto-negotiation),
 * hooks the packet-driver interrupt and the card's hardware IRQ, and then
 * stays resident (TSR) so TCP/IP stacks such as mTCP can talk to it via
 * INT 60h (or whichever interrupt was given on the command line).
 *
 * DELIBERATELY NO <stdio.h>/<stdlib.h>: printf() alone can pull in several
 * KB of format-string-parsing code from a statically linked C runtime -
 * disproportionate overhead for a packet driver (5-20KB in total is
 * typical). All text output below goes straight through DOS INT 21h
 * AH=02h (print one character), and numbers are converted to hex/decimal
 * with small hand-written helpers instead of printf's "%X"/"%u".
 */

#include <string.h>
#include <dos.h>
#include "rtl8169p.h"
#include "pktdrv.h"

/* Software release version of this project (not to be confused with
 * PD_VERSION in pktdrv.c, which is the Packet Driver Specification
 * protocol version reported to applications via driver_info() - a
 * different, unrelated number). Shown in the usage text and the normal
 * startup banner. */
#define RTL8169PD_VERSION "0.8.1"

/* Packet Driver Specification, section 4: "The packet driver is invoked
 * via a software interrupt in the range 0x60 through 0x80." We enforce
 * this range on the command line so a typo cannot end up hooking some
 * unrelated interrupt (e.g. a BIOS or DOS vector) by accident. */
#define PKTDRV_INT_MIN 0x60
#define PKTDRV_INT_MAX 0x80

static rtl_dev_t g_dev;

/* ---- minimal text output, no stdio ------------------------------------ */

static void dos_putchar(char c)
{
    union REGS r;
    r.h.ah = 0x02;
    r.h.dl = (unsigned char)c;
    int86(0x21, &r, &r);
}

static void dos_print(const char *s)
{
    while (*s) {
        dos_putchar(*s++);
    }
}

static void dos_print_hex_byte(unsigned char v)
{
    static const char digits[] = "0123456789ABCDEF";
    dos_putchar(digits[(v >> 4) & 0xF]);
    dos_putchar(digits[v & 0xF]);
}

static void dos_print_hex_word(unsigned v)
{
    dos_print_hex_byte((unsigned char)(v >> 8));
    dos_print_hex_byte((unsigned char)(v & 0xFF));
}

static void dos_print_dec(unsigned v)
{
    char buf[6];
    int n = 0;
    if (v == 0) {
        dos_putchar('0');
        return;
    }
    while (v > 0 && n < 5) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        dos_putchar(buf[--n]);
    }
}

static void dos_print_ulong(unsigned long v)
{
    char buf[11]; /* max "4294967295" = 10 digits */
    int n = 0;
    if (v == 0) {
        dos_putchar('0');
        return;
    }
    while (v > 0 && n < 10) {
        buf[n++] = (char)('0' + (unsigned)(v % 10UL));
        v /= 10UL;
    }
    while (n > 0) {
        dos_putchar(buf[--n]);
    }
}

/* ---- small hex parser, instead of stdlib's strtol --------------------- */

/* Parses s as a hexadecimal interrupt number (an optional "0x"/"0X"
 * prefix is accepted). Returns 1 and fills *out if s is a well-formed
 * hex number AND falls within the legal packet-driver interrupt range
 * (0x60-0x80); returns 0 otherwise (empty string, invalid characters, or
 * out of range) so the caller can show the usage text instead of
 * silently doing the wrong thing. */
static int parse_int_no(const char *s, unsigned char *out)
{
    unsigned v = 0;
    int digits = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (*s) {
        char c = *s++;
        unsigned digit;
        if (c >= '0' && c <= '9')      digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else return 0; /* invalid character */
        v = (v << 4) | digit;
        digits++;
    }
    if (digits == 0) return 0;
    if (v < PKTDRV_INT_MIN || v > PKTDRV_INT_MAX) return 0;

    *out = (unsigned char)v;
    return 1;
}

/* ---- usage text --------------------------------------------------------- */

static void print_banner(void)
{
    dos_print("RTL8169 packet driver for DOS, version " RTL8169PD_VERSION "\r\n");
    dos_print("Copyright (c) 2026 by Yoghoo\r\n");
}

static void print_usage(void)
{
    print_banner();
    dos_print("\r\n");
    dos_print("Usage: RTL8169P [-t n] [-r n] [-g us] [-m hex] <packet_int_no>\r\n");
    dos_print("       RTL8169P -u <packet_int_no>\r\n");
    dos_print("\r\n");
    dos_print("   packet_int_no -- Interrupt to use, 0x60-0x80 (required)\r\n");
    dos_print("   -u            -- Uninstall the driver, free its memory\r\n");
    dos_print("   -t n          -- TX descriptors/buffers (1-");
    dos_print_dec(RTL_MAX_TX_DESC);
    dos_print(", default ");
    dos_print_dec(RTL_DEFAULT_TX_DESC);
    dos_print(")\r\n");
    dos_print("   -r n          -- RX descriptors/buffers (1-");
    dos_print_dec(RTL_MAX_RX_DESC);
    dos_print(", default ");
    dos_print_dec(RTL_DEFAULT_RX_DESC);
    dos_print(")\r\n");
    dos_print("   -g us         -- RX burst gap, microseconds (0-");
    dos_print_dec(RTL_MAX_BURST_GAP_US);
    dos_print(", default ");
    dos_print_dec(RTL_DEFAULT_BURST_GAP_US);
    dos_print(")\r\n");
    dos_print("   -m hex        -- Raw IntrMitigate register value (0x0000-0xFFFF,\r\n");
    dos_print("                    default 0x5151) - paired with -g 0 by default;\r\n");
    dos_print("                    see README.md before changing either alone.\r\n");
    dos_print("\r\n");
    dos_print("See README.md for details on each option, and RTLDEBUG.EXE for\r\n");
    dos_print("diagnostics (-s/-d) against an already-loaded driver instance.\r\n");
}

/* ---- unload (-u) ---------------------------------------------------------
 *
 * Two steps, for the well-known reason that a program can never safely
 * free its OWN memory while still running from it:
 *   1) call the driver's own private extension (PKTDRV_UNLOAD_AH) - this
 *      unhooks its interrupt vectors and returns its PSP segment, but
 *      does not free its own memory.
 *   2) WE (a freshly started, separate process, so this is safe) then
 *      free that memory ourselves via DOS INT 21h AH=49h.
 */
static void report_dos_error(const char *what, unsigned err_code)
{
    dos_print("Error ");
    dos_print(what);
    dos_print(": DOS error code ");
    dos_print_dec(err_code);
    dos_print("\r\n");
}

static void dos_free_segment(unsigned seg, const char *label)
{
    union REGS r;
    struct SREGS sregs;

    if (seg == 0) return;

    memset(&r, 0, sizeof(r));
    memset(&sregs, 0, sizeof(sregs));
    r.h.ah = 0x49; /* DOS: free memory block */
    /* According to Watcom's own documentation, DS must hold a valid
     * value for int86x() (do not just leave it at 0) - use the current
     * DS via a near-to-far cast on a local variable. */
    sregs.ds = FP_SEG((void far *)&r);
    sregs.es = seg;
    int86x(0x21, &r, &r, &sregs);
    if (r.x.cflag) {
        report_dos_error(label, r.w.ax);
    }
}

/* Spec-conformant signature check (Packet Driver Specification, section
 * 4): a valid packet driver starts with a 3-byte jump, immediately
 * followed by the text "PKT DRVR". Used to:
 *   - confirm a driver is really present before -u/-s touch its vector
 *     (prevents an incorrect interrupt number from crashing some
 *     unrelated program), and
 *   - refuse to load a second copy on top of an already-loaded one (see
 *     "Known limitations" in the README for what a stacked double-load
 *     could otherwise do to the shared hardware state). */
static int packet_driver_present(unsigned char int_no)
{
    void (__interrupt __far *vec)(void);
    unsigned char far *sig;
    int i;
    static const char expected[8] = "PKT DRVR";

    vec = _dos_getvect(int_no);
    sig = (unsigned char far *)vec;

    for (i = 0; i < 8; i++) {
        if (sig[3 + i] != (unsigned char)expected[i]) return 0;
    }
    return 1;
}

static void do_unload(unsigned char int_no)
{
    union REGS r;
    struct SREGS sregs;
    unsigned psp_seg, env_seg;
    rtl_free_list_t far *flist;
    rtl_free_list_t local_flist;
    int i;

    print_banner();
    dos_print("\r\n");

    if (!packet_driver_present(int_no)) {
        dos_print("No recognisable packet driver found on interrupt 0x");
        dos_print_hex_byte(int_no);
        dos_print(".\r\n");
        return;
    }

    /* NOTE: this needs int86x() (not plain int86()) because we need
     * ES:DI back (the far pointer to the free list) - union REGS.w has
     * no es/ds fields, only struct SREGS does. */
    memset(&r, 0, sizeof(r));
    memset(&sregs, 0, sizeof(sregs));
    r.h.ah = PKTDRV_UNLOAD_AH;
    r.w.bx = PKTDRV_UNLOAD_MAGIC_BX;
    r.w.cx = PKTDRV_UNLOAD_MAGIC_CX;
    int86x(int_no, &r, &r, &sregs);

    if (r.x.cflag) {
        dos_print("Could not remove the driver (error code ");
        dos_print_dec(r.h.dh);
        dos_print(").\r\nIs it still in use by a TCP/IP stack (e.g. mTCP)?\r\n");
        dos_print("Shut that down first and try again.\r\n");
        return;
    }

    psp_seg = r.w.ax;
    dos_print("Driver unhooked from interrupt 0x");
    dos_print_hex_byte(int_no);
    dos_print(".\r\n  Driver PSP segment: 0x");
    dos_print_hex_word(psp_seg);
    dos_print("\r\n");

    /* IMPORTANT: the free list (tx_ring/rx_ring/all buffer segments)
     * still lives in the driver's own memory (a far pointer to it was
     * returned via ES:DI) - that memory still needs to be freed, so we
     * FIRST copy the list to our OWN local memory, before freeing
     * anything at all. Without this copy we could accidentally free the
     * list itself before finishing reading it. */
    flist = (rtl_free_list_t far *)MK_FP(sregs.es, r.w.di);
    _fmemcpy(&local_flist, flist, sizeof(local_flist));

    /* The driver's environment segment (PSP+0x2C, standard DOS PSP
     * layout) is looked up separately and freed too, or it would be
     * left behind. */
    env_seg = *(unsigned far *)MK_FP(psp_seg, 0x2C);
    dos_print("  Environment segment: 0x");
    dos_print_hex_word(env_seg);
    dos_print("\r\n");

    /* Free the descriptor rings and packet buffers FIRST (order does not
     * actually matter here since the segment values were already copied
     * safely into local_flist above). This now happens FROM THIS
     * (calling) PROCESS, not from the driver itself - see PKTDRV_UNLOAD_AH
     * in pktdrv.h for why. */
    dos_print("Freeing descriptor rings and packet buffers...\r\n");
    dos_free_segment(local_flist.tx_ring_seg, "freeing tx_ring");
    dos_free_segment(local_flist.rx_ring_seg, "freeing rx_ring");
    for (i = 0; i < local_flist.num_tx_desc; i++) {
        dos_free_segment(local_flist.tx_buf_seg[i], "freeing tx_buf");
    }
    for (i = 0; i < local_flist.num_rx_desc; i++) {
        dos_free_segment(local_flist.rx_buf_seg[i], "freeing rx_buf");
    }
    if (local_flist.cap_seg != 0) {
        dos_print("Freeing capture buffer...\r\n");
        dos_free_segment(local_flist.cap_seg, "freeing capture buffer");
    }

    dos_print("Freeing environment block...\r\n");
    dos_free_segment(env_seg, "freeing environment block");

    /* Always free the main memory block LAST: it holds the driver's code
     * and data (including local_flist's original source), so everything
     * still needed from it must be secured first - which has already
     * happened above. */
    dos_print("Freeing main memory block...\r\n");
    dos_free_segment(psp_seg, "freeing main memory block");

    dos_print("Done.\r\n");
}


/* Small decimal-to-unsigned parser for -t/-r (no <stdlib.h>, see the
 * file header - this is the same reasoning as parse_int_no() below, just
 * base 10 instead of base 16). Returns 0 on an empty/invalid string,
 * which the caller treats as "not a valid count". */
static unsigned parse_decimal(const char *s)
{
    unsigned v = 0;
    if (*s == 0) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        v = (unsigned)(v * 10 + (unsigned)(*s - '0'));
        s++;
    }
    return v;
}

/* For -m: unlike parse_decimal() above, 0 is itself a valid, meaningful
 * result here ("disabled" - see RTL_DEFAULT_INTR_MITIGATE in
 * rtl8169p.h), so it cannot double as a parse-failure sentinel the way
 * parse_decimal()'s 0 does. Returns success/failure via the return
 * value instead (matching parse_int_no()'s pattern), with the parsed
 * value written to *out only on success. Accepts a leading "0x"/"0X"
 * (optional - digits alone also work), same as parse_int_no(). */
static int parse_hex16(const char *s, unsigned *out)
{
    unsigned v = 0;
    int digits = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    while (*s) {
        char c = *s++;
        unsigned digit;
        if (c >= '0' && c <= '9')      digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
        else return 0; /* invalid character */
        v = (v << 4) | digit;
        digits++;
        if (digits > 4) return 0; /* more than 16 bits' worth of hex digits */
    }
    if (digits == 0) return 0;

    *out = v;
    return 1;
}

int main(int argc, char *argv[])
{
    unsigned char int_no;
    unsigned i;
    int uninstall = 0;
    int argi = 1;
    const char *int_arg = NULL;
    unsigned tx_desc_arg = 0; /* 0 = use the built-in default, see rtl8169p.h */
    unsigned rx_desc_arg = 0;
    unsigned cap_size_kb = 0; /* 0 = capture disabled (the no-cost default) */
    unsigned burst_gap_arg = RTL_BURST_GAP_UNSET; /* -g not given -> use the default (see rtl8169p.h) */
    unsigned intr_mitigate_arg = RTL_DEFAULT_INTR_MITIGATE; /* -m not given -> disabled, its own natural default */

    /* Options may appear in any order before the required packet_int_no.
     * This is the stripped-down RTL8169P.EXE front end - only load-time
     * options plus -u remain; -s/-d (diagnostics against an already-
     * running instance) were moved to RTLDEBUG.EXE (rtldebug.c) to keep
     * this executable's resident footprint smaller. Either executable
     * can send -u/-s/-d to a driver instance regardless of which one
     * loaded it - see the comment at the top of rtldebug.c. */
    while (argi < argc) {
        if (strcmp(argv[argi], "-u") == 0) {
            uninstall = 1;
            argi++;
        } else if (strcmp(argv[argi], "-t") == 0) {
            argi++;
            if (argi < argc) { tx_desc_arg = parse_decimal(argv[argi]); argi++; }
        } else if (strcmp(argv[argi], "-r") == 0) {
            argi++;
            if (argi < argc) { rx_desc_arg = parse_decimal(argv[argi]); argi++; }
        } else if (strcmp(argv[argi], "-c") == 0) {
            argi++;
            if (argi < argc) { cap_size_kb = parse_decimal(argv[argi]); argi++; }
        } else if (strcmp(argv[argi], "-g") == 0) {
            argi++;
            if (argi < argc) { burst_gap_arg = parse_decimal(argv[argi]); argi++; }
        } else if (strcmp(argv[argi], "-m") == 0) {
            argi++;
            if (argi < argc) {
                if (!parse_hex16(argv[argi], &intr_mitigate_arg)) {
                    dos_print("-m needs a hexadecimal value, e.g. -m 0x5151\r\n");
                    return 1;
                }
                argi++;
            }
        } else {
            int_arg = argv[argi];
            argi++;
        }
    }

    if (int_arg == NULL || !parse_int_no(int_arg, &int_no)) {
        print_usage();
        return 1;
    }

    if ((tx_desc_arg != 0 && (tx_desc_arg < 1 || tx_desc_arg > RTL_MAX_TX_DESC)) ||
        (rx_desc_arg != 0 && (rx_desc_arg < 1 || rx_desc_arg > RTL_MAX_RX_DESC))) {
        dos_print("-t/-r must be between 1 and ");
        dos_print_dec(RTL_MAX_TX_DESC);
        dos_print(" (TX) / ");
        dos_print_dec(RTL_MAX_RX_DESC);
        dos_print(" (RX).\r\n");
        return 1;
    }

    if (cap_size_kb != 0 && cap_size_kb > CAP_MAX_KB) {
        dos_print("-c must be between 1 and ");
        dos_print_dec(CAP_MAX_KB);
        dos_print(" (KB).\r\n");
        return 1;
    }

    if (burst_gap_arg != RTL_BURST_GAP_UNSET && burst_gap_arg > RTL_MAX_BURST_GAP_US) {
        dos_print("-g must be between 0 and ");
        dos_print_dec(RTL_MAX_BURST_GAP_US);
        dos_print(" (microseconds).\r\n");
        return 1;
    }

    if (uninstall) {
        do_unload(int_no);
        return 0;
    }

    print_banner();
    dos_print("\r\n");

    /* Refuse to load a second copy on top of an already-loaded one. Two
     * instances would both try to own the same physical card: the
     * second one's init would reset the chip and repoint its descriptor
     * registers at its OWN buffers, effectively hijacking the hardware
     * out from under the first instance while that first instance's
     * code, ISR hook, and memory all stay resident regardless - wasted
     * memory at best, and a real hazard if the first instance's
     * interrupt handler ever runs again mid-transfer (e.g. via IRQ
     * chaining) expecting hardware state that the second instance has
     * since changed. */
    if (packet_driver_present(int_no)) {
        dos_print("A packet driver is already loaded on interrupt 0x");
        dos_print_hex_byte(int_no);
        dos_print(".\r\n");
        dos_print("Run \"RTL8169P -u 0x");
        dos_print_hex_byte(int_no);
        dos_print("\" first if you want to reload it.\r\n");
        return 1;
    }

    dos_print("Searching for a Realtek RTL8169 on the PCI bus...\r\n");

    if (!rtl_probe_and_init(&g_dev, tx_desc_arg, rx_desc_arg, burst_gap_arg, intr_mitigate_arg)) {
        dos_print("No RTL8169 found (or no usable I/O BAR). Stopping.\r\n");
        return 1;
    }

    if (cap_size_kb != 0) {
        if (rtl_capture_init(&g_dev, cap_size_kb)) {
            dos_print("Capture enabled: ");
            dos_print_dec(g_dev.cap_num_slots);
            dos_print(" frames (");
            dos_print_dec(cap_size_kb);
            dos_print("KB). Use -d <file> to dump it.\r\n");
        } else {
            dos_print("Could not enable capture (out of memory?) - continuing without it.\r\n");
        }
    }

    /* Only ONE of RX burst gap and interrupt mitigation needs to be
     * active to avoid the confirmed data-corruption bug (see the
     * README/DESIGN.md) - the two defaults (burst gap 0, mitigation
     * 0x5151) are a validated PAIR, not independent settings, so the
     * one combination genuinely worth warning about is BOTH disabled at
     * once ("-g 0 -m 0"), which reverts to the original, confirmed-
     * buggy behaviour. */
    if (g_dev.burst_gap_us == 0 && g_dev.intr_mitigate == 0) {
        dos_print("WARNING: RX burst gap AND interrupt mitigation are both\r\n");
        dos_print("         disabled - this re-enables a confirmed data-\r\n");
        dos_print("         corruption bug under bursty traffic.\r\n");
    } else {
        dos_print("RX burst gap: ");
        dos_print_dec(g_dev.burst_gap_us);
        dos_print(" us\r\n");
        dos_print("Interrupt mitigation: 0x");
        dos_print_hex_word(g_dev.intr_mitigate);
        dos_print("\r\n");
    }

    dos_print("Found: I/O base 0x");
    dos_print_hex_word(g_dev.io_base);
    dos_print(", IRQ ");
    dos_print_dec(g_dev.irq_line);
    dos_print(", MAC ");
    for (i = 0; i < 6; i++) {
        dos_print_hex_byte(g_dev.mac[i]);
        dos_print((i < 5) ? ":" : "\r\n");
    }

    /* Install the packet driver (and unmask the hardware IRQ) BEFORE
     * waiting for link, not after. RX/TX were already enabled inside
     * rtl_probe_and_init() - if the (potentially multi-second) link
     * wait happened first, there would be a window where the card is
     * already actively receiving but the hardware interrupt is still
     * masked at the PIC, with nothing to process whatever the card
     * puts in the RX ring during that time until the very first
     * interrupt is finally allowed through. Every other network card/
     * driver combination this project has been tested against gets
     * DHCP right first try; this one did not, consistently - closing
     * this window is the leading candidate fix for that. */
    if (!pktdrv_install(&g_dev, int_no)) {
        dos_print("Could not install the packet-driver interrupt 0x");
        dos_print_hex_byte(int_no);
        dos_print(".\r\n");
        return 1;
    }

    rtl_wait_for_link(&g_dev, 8000);

    if (g_dev.link_up) {
        dos_print("Link: UP, ");
        dos_print_dec(g_dev.link_speed_mbit);
        dos_print(" Mbit, ");
        dos_print(g_dev.link_full_duplex ? "full" : "half");
        dos_print("-duplex\r\n");
    } else {
        dos_print("Link: NO LINK (cable unplugged, port down, or auto-negotiation\r\n");
        dos_print("      did not complete within 8s). The driver continues anyway -\r\n");
        dos_print("      check the link LED and the switch port if DHCP/ping fail.\r\n");
    }

    dos_print("Packet driver active on interrupt 0x");
    dos_print_hex_byte(int_no);
    dos_print(".\r\n");
    dos_print("Going resident (TSR)...\r\n");

    /* 0x1000 paragraphs (64KB) requested as a safe CEILING, not a
     * precise calculation of what is strictly needed - see "Known
     * limitations" in the README for why this is not actually a trade-
     * off: DOS can only shrink this block down to what the linker's own,
     * much smaller MaxAlloc already gave the program at load time, never
     * grow it, so asking for a generous ceiling here costs nothing in
     * practice (confirmed via direct MCB-chain inspection during the
     * memory-leak investigation).
     *
     * A precise, EtherDFS-style resident-size calculation (explicit
     * BEGTEXT/RESDATA segments placed first via a linker ORDER
     * directive, exact size from CS-DS) was attempted and REVERTED -
     * see DESIGN.md for the full story. Real-world testing showed a
     * custom data segment/class is not automatically part of DGROUP,
     * so DS did not point where the scheme assumed; the resulting
     * driver loaded but DHCP no longer worked, and unloading no longer
     * freed memory correctly. Parked for now - would need a real way to
     * fold a custom segment into DGROUP (not just place it nearby) to
     * revisit safely. */
    _dos_keep(0, 0x1000);

    /* Never reached - _dos_keep does not return. */
    return 0;
}

# RTL8169 Packet Driver - Design Notes

Detailed reasoning behind this driver's known limitations, followed by
the full design/debugging history. Moved out of `README.md` to keep that
file short for anyone just landing on the project page - see there for
the short version of each limitation.

## Known limitations - detailed reasoning


- **Single handle/interface only.** No multicast list management, no
  chaining to a further INT 60h handler if another driver already shares
  the same interrupt vector. This is enough for one TCP/IP stack
  (mTCP/WatTCP), not for several packet-driver clients running at once.
- **Link changes are not monitored after startup.** The link is checked
  once, at load time. Unplugging and replugging the cable while the
  driver is running is not detected.
- **No jumbo frames**, and no VLAN/checksum offload - the C+ mode
  descriptor bits for these are deliberately left off.
- **No per-chip-revision workarounds.** Linux's `r8169_main.c` carries
  dozens of MAC-version-specific quirks (RTL8169 vs. RTL8168 vs. RTL8110
  and their sub-revisions); this driver uses one generic init sequence
  and has only been tested against one specific card.
- **Gigabit throughput is architecturally limited.** `rtl_poll_rx()`
  drains every ready descriptor within a single hardware interrupt
  (several frames per interrupt under load, not one), including the
  full two-stage packet-driver upcall to the application for each -
  there is still a real per-frame processing cost, just not literally
  "one interrupt per frame" any more. There is no interrupt coalescing
  (the RTL8169's `IntrMitigate` register is not used) - and enabling it
  would need real caution, not just implementation effort: coalescing
  works by deliberately making RX bursts LARGER and less frequent, which
  is the opposite of what the confirmed burst-gap fix above does (see
  its long comment in `rtl8169p.c`) to avoid a real data-corruption bug
  tied to bursty back-to-back RX delivery. Combining the two would need
  the same re-validation rigour (~20 repeated real-world transfers) the
  burst-gap fix itself received, not just adding it and assuming the two
  coexist safely. RX ring depth is runtime-configurable (`-r`, default
  `RTL_DEFAULT_RX_DESC` = 8) rather than a fixed value, and was tuned via
  repeated real-world testing rather than chosen arbitrarily - see the
  long comment at `RTL_DEFAULT_RX_DESC` in `rtl8169p.h`. None of this
  changes the basic ceiling: do not expect to saturate a gigabit link
  from DOS with this driver, only fine for normal use (DHCP, ping,
  terminal-style traffic, file transfers at well under line rate).
- **EEPROM MAC address is not bit-banged**; the driver simply trusts the
  MAC0-5 I/O registers, which the card's firmware/BIOS normally loads on
  power-up. This matches upstream Linux's own eventual approach for the
  classic RTL8169 (an earlier attempt at active EEPROM reads was added
  and then reverted from `r8169_main.c` for reliability reasons) - see
  the source comments for details. A card with genuinely corrupted
  autoload EEPROM contents would still need extra handling here.
- **Resident memory size is requested as a fixed 64KB ceiling**
  (`_dos_keep(0, 0x1000)`), not a value computed from the actual linked
  program size - but this is NOT the same as "64KB is actually kept
  resident". Confirmed via direct MCB-chain inspection during the
  memory-leak investigation (see the design notes above): `_dos_keep()`
  can only SHRINK a program's memory block down to at most what DOS
  already allocated to it at load time (governed by the linker's own,
  much smaller MaxAlloc setting for this program) - it can never grow
  the block beyond that, regardless of the value passed. The actual
  resident size has always tracked the real buffer/descriptor
  configuration in every measurement taken throughout this project's
  memory-optimisation work (74KB -> 54KB -> 51KB -> 48KB -> 45KB as
  buffer counts were reduced) - if the 0x1000 request were literally
  being kept resident, those numbers would not have moved together like
  that. In other words: asking for a generous 64KB ceiling here costs
  nothing in practice, since DOS silently caps it to the real, much
  smaller size anyway - there was never a "safe overallocation" trade-
  off to weigh, just no reason to ask for less than a safe upper bound.


## Design notes


A few implementation choices are documented in detail as source comments
because they were not obvious in advance and cost real debugging time to
get right:

- **Auto-negotiation, not a forced speed.** An earlier version forced
  100 Mbit/full duplex. In practice this risks a duplex mismatch: a link
  partner that itself auto-negotiates can detect the forced speed via
  "parallel detection" but not the duplex mode, and falls back to half
  duplex - causing exactly the kind of packet loss/ARP timeouts this
  driver hit during development. See `rtl_hw_start()` in `rtl8169p.c`.
- **The RTL8169 uses a completely different RX/TX architecture from the
  RTL8139**: descriptor rings with an OWN bit toggled between driver and
  NIC, not the RTL8139's streaming ring-buffer mode. Code and ideas
  cannot be ported directly between the two chip families beyond the
  PCI-detection and packet-driver-dispatch layers.
  See `rtl8169p.h`/`rtl8169p.c` for the descriptor layout, cross-checked
  against Linux's `r8169_main.c` and, for a second opinion, QEMU's/86Box's
  RTL8139C+ emulation (which uses a closely related descriptor format).
- **The PHY has a paged register set.** Writes to gigabit-related MII
  registers (e.g. register 9, the 1000BASE-T control register) can land
  on the wrong, vendor-specific register bank if the PHY is not first
  forced back to its default page (`MII_PAGE_SELECT`, register 0x1F).
  See `rtl_hw_start()`.
- **Full auto-negotiation completion is only reliably indicated by the
  standard IEEE `BMSR` "Auto-Negotiation Complete" bit**, not by the
  RTL8169-specific `PHYstatus.LinkStatus` bit, which was observed to go
  high during an intermediate state before gigabit master/slave training
  had actually finished. See `rtl_wait_for_link()`.
- **The INT 60h identification stub.** Section 4 of the Packet Driver
  Specification requires the handler at the configured interrupt to
  start with a 3-byte jump followed by the null-terminated text
  `"PKT DRVR"`, so client stacks (mTCP included) can verify a real packet
  driver is present. A Watcom `__interrupt` function cannot have this
  exact byte sequence at its own entry point (the compiler's own
  prologue goes there instead), so a small stub is built at runtime in a
  static buffer and installed on the interrupt instead; it jumps straight
  into the real handler. See `build_pktdrv_stub()` in `pktdrv.c`.
- **Uninstalling a TSR cleanly is unusually fragile in DOS.** A running
  program can never safely free its own memory. The resident driver's
  private `-u` extension (`PKTDRV_UNLOAD_AH`, in the AH range 0x80-0xFF
  reserved by the spec for private use) therefore only unhooks its
  interrupt vectors and *reports* which DOS memory segments need
  freeing; the actual `INT 21h AH=49h` calls are all made by the
  freshly started `-u` process, which runs in a safe, separate context.
  An earlier version tried to free memory from inside the driver's own
  interrupt handler and had no effect at all - confirmed by walking the
  DOS memory-control-block chain by hand. See `pktdrv.h` and
  `do_unload()` in `main.c`.
- **`_fmalloc()`/`_ffree()` are not used** for the DMA descriptor rings
  and packet buffers. Watcom's far-heap allocator appears to keep its
  own internal arena and does not return the underlying DOS memory block
  to the system even after every individual `_ffree()` call - fine for a
  normal program, fatal for a TSR that is expected to fully vacate
  memory on `-u`. Buffers are instead allocated directly via
  `INT 21h AH=48h`/`49h`, with the driver tracking the exact raw DOS
  segment of each allocation itself. See the top of `rtl8169p.c`.
- **`send_pkt()` waits briefly (calibrated) for a full TX ring to drain,
  rather than either busy-waiting with an uncalibrated delay or failing
  immediately.** Two earlier versions got this wrong in opposite
  directions:
  - The original version busy-waited for up to ~2000 iterations using
    the uncalibrated `io_delay()` (a plain port-0x80 loop) - no timing
    guarantee, and needlessly long on a fast machine.
  - A later "simplification" removed the wait entirely on the reasoning
    that "there is no send queue, so waiting cannot help". That
    reasoning only holds for a genuinely stuck ring (cable pulled,
    hardware wedged) - during ordinary bulk transfers (e.g. an FTP
    upload keeping several frames in flight) a full ring is a normal,
    momentary state that clears within tens of microseconds at
    100/1000 Mbit. Failing immediately handed `CANT_SEND` back to the
    application for sends that would have succeeded a fraction of a
    millisecond later. In practice this caused hung FTP uploads and
    wildly inconsistent ping times, because mTCP's own, much coarser
    TCP retransmission timers took over instead of the packet driver
    simply retrying.

  The current version polls the descriptor for up to 2ms in 50us steps
  using the calibrated `udelay_dos()` (see the PIT-based delay
  discussion above) - generous enough for real drainage at any
  supported link speed, and still fails fast for an actually stuck ring.
- **`get_statistics()` (function 24) is implemented**, tracking
  packets/bytes in and out, RX errors (from the hardware error bits),
  TX errors, and packets lost (RX ring overflow or the application
  declining a buffer in the RX upcall). `driver_info()` reports
  functionality level 2 ("basic + extended") accordingly. See the
  counters in `pktdrv.c` and `rtl_dev_t.stat_rx_errors` in `rtl8169p.h`.
- **The hardware IRQ line is properly shared with other devices.** PCI
  only has 4 physical interrupt pins for the whole bus, so a given IRQ
  line is very often shared between several cards. `hw_isr()` first
  checks the RTL8169's own status register; if it is 0, the interrupt
  was not caused by this card, and control is handed to whatever
  handler was previously on that vector via Watcom's `_chain_intr()`
  (which reconstructs the stack so that handler's own `IRET` returns to
  the original caller, exactly as if this driver were not installed at
  all). For the same reason, `pktdrv_uninstall()` no longer masks the
  IRQ line at the PIC on unload - doing so would silently disable
  interrupts for any other device still sharing that line; `rtl_stop()`
  already ensures the RTL8169 itself stops asserting the line.

- **The driver refuses to load a second copy on top of an already-loaded
  one.** Loading twice on the same interrupt would leave two instances
  both believing they own the physical card: the second instance's init
  resets the chip and repoints its descriptor registers at its own
  buffers, effectively hijacking the hardware from the first instance,
  while the first instance's code, ISR hook, and memory all stay
  resident regardless. `main.c` checks for the "PKT DRVR" signature
  (see `packet_driver_present()`) before installing, and refuses with a
  clear message (suggesting `-u` first) if the interrupt is already in
  use.

- **TX DMA burst size and early-transmit threshold now set explicitly.**
  This was the root cause of a long-standing bug: uploads (DOS
  transmitting) failed and stalled specifically at the standard 1500
  MTU, while working fine at a smaller MTU (576) and while receiving
  equally large frames was never a problem. Confirmed via a packet
  capture on the receiving end showing DOS-transmitted TCP segments
  simply never arriving. Root cause (verified against the actual
  RTL8169 datasheet, Realtek doc rev 1.21): the `TxConfig` write only
  set the InterFrameGap bits and left the DMA burst-size field (MXDMA,
  bits 8-10) at its power-on default of 16 bytes. A ~1514-byte frame
  needs roughly 95 separate 16-byte DMA bursts to fully load into the
  TX FIFO; combined with the chip's "early transmit" feature (which can
  start putting bits on the wire before the whole frame has finished
  loading), a large frame at that burst size is at real risk of a FIFO
  underrun mid-frame - a ~590-byte frame either finishes loading before
  transmission starts or needs far fewer bursts either way. Fixed by
  explicitly setting the burst field to 1024 bytes (matching the
  well-tested value used by both the Linux r8169 driver and U-Boot's
  rtl8169 driver) and by disabling early transmit entirely via
  `EarlyTxThres` (`NoEarlyTx`), removing the underrun risk at its root
  rather than just reducing its likelihood. See the long comment at the
  `TxConfig` write in `rtl8169p.c`.

- **"First attempt after loading fails" pattern - PHY-settling theory
  tried and disproven.** Seen across multiple unrelated contexts: the
  first DHCP request timing out, mTCP's file buffer stalling the first
  time it filled, the first FTP download when acting as a server - every
  subsequent attempt in the same session works normally. A 2-second
  settle delay after link-up (on the theory that the PHY's digital
  "link up" signal does not necessarily mean the physical layer has
  fully settled) was tried and made no difference. More importantly: a
  stuck transfer that is manually aborted and immediately retried (same
  session, same link, no reload) succeeds right away, with no comparable
  delay before the retry. Since a fresh retry has no more elapsed time
  for any physical-layer settling than the original attempt did, this
  rules out a link/hardware warm-up explanation and points instead at
  something specific to that first TCP connection/session (most likely
  in mTCP's own ARP cache or per-connection state) rather than at the
  driver or the PHY. Not yet resolved - the working theory going into
  the next round is application-side (mTCP), not driver-side.

- **Hardware IRQ is now unmasked before the (potentially multi-second)
  link wait, not after.** Previously, `rtl_probe_and_init()` enabled
  RX/TX and then waited for link (up to 8s) before `pktdrv_install()`
  ever ran - meaning the card could already be actively receiving while
  the hardware interrupt was still masked at the PIC, with the RX ring
  unattended until the very first interrupt was finally allowed through
  at the end of that whole window. This was the leading suspect for a
  consistent "first DHCP request after loading always fails, every
  retry succeeds" pattern - notably NOT reproduced with any other NIC/
  driver combination tested. `rtl_wait_for_link()` was moved out of
  `rtl_probe_and_init()` and is now called explicitly from `main.c`,
  positioned after `pktdrv_install()` (which unmasks the IRQ) instead of
  before it. `rtl_reinit()` (used by `reset_interface()`) is unaffected
  - the IRQ is already unmasked by the time that runs.

- **Known environmental quirk: gigabit negotiation and first-DHCP-
  attempt failures with some routers.** During development, the first
  DHCP request right after loading the driver consistently failed
  (always succeeding on mTCP's built-in retry) on one specific router,
  while 20+ other NIC/driver combinations (NE2000, RTL8139, various
  buses) on the *same* network and DHCP server always succeeded on the
  first try. The common factor: this driver was the only one of those
  negotiating **gigabit** (NE2000 typically only does 10BASE-T link
  pulses, no real negotiation; RTL8139 caps at 100 Mbit) - gigabit auto-
  negotiation (IEEE 802.3 clause 40 master/slave clock resolution) is
  measurably slower/more involved than 10/100 negotiation, and some
  router/switch firmware applies extra caution specifically around a
  port that just completed one. Confirmed by direct test: physically
  connecting through a 100 Mbit-only switch (forcing real, physical
  100 Mbit negotiation, no code changes) made the first attempt succeed
  every time on the same router chain. This is a property of specific
  router/switch hardware reacting to a fresh gigabit link, not a driver
  bug, and auto-negotiation (including gigabit) is kept as the driver's
  default. If you hit this symptom on your own network: either accept
  mTCP's built-in DHCP retry (already handles it gracefully, see
  `DHCP.CPP`'s `-retries`/`-timeout` options to tune the budget), or
  force `BMCR_FORCE_100_FULL` instead of auto-negotiation in
  `rtl8169p.c` if gigabit is not needed on that particular network (note
  the known duplex-mismatch risk of forcing, documented at that same
  spot in the code).

- **Memory footprint reduced further: asymmetric TX/RX descriptor counts
  (now runtime-configurable via `-t`/`-r`), plus a smaller explicit
  stack.** Measured resident usage had crept up to ~74KB (16+16
  descriptors at 1536 bytes each = 48KB of buffers alone, plus the
  ~22KB main block and ~1KB environment block) - too large for comfort
  in a DOS environment. Splitting out non-resident init code (the other
  obvious lever) turned out to have limited headroom once actually
  measured: the whole `.EXE` is only ~13KB, so even perfect separation
  could not have saved more than a few KB - not worth the Watcom-
  segment-pragma risk for that little gain. Instead:
  - TX/RX descriptor counts are no longer fixed at compile time.
    `rtl_dev_t` sizes its `tx_buf[]`/`rx_buf[]` pointer arrays (and the
    unload free-list in `pktdrv.h`) at a fixed `RTL_MAX_TX_DESC`/
    `RTL_MAX_RX_DESC` (32 each - cheap, just far pointers, 128 bytes per
    array regardless of how many are actually used), but only allocates
    the expensive 1536-byte buffers for however many are actually
    requested via the new `-t`/`-r` command-line options (see
    "Usage" above). `rtl_probe_and_init()`'s signature changed to take
    the desired counts (0 = use the default) as parameters.
  - The defaults (`RTL_DEFAULT_TX_DESC`/`RTL_DEFAULT_RX_DESC`) are 4 and
    12. TX is paced by this driver's own `send_pkt()` calls, not by
    uncontrolled arriving traffic, so a full TX ring only means
    `rtl_send()`'s calibrated wait engages a little more often (bounded,
    never silent data loss) - safe to default shallow with no
    correctness trade-off. RX defaults to 12 (down from a prior 16) as
    a re-test validated after the TX DMA-burst/EarlyTxThres fix made the
    whole TX path more efficient than it was when 8 was measured to
    cause real RX ring overflow under FTP load - confirmed safe under
    real transfers (only 1 packet lost across a multi-file FTP test
    session that also hit the unrelated hang described below multiple
    times). Together, 4+12 at the defaults use ~24KB of buffers instead
    of the previous 48KB.
  - The linker's default DOS stack size is reduced to an explicit 4KB
    (`-k4096` in `Makefile`, `LFLAGS`). Note that this only affects the
    non-resident loading phase (`main()`'s own PCI probing/printing) -
    the resident interrupt handlers (`hw_isr`/`pktdrv_isr`) run on
    whichever program's stack happened to be active when the interrupt
    fired, not on this driver's own stack, since real mode does not
    switch stacks on interrupt entry. The saving comes from reserving
    less stack space in this program's own DGROUP to begin with, which
    is what actually stays resident via `_dos_keep()`.

- **Built-in raw-frame capture (`-c`/`-d`), instead of a second packet-
  driver handle.** Diagnosing intermittent stalls needed a way to
  capture traffic directly on the DOS side, but this driver (like most
  simple packet drivers) only supports one handle at a time, and DOS's
  single-tasking nature means there is no second window to run a
  separate sniffer application from anyway. Solution: capture lives
  inside the driver itself. A fixed-size ring buffer (one raw DOS
  segment, capped at `CAP_MAX_KB`/64KB so every slot is reachable with a
  single un-normalised far pointer) records every TX/RX frame directly
  in `rtl_send()`/`rtl_poll_rx()`; a private extension function
  (`PKTDRV_CAPDUMP_AH`, same magic-value safety mechanism as
  `PKTDRV_UNLOAD_AH`) lets a freshly started `-d` process read it out
  and write a standard `.pcap` file - all without disturbing whatever
  application currently holds the handle. This is precisely what made
  it possible to finally capture a genuine Windows-to-DOS upload stall
  directly - previous attempts capturing on an uninvolved third machine
  came back empty, since a switch never forwards unicast traffic
  between two other devices to an unrelated port.
- **Fixed a 16-bit overflow in the capture ring's slot-address
  arithmetic.** `slot_index * sizeof(cap_entry_t)` is a 16-bit-by-16-bit
  multiplication in this small-model build; for a large capture buffer
  (near `CAP_MAX_KB`), that product can legitimately approach 65536
  (e.g. 484 slots * 135 bytes = 65340) and silently wrap around to a
  small, wrong offset - corrupting or misreading entries near the end
  of the ring. First-run testing showed this exactly: many entries
  decoded with a timestamp of 0 (pointing at never-written memory)
  interleaved with entries showing wildly implausible multi-year
  timestamps (pointing at the wrong slot entirely) once the ring had
  filled close to capacity. Fixed by forcing the multiplication into
  32-bit arithmetic (`(unsigned long)idx * (unsigned long)sizeof(...)`)
  in both `rtl_capture_frame()` (rtl8169p.c) and `do_capture_dump()`
  (main.c) before truncating back to a 16-bit offset - safe to do only
  because the ring itself is capped at 64KB, so the final offset can
  never legitimately need more than 16 bits.
- **A second, separate capture-timestamp problem - the real cause, found
  after the fix below did not help.** Even with the 16-bit-overflow fix
  in place, a fresh capture still showed the same class of symptom:
  entry *content* (TCP sequence numbers, flags, ports - all read from
  `slot->data[]`, a plain byte array) decoded correctly and varied
  sensibly between entries, but the `tick` timestamp field specifically
  was frequently wrong, including the exact same implausible value
  repeated across several consecutive entries.

  First attempt (did NOT fix it): suspected a code-generation quirk with
  misaligned multi-byte far-pointer access (slots are packed at exactly
  `sizeof(cap_entry_t)`, 135 bytes, odd, so successive slots alternate
  between even and odd byte offsets) and changed both the write side
  (`rtl_capture_frame()`) and read side (`do_capture_dump()`) to copy
  the tick value via `_fmemcpy()` instead of a direct far dereference.
  Retested - same corruption, unchanged.

  Real cause: `get_bios_ticks()` called `int86(0x1Ah, ...)` - a BIOS
  interrupt call - from *inside* `rtl_capture_frame()`, itself called
  from `rtl_send()`/`rtl_poll_rx()` while running INSIDE this driver's
  own interrupt handlers (`pktdrv_isr`/`hw_isr`). This project already
  learned, elsewhere, that DOS/BIOS calls made from inside its own
  interrupt handler are unreliable (see the long comment at
  `PKTDRV_UNLOAD_AH` in `pktdrv.h` for the `INT 21h` case that started
  that investigation) - calling `INT 1Ah` from in here turned out to be
  exactly the same category of mistake, and explains why the `_fmemcpy`
  fix could not have helped: the value was already wrong at the point it
  was read, not corrupted afterwards by how it was stored.

  Fixed by reading the tick count directly from the BIOS Data Area
  (`0040:006C`, a plain 32-bit memory location - not a software
  interrupt at all) instead of calling `INT 1Ah`, sidestepping the
  reentrancy problem entirely. Read as a single 32-bit access (this
  project already requires a 386+ target) rather than two 16-bit halves,
  which also avoids a possible torn read if a timer tick lands between
  two separate reads.

- **`CAP_SNAPLEN` raised from 128 bytes to a full frame (1536).** Needed
  to independently verify a TCP segment's checksum against what this
  driver's own hardware actually received, as opposed to what a THIRD-
  PARTY capture on the sending machine shows - which cannot rule out a
  sender-side checksum-offload artifact making an otherwise-valid
  outgoing packet look "corrupt" in that capture (observed in practice:
  a Wireshark capture on a Windows sender with TCP Checksum Offload
  enabled showed several "invalid" checksums that were, on inspection,
  entirely consistent with the well-known offload-capture artifact -
  the NIC computes the real checksum in hardware, after Wireshark has
  already captured the packet, so the capture shows a placeholder
  value). A capture taken with `-c`/`-d` on the DOS side reflects what
  was actually received after the hardware CRC check, sidestepping that
  whole class of ambiguity. Trade-off: fewer entries fit in the same
  `CAP_MAX_KB` budget (roughly 42 at the 64KB maximum, vs. ~485 with the
  old 128-byte snaplen) - still plenty for catching a stall shortly
  after enabling capture.

- **`WBINVD` diagnostic test: ruled out CPU-cache-staleness.** Tested
  the same corrupted-data pattern found via the full-frame capture
  (byte-for-byte comparison of several receptions of what should have
  been byte-identical TCP retransmissions - same source, same sequence
  number, confirmed genuine retransmits via the IP identification-field
  progression - showed a handful of scattered PAYLOAD bytes differing
  between receptions, despite the NIC's own hardware CRC check having
  already passed each time) against a cache-coherency hypothesis:
  `WBINVD` (full CPU cache flush, raw opcode bytes `0F 09h`) immediately
  before reading each received frame's data, to rule out the CPU reading
  a stale, pre-DMA cache line. Real-world testing: **no difference** -
  the corruption persisted. This cleanly rules out CPU cache staleness
  as the cause; the `WBINVD` call has been removed (it was a genuine
  "sledgehammer" - flushing the entire cache on every received frame -
  not something to leave in place once it had done its diagnostic job).
- **Current hypothesis: PCI posted-write ordering, not cache staleness.**
  A different, well-documented class of "DMA looks done but the data
  has not really landed yet" hazard, distinct from CPU caching: the
  packet-data DMA write and the descriptor's own `OWN`-bit write are two
  SEPARATE PCI write transactions, and PCI write posting means a bridge/
  chipset can report a write "done" to the CPU before it has actually
  reached memory, with no inherent guarantee the data write and the
  status write complete in the order they were issued. PCI ordering
  rules do guarantee that a READ COMPLETION from a device cannot
  overtake that same device's own earlier posted writes - so
  `rtl_poll_rx()` (`rtl8169p.c`) now reads back the `IntrStatus` register
  (a convenient, already-mapped register - the value itself is not used)
  immediately before trusting a received frame's data, forcing any
  outstanding posted write from this card to complete first. Far
  cheaper than `WBINVD` (one extra I/O port read per frame vs. a full
  cache flush). Not yet confirmed on real hardware - report back whether
  the corruption pattern persists.

- **CONFIRMED FIX: a small delay between bursty, back-to-back RX
  frames fixes the long-standing data-corruption problem.** Two more
  specific mechanisms were tried and ruled out first: CPU cache
  staleness (`WBINVD`) and PCI posted-write ordering (an `IntrStatus`
  read-back before trusting received data, kept in place - harmless and
  theoretically still correct for the class of hazard it addresses,
  even though it alone did not fix this). Both were tested directly
  against the data-corruption pattern (byte-level comparison of
  repeated retransmissions of the same TCP segment had shown a handful
  of scattered payload bytes differing between receptions, despite the
  NIC's own CRC check passing each time) and neither made a difference.
  This driver has no concept of TCP connections at all (it only ever
  sees raw Ethernet frames), so it cannot detect "early in a connection"
  directly - but it CAN detect and act on the closest underlying
  property: several frames being ready at once within a single
  `rtl_poll_rx()` call, which is what TCP slow-start's early,
  doubling-each-RTT bursts look like at this driver's level, regardless
  of which connection they belong to. `rtl_poll_rx()` inserts a small,
  calibrated 300us gap (`udelay_dos()`) between frames ONLY when more
  than one is ready in the same poll (skipped for a burst's first frame,
  and for ordinary non-bursty single-frame arrivals, so normal traffic
  is unaffected). **Confirmed** via ~20 repeated real-world transfers of
  the specific file that had reliably reproduced the corruption before
  this fix, across varied conditions (alongside larger and smaller
  files, alone, with reboots in between) - zero recurrences. The exact
  mechanism behind why bursty timing specifically triggers this remains
  unconfirmed (neither of the two more specific hypotheses tested
  explains it) - this is a validated, working fix, not a fully
  understood one. 300us was not specifically tuned for minimum
  throughput impact; a smaller value might also work but has not been
  re-validated with the same rigour.

- **RX default further reduced from 12 to 8, after the burst-gap fix.**
  Re-tested once more given how much the TX DMA-burst, posted-write, and
  burst-gap fixes had already changed the RX side's ability to keep up.
  Confirmed via a substantial real-world session (~75MB across 4 files)
  at `-t 4 -r 8`: only 3 packets lost at the driver level (`-s`), and
  critically, **zero** loss visible in mTCP's own reporting - negligible
  either way at that volume. Also confirmed, directly: TX depth matters
  more than initially assumed. A too-shallow TX ring does not lose data
  in this driver directly (`rtl_send()`'s calibrated wait just engages
  more often), but mTCP's own `send_pkt()` retry budget is limited (see
  `Packet.cpp`) - if the ring stays full long enough to exhaust that
  budget too, mTCP itself reports a real, visible send failure. Measured
  directly: `-t 2` showed real loss in mTCP's own reporting; `-t 4` did
  not. New defaults: `RTL_DEFAULT_TX_DESC` = 4 (unchanged),
  `RTL_DEFAULT_RX_DESC` = 8 (down from 12) - together using ~18KB of
  buffers, down from ~24KB. Also observed and worth noting for anyone
  tuning `-r` further: deeper is not simply safer. `rtl_poll_rx()`
  drains the WHOLE ring before returning, so a deeper ring lets it hand
  mTCP a LARGER uninterrupted burst per interrupt - and mTCP's own
  receive-buffer pool is a separate, fixed-size resource from this ring,
  which a big enough burst can exhaust even when this driver's own
  hardware ring never overflows. This is why testing sometimes showed
  MORE loss at a higher `-r` than a lower one - not a measurement error,
  a real trade-off between two different kinds of overflow.

- **Burst-gap default lowered from 300us to 100us.** 300us was the
  value confirmed via ~20 repeated real-world transfers (see above).
  100us was tested afterward and confirmed stable too. Going LOWER than
  100us is the untested, risky direction: 10us was directly confirmed
  NOT enough - the corruption came back at that value.

- **ATTEMPTED AND REVERTED: EtherDFS-style resident-memory-footprint
  scheme.** Tried moving this driver's resident code/data into explicit
  `BEGTEXT`/`RESDATA` segments (via `#pragma code_seg()`/
  `#pragma data_seg()`), with a linker `ORDER` directive (`link.lnk`,
  invoking `wlink` directly instead of through `wcl`) placing them first
  in the memory image, then computing the exact resident size at
  `_dos_keep()` time from the CS-DS segment difference (RESDATA) plus a
  conservative fixed estimate (BEGTEXT) - see the technique description
  this was adapted from at [EtherDFS's client fork by Davide
  Bresolin](https://github.com/davidebreso/etherdfs-client). Also
  required hand-rolling `_fmemcpy()`/`memset()`/`_dos_setvect()`
  replacements for every resident code path, since Watcom's precompiled
  runtime library functions cannot be segment-tagged and there was no
  way to confirm where the linker would place them relative to the
  custom segments without an actual build.

  Got as far as compiling and linking cleanly (after fixing two real
  syntax mistakes along the way: the inline-assembly variable name
  `seg` collided with wlink's `SEG` operator keyword and had to be
  renamed, and the linker directive file needed `#`-style comments and
  `CLNAME`/bare identifiers rather than the `CLASS 'name'` syntax
  first guessed at). But real-hardware testing showed the scheme itself
  was flawed: the driver loaded, but **DHCP stopped working**, `mem /c`
  showed roughly 3x more memory in use than the computed resident size
  claimed (30,912 bytes used vs. an 11,040-byte calculation), and
  unloading no longer freed memory correctly. The likely root cause:
  a custom `#pragma data_seg()` segment/class is not automatically part
  of `DGROUP` (the default data group small-model code assumes DS points
  to at program entry) just by being placed adjacent to it via `ORDER` -
  it has to be explicitly folded INTO `DGROUP`, which this attempt never
  did. That mismatch means DS almost certainly did not point at RESDATA
  at all when the resident interrupt handlers ran, so the CS-DS
  calculation was measuring the wrong thing, and worse, the ISRs
  themselves may have been reading/writing global state through the
  wrong segment.

  Reverted entirely back to the simple, proven `_dos_keep(0, 0x1000)`
  approach. Revisiting this would need a real, verified way to fold a
  custom segment into DGROUP (not just place it nearby in the memory
  map) - worth doing only with the ability to actually build, link, and
  inspect a `.map` file iteratively, which was not available during this
  attempt.

- **`-m`: `IntrMitigate` (interrupt coalescing) support added, then
  promoted to a validated default alongside `-g`.** Investigated after
  the memory-footprint work, given its potential throughput/CPU-
  overhead benefit, especially on weak retro CPUs (fixed per-interrupt
  overhead - entry/exit, this driver's two-stage upcall, PIC acknowledge
  - weighs proportionally far more on something like a 466MHz Celeron
  than on modern hardware).

  Initial concern, since disproven for this driver's actual use: since
  mitigation works by deliberately holding back interrupts so MORE
  frames accumulate before the CPU is notified, it seemed likely to
  make the same bursty-RX corruption that the `-g` burst-gap fix exists
  to reduce WORSE rather than better. First real-world test (`-m 0x5151`
  with `-g` left at its then-default of 100us) confirmed this concern:
  the corruption came back. But a follow-up test - `-m 0x5151` together
  with `-g 0` (burst gap fix OFF) - ran cleanly: 0 loss at both the
  driver level (`-s`) and mTCP's own reporting, across 6 full power-
  cycles/reloads and 20+ repeated transfers of the file that had
  reliably reproduced the original corruption. Interpretation: mitigation
  and the burst-gap delay were fighting each other, not compounding -
  with the gap ALSO active, mitigation's own larger batches were made
  larger still by the added per-frame delay, worsening exactly the
  problem the gap exists to prevent; with the gap off, mitigation alone
  does an equivalent job (fewer, larger interrupts, same underlying
  effect) without that double-counted delay.

  Given that result, `-m 0x5151` and `-g 0` were promoted from
  independent, separately-tuned settings to a validated PAIR: both
  `RTL_DEFAULT_INTR_MITIGATE` (`0x5151`) and `RTL_DEFAULT_BURST_GAP_US`
  (`0`) were updated together, and the load-time warning in `main.c`
  now specifically checks for BOTH being disabled at once (`-g 0 -m 0`,
  the one combination confirmed to reproduce the original bug) rather
  than warning on any non-default `-m` value in isolation.

  `0x5151` was originally found in the older Linux `r8169.c`'s RTL8168
  (not RTL8169) init path; upstream later changed it to `0x5100`
  (RX_USECS/RX_FRAMES zeroed, TX-only coalescing) after a report that
  combining RX coalescing with ASPM (PCIe active-state power management)
  increased packet latency under Linux. That concern is specific to
  Linux's own ASPM driver state machine and has not been observed to
  apply here - DOS has no equivalent active ASPM management - so this
  driver kept the RX-inclusive `0x5151` rather than following upstream
  to `0x5100`; the latter remains available via `-m 0x5100` for anyone
  who wants to compare it directly.

  Register field layout and the per-chip timing table (confirmed
  against the Linux kernel's own `RTL_COALESCE_*` definitions and
  documented coalescing-unit table for the RTL8169 specifically) are in
  the long comment at `RTL_DEFAULT_INTR_MITIGATE` in `rtl8169p.h`.

- **Split into two executables (`RTL8169P.EXE` / `RTLDEBUG.EXE`) to
  shrink the resident footprint of the build most people keep loaded.**
  Adding `-c`/`-d`/`-s` (capture, capture-dump, statistics) had grown
  `main.c` enough that memory use crept back up - but unlike the
  EtherDFS-style segment-reordering attempt above, this needed no
  linker tricks at all: `-s`/`-d` (and their supporting DOS-file-I/O
  helpers, `dos_file_create`/`write`/`close`) are pure post-load
  diagnostics - they only ever call into an ALREADY-RUNNING driver
  instance via the same packet-driver-protocol interrupt calls
  (`PD_FUNC_GET_STATISTICS`, `PKTDRV_CAPDUMP_AH`) regardless of which
  executable originally loaded that instance. Moving them to a second,
  completely independent `.c` file/executable (`rtldebug.c` ->
  `RTLDEBUG.EXE`) needed nothing more than normal C - no `#pragma`,
  no custom linker directive file, no `DGROUP` risk.

  `pci.c`/`rtl8169p.c`/`pktdrv.c` are shared, unmodified, between both
  builds - the actual driver (hardware init, the two interrupt handlers,
  frame capture recording) is identical either way. Only the front-end
  (`main.c` vs `rtldebug.c`) differs: `RTL8169P.EXE` keeps just the
  load-time options (`-t`/`-r`/`-g`/`-m`) plus `-u`; `RTLDEBUG.EXE`
  keeps everything, including `-s`/`-d`.

  `-c` (enable capture at load time) could NOT be moved out the same
  way, since the actual per-frame recording (`rtl_capture_frame()`,
  called from `rtl_send()`/`rtl_poll_rx()` on every single TX/RX event)
  has to run from inside the resident driver regardless of which
  executable loaded it - there is no "diagnostics-only, post-load" way
  to add capture to an instance that was not loaded with it enabled.
  `RTL8169P.EXE` still accepts `-c` and it still works correctly, but
  the option is deliberately left out of that executable's own usage
  text: without `-d` there to read a capture back out, advertising `-c`
  in `RTL8169P.EXE` specifically would offer no real use - load with
  `RTLDEBUG.EXE` instead whenever you want to use `-c`/`-d` together.

  Rough size estimate for what moved out of the resident build: the
  removed functions (`do_stats`, `do_capture_dump`,
  `dos_file_create`/`write`/`close`) came to roughly 150-230 lines of
  C, estimated at somewhere in the 1-1.5KB range of resident 16-bit
  code once compiled - a real but modest saving, not a breakthrough on
  the scale of the earlier buffer-count tuning.


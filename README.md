# RTL8169 DOS Packet Driver

A [Packet Driver Specification](http://crynwr.com/packet_driver.html)
(Crynwr/FTP Software style, INT 60h) driver for the Realtek RTL8169
gigabit Ethernet chip, written for 16-bit real-mode DOS.

Tested on real hardware with [mTCP](http://www.brutman.com/mTCP/) (DHCP
and ping working over 10/100/1000 Mbit auto-negotiated links) and
[EtherDFS](https://mateusz.fr/etherdfs/) (a raw-Ethernet-frame
network redirector, no TCP/IP - large file transfers worked without
issues). See "Known limitations" below before relying on this for
anything serious.

## Features

- PCI bus detection via the PCI BIOS (INT 1Ah).
- Full RTL8169 initialisation: descriptor-ring based TX/RX (bus-master
  DMA, not the older RTL8139 ring-buffer style), interrupt handling,
  IRQ8-15 cascade handling.
- 10/100/1000 Mbit auto-negotiation (no forced speed - see "Design notes"
  for why).
- A minimal, functional subset of the Packet Driver Specification:
  `driver_info`, `access_type`, `release_type`, `send_pkt`, `terminate`,
  `get_address`, `reset_interface`, `get_statistics` - enough for a
  single TCP/IP stack such as mTCP or WatTCP.
- A safe, spec-compliant `-u` uninstall option that unhooks the driver
  and frees all of its memory (including the DMA descriptor rings and
  packet buffers, which live outside the normal resident program image).
- No `<stdio.h>`/`<stdlib.h>` dependency, to keep the resident footprint
  small.

## Requirements

- A Realtek RTL8169 (PCI vendor:device `10ec:8169`) network card.
- [Open Watcom v2](https://github.com/open-watcom/open-watcom-v2), 16-bit
  DOS target (`wcl`, not `wcl386`).
- DOS or a DOS-compatible environment (tested with FreeDOS). See
  "Testing" below for why you need real hardware, not an emulator.

## Building

```
cd src
wmake
```

This produces two executables, both placed in the repository ROOT (not
`src/`, where only the source and intermediate `.obj` files live):
`RTL8169P.EXE` (the stripped-down "production" build - only load-time
options plus `-u`) and `RTLDEBUG.EXE` (the full-featured build, adding
`-s`/`-d`). See "Usage" below and "Design notes" in `DESIGN.md` for why
this split exists. See `src/Makefile` for the exact compiler flags
(`-3 -ms -bt=dos`, i.e. 386 instructions, small memory model, DOS
target). All source (`.c`/`.h`) and the `Makefile` live under `src/`;
this file, `DESIGN.md` and `FILE_ID.DIZ` stay in the repository root, so
they render normally on GitHub's repo landing page.

The build also produces `RTL8169P.ZIP` in the repository root (Windows
only, via PowerShell's `Compress-Archive` - see `src/Makefile`): a
ready-to-distribute archive containing `RTL8169P.EXE` plus this file,
`DESIGN.md`, `FILE_ID.DIZ` and `LICENSE`. `RTLDEBUG.EXE` is deliberately
left out of it - it is the developer/diagnostic build, not part of the
normal distribution.

Open the repository ROOT as the workspace folder in VS Code (so
`README.md`/`DESIGN.md` are visible alongside `src/`, and Claude Code
picks up `.claude/CLAUDE.md` automatically) - a build task is already
set up in `.vscode/tasks.json`: press **Ctrl+Shift+B** to run the
equivalent of the commands above directly, with compiler errors/
warnings parsed into the Problems panel. This requires Open Watcom v2
for Windows on `PATH` (`wcl`/`wmake`) - only the compiler itself, not
DOSBox-X or any DOS environment, since those tools are ordinary native
Windows executables; DOSBox-X (or real hardware) is only needed to
actually *run* the resulting DOS `.EXE`.

## Usage

Load the driver with `RTL8169P.EXE` (smaller resident footprint) or
`RTLDEBUG.EXE` (same load behaviour, plus `-s`/`-d` for later use).
Either executable can then send `-u`/`-s`/`-d` to that running instance
regardless of which one loaded it - the packet-driver protocol calls
underneath are identical, so `RTLDEBUG.EXE -s 0x60` works whether the
driver was loaded via `RTL8169P.EXE` or `RTLDEBUG.EXE` itself.

```
RTL8169P [-t n] [-r n] [-g us] [-m hex] <packet_int_no>
RTL8169P -u <packet_int_no>

RTLDEBUG [-t n] [-r n] [-c kb] [-g us] [-m hex] <packet_int_no>
RTLDEBUG -u <packet_int_no>
RTLDEBUG -s <packet_int_no>
RTLDEBUG -d <file> <packet_int_no>
```

- `packet_int_no` - the software interrupt to use, as a hexadecimal value
  in the range `0x60`-`0x80` (the range reserved by the Packet Driver
  Specification). **Required** - the driver refuses to guess a default
  and will print usage instead.
- `-u` - uninstall the driver from the given interrupt and free its
  memory. Available in both executables.
- `-t n` / `-r n` - number of TX/RX descriptors (and matching packet
  buffers) to allocate, from 1 up to `RTL_MAX_TX_DESC`/`RTL_MAX_RX_DESC`
  (32 each by default in `rtl8169p.h`). Each one costs `RTL_BUF_SIZE`
  (1536) bytes of resident memory - the single biggest lever over this
  driver's memory footprint. Defaults (`RTL_DEFAULT_TX_DESC`/
  `RTL_DEFAULT_RX_DESC`, 4/8) were chosen and validated as described
  below - notably, TX depth matters more than it first appears to: a
  too-shallow TX ring does not lose data in THIS driver directly, but
  can exhaust mTCP's own limited `send_pkt()` retry budget, causing a
  real, mTCP-visible send failure (confirmed: `-t 2` showed real loss
  in mTCP's own reporting, `-t 4` did not); going lower on `-r` risks
  dropped packets under real load, though a DEEPER `-r` is not simply
  safer either (see the long comment at `RTL_DEFAULT_RX_DESC` in
  `rtl8169p.h`) - always check with `-s` after a representative test
  before trusting a value other than the confirmed default in
  production use.
- `-s` - print packet/byte/error counters (`get_statistics()`) from a
  running driver instance. **`RTLDEBUG.EXE` only** - see "Design notes"
  in `DESIGN.md` for why this was moved out of `RTL8169P.EXE`. Works
  against a driver instance loaded by either executable. Useful for
  confirming whether packet loss under real traffic is coming from RX
  ring overflow - see "Known limitations" below. Works even while
  another application (e.g. mTCP) currently holds the driver's handle -
  this deliberately does not follow the spec's strict per-handle access
  check, since DOS is single-tasking and there is no other way to
  inspect counters while a transfer is stuck. See the comment above the
  `get_statistics()` case in `pktdrv.c` for the reasoning.
- `-c kb` - enable a raw-frame capture ring buffer of the given size in
  KB (1-`CAP_MAX_KB`, 64 by default in `rtl8169p.h`; off unless given, so
  it costs no memory otherwise). Available in both executables (the
  actual recording code must stay resident either way - see
  `rtl_capture_frame()` in `rtl8169p.c`), but only listed in
  `RTLDEBUG.EXE`'s own usage text, since only that executable also has
  `-d` to read a capture back out - `RTL8169P.EXE` accepts `-c` too if
  you type it, but offers no way to retrieve the result, so advertising
  it there would be pointless. Every TX and RX frame is recorded
  (timestamp, direction, up to `CAP_SNAPLEN` bytes) into a fixed-size
  ring that silently overwrites its oldest entries once full - handy for
  catching what was happening right before a stall, without knowing in
  advance exactly when to start capturing.
- `-d file` - write a running driver's capture buffer (see `-c`) out as
  a standard `.pcap` file, directly readable in Wireshark. **`RTLDEBUG.
  EXE` only.** Like `-s`, this works even while another application
  still holds the driver's handle - dump it right after a transfer
  stalls, no need to unload or disturb anything. Timestamps use the
  BIOS clock tick (~55ms resolution) - coarse, but enough to see
  multi-second stalls; only relative timing between packets is
  meaningful, the absolute date/time in the resulting file is not real.
- `-g us` - delay, in microseconds, inserted between bursty, back-to-
  back RX frames (0-`RTL_MAX_BURST_GAP_US`, default
  `RTL_DEFAULT_BURST_GAP_US` = 0 in `rtl8169p.h`). Together with `-m`
  below, this is the confirmed fix for a data-corruption bug described
  in `DESIGN.md`. The default of 0 (this delay effectively off) relies
  on `-m`'s own default (0x5151) to do the equivalent job via hardware
  interrupt coalescing instead - the two are a validated PAIR, not
  independent settings. Disabling BOTH (`-g 0 -m 0`) reintroduces the
  original bug; the driver prints a warning at load time if you do.
  Earlier, standalone-delay values (300us, then 100us) were also
  confirmed stable on their own, before `-m` existed - see the long
  comment at `RTL_DEFAULT_BURST_GAP_US` in `rtl8169p.h` for the full
  value history if you want to use `-g` without `-m`.
- `-m hex` - raw 16-bit value written directly to the RTL8169's
  `IntrMitigate` register (interrupt coalescing/moderation), e.g.
  `-m 0x5151`. Default `RTL_DEFAULT_INTR_MITIGATE` = `0x5151` in
  `rtl8169p.h`, paired with `-g` defaulting to 0 (see above) - confirmed
  stable via 20+ repeated real-world transfers of a file that had
  reliably reproduced the original corruption, across 6 full power-
  cycles/reloads, with zero loss at both the driver level (`-s`) and
  mTCP's own reporting. Especially beneficial on weak retro CPUs (e.g. a
  466MHz Celeron): every interrupt has a largely CPU-speed-independent
  fixed overhead, which weighs proportionally far more there than on
  modern hardware, so fewer, larger interrupts is a genuine efficiency
  win. Two other known values worth knowing about, decoded as
  TX_USECS/TX_FRAMES/RX_USECS/RX_FRAMES:
  - `0x5151` (5/1/5/1, this driver's default) - originally found in the
    older Linux `r8169.c`'s RTL8168 (not RTL8169) init path.
  - `0x5100` (5/1/0/0, RX coalescing disabled, TX-only) - what upstream
    Linux later changed `0x5151` to, after a report that combining RX
    coalescing with ASPM (PCIe active-state power management) increased
    packet latency. That specific concern is Linux/ASPM-driver-state
    specific and has not been observed to apply here - DOS has no
    equivalent active ASPM management - which is why this driver keeps
    the RX-inclusive `0x5151` as its default rather than following
    upstream to `0x5100`; the latter remains available to try via `-m`
    if you want to compare.
  See the long comment at `RTL_DEFAULT_INTR_MITIGATE` in `rtl8169p.h`
  for the register's full field layout and the per-speed timing table.

Example:

```
RTL8169P 0x60
...
RTLDEBUG -s 0x60

...
RTL8169P -u 0x60
```

Running the driver with no arguments, or with an argument that is not a
valid interrupt number in the required range, prints usage information
and exits without touching any hardware.

## Testing

**No common PC emulator emulates the RTL8169.** QEMU, VirtualBox, VMware
and 86Box all emulate NE2000- and/or RTL8139-family cards, but none of
them emulate the RTL8169's descriptor-ring/bus-master DMA interface. This
driver can only be tested on real hardware. A second machine on the same
network segment (running Wireshark, or just `ping`) is very useful for
diagnosing TX/RX issues independently of the DOS side.

For isolating whether a problem is in this driver's raw TX/RX path
versus in a TCP/IP stack layered on top, testing with
[EtherDFS](https://mateusz.fr/etherdfs/) (raw Ethernet frames, no
TCP/IP at all) alongside mTCP is a useful cross-check - if EtherDFS
transfers large files cleanly while mTCP shows a problem, that points
away from this driver's own send/receive mechanics.

## Known limitations

- **Single handle/interface only** - enough for one TCP/IP stack
  (mTCP/WatTCP), not several packet-driver clients at once.
- **Link changes are not monitored after startup** - checked once, at
  load time.
- **No jumbo frames, VLAN, or checksum offload.**
- **No per-chip-revision workarounds** - one generic init sequence,
  tested against one specific card (RTL8169, not RTL8168/RTL8110).
- **Gigabit throughput is architecturally limited** - no interrupt
  coalescing; do not expect to saturate a gigabit link from DOS with
  this driver.
- **EEPROM MAC address is not bit-banged** - trusts the MAC0-5 I/O
  registers the card's firmware/BIOS loads on power-up.
- **Resident memory size is requested as a fixed 64KB ceiling**, not
  computed from the actual program size - this does not mean 64KB is
  actually kept resident (see `DESIGN.md`).

See [`DESIGN.md`](DESIGN.md) for the full reasoning behind each of these,
and the design/debugging history behind this driver's implementation
choices.

## File overview

| File               | Contents |
|--------------------|----------|
| `src/main.c`       | Command-line front end for `RTL8169P.EXE` (stripped down) |
| `src/rtldebug.c`   | Command-line front end for `RTLDEBUG.EXE` (full-featured) |
| `src/pci.c/.h`     | PCI BIOS (INT 1Ah) detection and configuration access |
| `src/rtl8169p.c/.h`| Chip register map, init, TX/RX descriptor rings, PHY/MDIO |
| `src/pktdrv.c/.h`  | INT 60h dispatcher, hardware IRQ ISR, RX upcall to the application |
| `src/Makefile`     | Open Watcom build file (builds `../RTL8169P.EXE`, `../RTLDEBUG.EXE`, `../RTL8169P.ZIP`) |
| `RTL8169P.EXE`     | Build output (see `src/Makefile`) |
| `RTLDEBUG.EXE`     | Build output (see `src/Makefile`) |
| `.claude/CLAUDE.md`| Developer-facing project context, auto-loaded by Claude Code |
| `.vscode/`         | VS Code build task (Ctrl+Shift+B) - see "Building" above |
| `.gitignore`       | Excludes `*.obj` (intermediate build files) |
| `DESIGN.md`        | Detailed design notes and debugging/investigation history |
| `FILE_ID.DIZ`      | Short BBS/archive-style description of the project |
| `LICENSE`          | GNU General Public License v3.0 (full text) |

## References

- [Linux `drivers/net/ethernet/realtek/r8169_main.c`](https://github.com/torvalds/linux/blob/master/drivers/net/ethernet/realtek/r8169_main.c) - register map, init sequence, descriptor bit layout.
- [U-Boot `drivers/net/rtl8169.c`](https://github.com/u-boot/u-boot/blob/master/drivers/net/rtl8169.c) - a simpler, bare-metal-style reference implementation.
- [OSDev.org RTL8169 wiki page](https://wiki.osdev.org/RTL8169).
- Realtek RTL8110S/RTL8169S datasheet, v1.3.
- [QEMU's `hw/net/rtl8139.c`](https://git.zx2c4.com/qemu/tree/hw/net/rtl8139.c) (also used by 86Box's RTL8139C+ emulation) - useful for cross-checking the C+-mode descriptor bit layout, which the RTL8169 shares in spirit.
- [Packet Driver Specification](http://crynwr.com/packet_driver.html) (FTP Software / Russ Nelson / Crynwr, v1.11) - the INT 60h API this driver implements.

## License

Copyright (C) 2026 Yoghoo

GNU General Public License v3.0 or later. See [LICENSE](LICENSE).

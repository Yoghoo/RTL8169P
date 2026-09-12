# RTL8169 DOS Packet Driver

A [Packet Driver Specification](http://crynwr.com/packet_driver.html)
(Crynwr/FTP Software style, INT 60h) driver for the Realtek RTL8169
gigabit Ethernet chip, written for 16-bit real-mode DOS in Open Watcom C.
Tested on real retro hardware with mTCP and EtherDFS.

**Before making changes, read `../DESIGN.md`.** It is the full design and
debugging history for this project - dozens of hard-won findings (DMA
timing, memory leaks, data corruption tied to bursty interrupts, a
reverted memory-optimisation attempt, and more), each with the reasoning
and the real-world testing that confirmed or refuted it. Assume any
"obvious" improvement has already been tried and either adopted or
rejected for a documented reason - check there first.

## Build

```
cd src
wmake
```

Requires Open Watcom v2 with `wcl` (NOT `wcl386` - this is a 16-bit real
mode target, no DOS/4GW extender). Produces `RTL8169P.EXE` and
`RTLDEBUG.EXE` in the repository root (not `src/` - only source and
intermediate `.obj` files stay there), plus (Windows only, via
PowerShell `Compress-Archive` in `src/Makefile`) `RTL8169P.ZIP`,
also in the root, containing `RTL8169P.EXE` plus the four root-level
docs (README/DESIGN/FILE_ID.DIZ/LICENSE). From the workspace root in
VS Code, `Ctrl+Shift+B` runs the equivalent build task
(`.vscode/tasks.json`). `.gitignore` excludes `*.obj` only - the built
`.EXE`/`.ZIP` files are not excluded.

- **`RTL8169P.EXE`** (`src/main.c`) - stripped-down "production" build.
  Load-time options only (`-t`/`-r`/`-g`/`-m`) plus `-u`.
- **`RTLDEBUG.EXE`** (`src/rtldebug.c`) - full-featured build, adds
  `-s`/`-d` (diagnostics against an already-loaded instance). Either
  executable can send `-u`/`-s`/`-d` to a running driver regardless of
  which one loaded it - same underlying protocol.

`src/pci.c`/`src/rtl8169p.c`/`src/pktdrv.c` are shared, unmodified,
between both builds.

## Critical constraints

- **No emulator can test this.** QEMU/VirtualBox/VMware/86Box do not
  emulate the RTL8169's descriptor-ring/bus-master DMA interface. Any
  change touching hardware I/O, DMA, or interrupt handling needs real
  hardware to validate - say so explicitly rather than assuming a change
  is safe because it compiles.
- **DOS/BIOS calls from inside the resident interrupt handlers
  (`hw_isr`/`pktdrv_isr` in `pktdrv.c`, and anything they call) are
  unreliable or outright wrong.** This has broken things multiple times
  (see `../DESIGN.md`). Prefer direct hardware I/O or raw memory reads
  there instead.
- **A real, confirmed data-corruption bug exists** under bursty RX
  traffic, fixed by a specific combination of settings
  (`RTL_DEFAULT_BURST_GAP_US` / `RTL_DEFAULT_INTR_MITIGATE` in
  `src/rtl8169p.h`). Do not change either default without the same
  real-world, repeated-transfer testing rigour described in `../DESIGN.md`.
- Comment blocks in this codebase are long and explain *why*, not just
  *what* - many exist specifically because a shorter version once led
  to a bug being reintroduced. Preserve that reasoning when editing
  nearby code, don't trim it for brevity.

## Other docs

- `../README.md` - user-facing: features, usage, options, known limitations.
- `../FILE_ID.DIZ` - short BBS/archive-style project blurb.
- `../LICENSE` - GNU General Public License v3.0.

# Makefile for Open Watcom C/C++ (wcl), DOS real-mode target.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# Part of the RTL8169P DOS Packet Driver project.
# Copyright (C) 2026 Yoghoo
#
# Requires: Open Watcom v2 (open-watcom-v2 on GitHub, or the classic
# OpenWatcom 1.9 install) with wcl (NOT wcl386 - this builds 16-bit
# REAL MODE DOS .EXEs, no DOS/4GW extender involved), using -bt=dos.
#
# -3      : allow 386 instructions (needed for the 32-bit I/O ports and
#           the 32-bit PCI configuration register read in pci.c)
# -ms     : small memory model (enough for this TSR; buffers are far)
# -bt=dos : target = DOS
# -zq     : quiet
# -k4096  : explicit 4KB stack (LFLAGS only - see below for why)
#
# Builds TWO executables from the same underlying driver code
# (pci.c/rtl8169p.c/pktdrv.c, unchanged either way):
#   RTL8169P.EXE - stripped-down "production" build (main.c). Only
#                  load-time options (-t/-r/-c/-g/-m) plus -u.
#   RTLDEBUG.EXE  - full-featured build (rtldebug.c), adds -s/-d
#                  (diagnostics against an already-loaded instance).
# See the file header comments in main.c/rtldebug.c, and "Design notes"
# in ../DESIGN.md, for why this split exists (smaller resident footprint
# for the build most people keep loaded) and why either executable can
# talk to a driver instance regardless of which one loaded it.
#
# Also produces RTL8169P.ZIP - a ready-to-distribute archive of
# RTL8169P.EXE plus the four root-level docs (README.md/DESIGN.md/
# FILE_ID.DIZ/LICENSE), built via PowerShell's Compress-Archive. Windows-
# only
# (uses "powershell" directly) - if you build on a different host,
# either skip the $(ZIP_TARGET) prerequisite below or replace that one
# recipe line with your platform's own zip tool. RTLDEBUG.EXE is
# deliberately left OUT of this archive - it is the developer/diagnostic
# build, not part of the normal distribution.
#
# Build everything (both executables + the zip):
#   wmake
# Clean:
#   wmake clean

CC      = wcl
CFLAGS  = -3 -ms -bt=dos -zq -oe
# -k4096: Watcom's default DOS stack size is more generous than this TSR
# actually needs - the interrupt handlers (pktdrv_isr/hw_isr and what they
# call) only ever use small local variables/structs, no large arrays, no
# deep recursion. 4KB leaves comfortable margin over the deepest observed
# call chain while trimming a few KB off the resident footprint (see the
# memory notes in README.md). If you add code with larger stack frames
# (e.g. sizeable local arrays), re-check this is still enough BEFORE
# testing on real hardware - a stack overflow here is silent memory
# corruption, not a clean crash.
LFLAGS  = -bt=dos -k4096

DRIVER_TARGET = RTL8169P.EXE
DEBUG_TARGET  = RTLDEBUG.EXE
ZIP_TARGET    = RTL8169P.ZIP

SHARED_OBJS = pci.obj rtl8169p.obj pktdrv.obj

all: $(DRIVER_TARGET) $(DEBUG_TARGET) $(ZIP_TARGET) .SYMBOLIC

$(DRIVER_TARGET): main.obj $(SHARED_OBJS)
	$(CC) $(LFLAGS) -fe=$(DRIVER_TARGET) main.obj $(SHARED_OBJS)

$(DEBUG_TARGET): rtldebug.obj $(SHARED_OBJS)
	$(CC) $(LFLAGS) -fe=$(DEBUG_TARGET) rtldebug.obj $(SHARED_OBJS)

# Repacks every time "wmake"/"wmake all" runs (depends on $(DRIVER_TARGET),
# which wmake only rebuilds when something actually changed) - harmless,
# since Compress-Archive with -Force just overwrites the same output.
$(ZIP_TARGET): $(DRIVER_TARGET)
	powershell -NoProfile -Command "Compress-Archive -LiteralPath 'RTL8169P.EXE','..\README.md','..\DESIGN.md','..\FILE_ID.DIZ','..\LICENSE' -DestinationPath 'RTL8169P.ZIP' -Force"

main.obj: main.c rtl8169p.h pktdrv.h
	$(CC) $(CFLAGS) -c main.c

rtldebug.obj: rtldebug.c rtl8169p.h pktdrv.h
	$(CC) $(CFLAGS) -c rtldebug.c

pci.obj: pci.c pci.h
	$(CC) $(CFLAGS) -c pci.c

rtl8169p.obj: rtl8169p.c rtl8169p.h pci.h
	$(CC) $(CFLAGS) -c rtl8169p.c

pktdrv.obj: pktdrv.c pktdrv.h rtl8169p.h
	$(CC) $(CFLAGS) -c pktdrv.c

clean: .SYMBOLIC
	del *.obj
	del $(DRIVER_TARGET)
	del $(DEBUG_TARGET)
	del $(ZIP_TARGET)

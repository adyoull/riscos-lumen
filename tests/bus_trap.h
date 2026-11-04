/* Copyright (c) 2026 Andrew Youll
   Released under the MIT licence: see the LICENSE file.
   SPDX-License-Identifier: MIT */
/* bus_trap.h: on ARM Linux (the QEMU alignment-trap tests), a SIGBUS
   handler that says where the misaligned access was before stopping, since
   there's no debugger in the container. Look the address up with
   arm-linux-gnueabihf-addr2line -f -e PROGRAM ADDRESS. Elsewhere: nothing. */
#ifndef LUMEN_BUS_TRAP_H
#define LUMEN_BUS_TRAP_H
#if defined(__arm__) && defined(__linux__)
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <ucontext.h>
static void bus_trap_handler (int sig, siginfo_t *si, void *ctx)
{
  ucontext_t *uc = ctx;
  fprintf (stderr, "SIGBUS: misaligned access to %p at pc %08lx (lr %08lx)\n", si->si_addr,
           (unsigned long) uc->uc_mcontext.arm_pc, (unsigned long) uc->uc_mcontext.arm_lr);
  (void) sig;
  _Exit (135);
}
static void bus_trap_install (void)
{
  struct sigaction sa;
  sa.sa_sigaction = bus_trap_handler;
  sigemptyset (&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  sigaction (SIGBUS, &sa, NULL);
}
#else
static void bus_trap_install (void) {}
#endif
#endif

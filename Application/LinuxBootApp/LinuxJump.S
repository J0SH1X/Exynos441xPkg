/** @file
  Leave UEFI for an ARM Linux zImage.

  VOID LinuxJump (UINT32 Entry, UINT32 MachType, UINT32 Atags);

  Called after ExitBootServices with everything in place.  Uses no stack
  from the point the D-cache is cleaned: once caching is off, dirty lines
  that never reached DRAM would be lost, and the C stack is one of them.

    1. IRQ/FIQ off.
    2. Clean + invalidate the L1 D-cache by set/way (ArmLib in this edk2 only
       has by-MVA maintenance).  The PL310 L2 is handled by the caller.
    3. MMU, D-cache and alignment checking off; I-cache may stay on.
    4. Invalidate I-cache, branch predictor and TLBs.
    5. r0 = 0, r1 = machine type, r2 = ATAG list; jump in ARM state.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <AsmMacroLib.h>

  .arm

ASM_FUNC(LinuxJump)
  mov     r12, r0                 // entry
  mov     r6, r1                  // machine type
  mov     r8, r2                  // ATAG list

  cpsid   if

  //
  // L1 clean + invalidate by set/way (canonical ARMv7 walk).
  //
  dsb
  mrc     p15, 1, r0, c0, c0, 1   // CLIDR
  ands    r3, r0, #0x7000000      // LoC
  mov     r3, r3, lsr #23
  beq     L_FlushDone
  mov     r10, #0                 // cache level << 1
L_Level:
  add     r2, r10, r10, lsr #1    // 3 * level
  mov     r1, r0, lsr r2
  and     r1, r1, #7              // cache type at this level
  cmp     r1, #2
  blt     L_NextLevel             // no data cache
  mcr     p15, 2, r10, c0, c0, 0  // CSSELR
  isb
  mrc     p15, 1, r1, c0, c0, 0   // CCSIDR
  and     r2, r1, #7
  add     r2, r2, #4              // log2 line length
  ldr     r4, =0x3ff
  ands    r4, r4, r1, lsr #3      // max way
  clz     r5, r4                  // way shift
  ldr     r7, =0x7fff
  ands    r7, r7, r1, lsr #13     // max set
L_Set:
  mov     r9, r4
L_Way:
  orr     r11, r10, r9, lsl r5
  orr     r11, r11, r7, lsl r2
  mcr     p15, 0, r11, c7, c14, 2 // DCCISW
  subs    r9, r9, #1
  bge     L_Way
  subs    r7, r7, #1
  bge     L_Set
L_NextLevel:
  add     r10, r10, #2
  cmp     r3, r10
  bgt     L_Level
L_FlushDone:
  mov     r10, #0
  mcr     p15, 2, r10, c0, c0, 0
  dsb
  isb

  //
  // MMU, D-cache, alignment checks off.
  //
  mrc     p15, 0, r0, c1, c0, 0
  bic     r0, r0, #(1 << 0)       // M
  bic     r0, r0, #(1 << 1)       // A
  bic     r0, r0, #(1 << 2)       // C
  mcr     p15, 0, r0, c1, c0, 0
  isb

  mov     r0, #0
  mcr     p15, 0, r0, c7, c5, 0   // ICIALLU
  mcr     p15, 0, r0, c7, c5, 6   // BPIALL
  mcr     p15, 0, r0, c8, c7, 0   // TLBIALL
  dsb
  isb

  mov     r0, #0
  mov     r1, r6
  mov     r2, r8
  bx      r12

  .ltorg

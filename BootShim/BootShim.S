/*
 * BootShim for Samsung Exynos441x (Galaxy Note 1 GT-N7000 first).
 *
 * The Samsung bootloader loads the KERNEL partition as a raw zImage-style
 * image and jumps to its start.  This shim:
 *
 *   - carries a valid zImage magic (0x016f2818 @ 0x24) so the BL is happy,
 *   - puts the CPU into a known state (SVC, IRQ/FIQ off, MMU/caches off,
 *     VFP off, watchdog stopped),
 *   - relocates the attached EDK2 firmware device (FD) to UEFI_BASE
 *     (position independent: works no matter where the BL loaded us),
 *   - jumps to the FD entry stub that GenFv placed at FD offset 0.
 *
 * Modelled on HtcLeoPkg's BootShim (which itself derives from
 * SurfaceDuoPkg/BootShim and the htcleo kernel's arch-init code).
 */

#define DSB .byte 0x4f, 0xf0, 0x7f, 0xf5
#define ISB .byte 0x6f, 0xf0, 0x7f, 0xf5

.text
.globl _start
_start:
	b	_Reset

	/* ARM zImage header: magic number at offset 0x24 */
.org 0x24
	.word 0x016f2818
	.word 0x00000000          /* zImage "start" / end-of-setup words  */
	.word 0x00000000
	.word 0x00000000
	.word 0x00000000
	.word 0x00000000
	.word 0x00000000          /* zImage ends at 0x38..0x3C           */

.org 0x40

_Reset:
	/* Enter SVC mode, IRQ + FIQ masked */
	mov	r0, #0xD3		/* CPSR_MODE_SVC | I | F */
	msr	CPSR_cxsf, r0

	/*
	 * Force physical, uncached accesses before touching the display:
	 * turn MMU + D-cache off and flush the TLB.  (If the BL ran with
	 * caches on, dirty lines are abandoned on purpose — it never
	 * comes back here.)
	 */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)	/* M - MMU off */
	bic	r0, r0, #(1 << 2)	/* C - D-cache off */
	mcr	p15, 0, r0, c1, c0, 0
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0	/* TLBIALL */
	dsb
	isb

_Relocate:
	/* r0 = runtime address of the payload, r1 = destination */
	adr	r0, _Payload
	ldr	r1, =UEFI_BASE
	cmp	r0, r1
	beq	_Entry
	ldr	r2, =UEFI_SIZE

_CopyLoop:
	ldr	r3, [r0], #4
	str	r3, [r1], #4
	subs	r2, r2, #4
	bgt	_CopyLoop

_Entry:
	ldr	r5, =UEFI_BASE
	bx	r5

.ltorg

	.align 5
_Payload:

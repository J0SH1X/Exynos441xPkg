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

	/*
	 * PROOF OF EXECUTION: paint whatever framebuffer THE BOOTLOADER
	 * programmed.  We cannot know its address from the host (the
	 * recovery kernel's 0x70C6C000 is a different buffer), so read
	 * it out of the FIMD0 window registers as the BL left them:
	 *
	 *   WINCON<n>     0x20 + 4n    bit 0 = window enabled
	 *   VIDW<n>_ADD0  0xA0 + 16n   buffer start address
	 *   VIDW<n>_ADD1  0xA4 + 16n   buffer end address (inclusive)
	 *
	 * Solid magenta, windows 0..4, only ranges inside DRAM.
	 */
	ldr	r6, =0x11C00000		/* FIMD0 base */
	mov	r7, #0			/* window index 0..4 */
_ClearFbWindow:
	add	r8, r6, #0x20
	add	r8, r8, r7, lsl #2	/* WINCON<n> = 0x20 + 4n */
	ldr	r2, [r8]		/* WINCON<n> */
	tst	r2, #1			/* ENWIN? */
	beq	_ClearFbNext
	/* start addr: vidw00add0b0 + 8n (u-boot struct exynos_fb) */
	add	r3, r6, #0xA0
	add	r3, r3, r7, lsl #3
	ldr	r0, [r3]		/* start */
	/* end addr: vidw00add1b0 + 8n (u-boot struct exynos_fb) */
	add	r3, r6, #0xD0
	add	r3, r3, r7, lsl #3
	ldr	r1, [r3]		/* end (inclusive) */
	add	r1, r1, #4
	/* Sanity: start must point into DRAM (0x40000000..0x80000000) */
	ldr	r4, =0x40000000
	cmp	r0, r4
	blo	_ClearFbNext
	cmp	r1, r0
	bls	_ClearFbNext		/* end <= start: nonsense, skip */
	ldr	r4, =0x80000000
	cmp	r0, r4
	bhs	_ClearFbNext
	ldr	r2, =0x00FF00FF		/* magenta */
1:	str	r2, [r0], #4
	cmp	r0, r1
	blo	1b
_ClearFbNext:
	add	r7, r7, #1
	cmp	r7, #5
	blo	_ClearFbWindow

	/*
	 * Stop the Samsung bootloader's software watchdog so slow phases
	 * (DEBUG builds!) cannot trigger a reset.  Exynos4 watchdog @
	 * 0x10060000: WTCON bit 5 = 0 disables the watchdog.
	 */
	ldr	r1, =0x10060000
	ldr	r0, [r1]
	bic	r0, r0, #(1 << 5)
	str	r0, [r1]

	/* Read SCTLR */
	mrc	p15, 0, r0, c1, c0, 0

	/* If the D-cache was left enabled by the BL, clean+invalidate it
	 * first (canonical ARMv7 set/way walk, no stack needed). */
	tst	r0, #(1 << 2)
	beq	_CacheSkip

	dsb
	mrc	p15, 1, r0, c0, c0, 1		/* CLIDR */
	ands	r3, r0, #0x7000000		/* LoC */
	mov	r3, r3, lsr #23
	beq	_FlushDone
	mov	r10, #0			/* cache level 0 */
_FlushLoop1:
	add	r2, r10, r10, lsr #1		/* 3 * level */
	mov	r1, r0, lsr r2
	and	r1, r1, #7
	cmp	r1, #2
	blt	_FlushSkip			/* no D-cache here */
	mcr	p15, 2, r10, c0, c0, 0		/* CSSELR = level */
	isb
	mrc	p15, 1, r1, c0, c0, 0		/* CCSIDR */
	and	r2, r1, #7			/* line length */
	add	r2, r2, #4
	ldr	r4, =0x3ff
	ands	r4, r4, r1, lsr #3		/* max way */
	clz	r5, r4				/* way shift */
	ldr	r7, =0x7fff
	ands	r7, r7, r1, lsr #13		/* max set */
_FlushLoop2:
	mov	r9, r4
_FlushLoop3:
	orr	r11, r10, r9, lsl r5
	orr	r11, r11, r7, lsl r2
	mcr	p15, 0, r11, c7, c14, 2		/* clean+invalidate set/way */
	subs	r9, r9, #1
	bge	_FlushLoop3
	subs	r7, r7, #1
	bge	_FlushLoop2
_FlushSkip:
	add	r10, r10, #2
	cmp	r3, r10
	bgt	_FlushLoop1
	mov	r10, #0
	mcr	p15, 2, r10, c0, c0, 0
	dsb
_FlushDone:

_CacheSkip:
	/* Re-read SCTLR (registers were clobbered by the flush) and set up
	 * a flat, unaligned-tolerant environment: MMU off, D-cache off,
	 * low vectors, branch prediction + I-cache on. */
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #(1 << 0)		/* M   - MMU off           */
	bic	r0, r0, #(1 << 2)		/* C   - D-cache off       */
	bic	r0, r0, #(1 << 1)		/* A   - alignment off     */
	bic	r0, r0, #(1 << 13)		/* V   - low vectors       */
	bic	r0, r0, #(1 << 28)		/* TRE                     */
	bic	r0, r0, #(1 << 29)		/* AFE                     */
	orr	r0, r0, #(1 << 12)		/* I   - I-cache on        */
	orr	r0, r0, #(1 << 11)		/* Z   - branch prediction */
	mcr	p15, 0, r0, c1, c0, 0
	isb

	/* Invalidate TLBs, I-cache and BTB */
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0		/* TLBIALL   */
	mcr	p15, 0, r0, c7, c5, 6		/* BPIALL    */
	mcr	p15, 0, r0, c7, c5, 0		/* ICIALLU   */
	dsb
	isb

	/* NOTE: the VFP is deliberately left in whatever state the BL had it.
	 * PrePi enables CP10/CP11 + FPEXC itself; touching FPEXC here could
	 * trap to UNDEF if the BL had the coprocessors disabled in CPACR. */

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

/*
 * Reset entry: replaces the weak handle_reset of the SDK startup_ch32v20x_D6.S with identical content,
 * except that oring_reset_hook() is called after setting gp/sp and before copying .data and zeroing .bss.
 *
 * Reason: it takes about 2-3ms from reset to main() (zeroing about 16KB of .bss at 8MHz HSI, SystemInit setting up the PLL),
 * during which the EN pin floats and LTC4359 SHDN is held only by C30 (100nF) through R12 (50k); it may drop below threshold and shut off,
 * and turning back on takes about 3ms more to charge the gate at 10uA. Measured 2026-10-25: warm reset at 1A load, VOUT 47.5V -> 31V (v0.0.88).
 * oring_reset_hook must not use .data/.bss (not yet initialized); it only reads registers, constants and the reset record at a fixed address.
 */
	.section	.text.handle_reset,"ax",@progbits
	.globl	handle_reset
	.align	1
handle_reset:
.option push
.option	norelax
	la gp, __global_pointer$
.option	pop
	la sp, _eusrstack
	call oring_reset_hook
/* Load data section from flash to RAM */
	la a0, _data_lma
	la a1, _data_vma
	la a2, _edata
	bgeu a1, a2, 2f
1:
	lw t0, (a0)
	sw t0, (a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
/* Clear bss section */
	la a0, _sbss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	sw zero, (a0)
	addi a0, a0, 4
	bltu a0, a1, 1b
2:
/* Configure pipelining and instruction prediction */
	li t0, 0x1f
	csrw 0xbc0, t0
/* Enable nested and hardware stack */
	li t0, 0x3
	csrw 0x804, t0
/* Enable interrupt */
	li t0, 0x88
	csrw mstatus, t0
/* Configure the interrupt vector table recognition mode and entry address mode
 * The SDK _vector_base is a local label inside the startup file; .vector directly follows .init, so the linker script symbol _einit is its address (0x4) */
	la t0, _einit
	ori t0, t0, 3
	csrw mtvec, t0

	jal  SystemInit
	la t0, main
	csrw mepc, t0
	mret

/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2024, Oracle and/or its affiliates.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <grub/symbol.h>
#include <grub/slaunch.h>
#include <grub/i386/crfr.h>
#include <grub/i386/msr.h>

#define GRUB_SMX_LEAF_SENTER	4
#define CS_SEL32		0x0008
#define DS_SEL			0x0010

	.file	"dltrampoline.S"
	.text

	.code64
	.globl	dl_entry_trampoline
dl_entry_trampoline:
	/* %edi should contain dl_context pointer */
	call	EXT_C(dl_entry)
	ud2

	.globl	dl_trampoline
dl_trampoline:
	cli
	leaq	dl_gdt_base(%rip), %rax
	leaq	dl_gdt(%rip), %rbx
	movl	%ebx, (%eax)
	lgdt	dl_gdtr(%rip)

	/* Setup target to ret to compat mode */
	leal	1f(%rip), %ecx
	pushq	$CS_SEL32
	pushq	%rcx
	lretq

	.code32
1:	/* Now in long compatibility (IA-32e), mode load data segments */
	movw	$DS_SEL, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	cmpl	$SLP_AMD_SKINIT, %edx
	je	1f

	/* Intel SENTER */
	movl	$GRUB_SMX_LEAF_SENTER, %eax
	movl	%edi, %ebx
	movl	%esi, %ecx
	xorl	%edx, %edx
	getsec

1:
	/* Turn paging off - we are identity mapped so we will survive */
	movl	%cr0, %eax
	andl	$~(GRUB_CR0_X86_PG | GRUB_CR0_X86_NE | GRUB_CR0_X86_TS | GRUB_CR0_X86_MP), %eax
	movl	%eax, %cr0

	/* Disable long mode */
	movl	$(GRUB_MSR_X86_EFER), %ecx
	rdmsr
	andl	$~(GRUB_MSR_EFER_LME), %eax
	wrmsr

	/* Now in protected mode, disable PAE */
	movl	%cr4, %eax
	andl	$~(GRUB_CR4_X86_PAE), %eax
	movl	%eax, %cr4

	/* AMD SKINIT */
	movl	%edi, %eax
	skinit

	.align 8
dl_gdt:
	/* Null Segment */
	.quad	0
	/* 32b Code Segment */
	.word	0xffff /* Limit 1 */
	.word	0x0000 /* Base 1 */
	.byte	0x00   /* Base 2 */
	.byte	0x9b   /* P=1 DPL=0 S=1 Type=0010 C=0 W=1 A=1 */
	.byte	0xcf   /* G=1 D=1 L=0 AVL=0 Limit 2 */
	.byte	0x00   /* Base 3 */
	/* Data Segment, can be used both in 32b and 64b */
	.word	0xffff /* Limit 1 */
	.word	0x0000 /* Base 1 */
	.byte	0x00   /* Base 2 */
	.byte	0x93   /* P=1 DPL=0 S=1 Type=0010 C=0 W=1 A=1 */
	.byte	0xcf   /* G=1 D=1 L=0 AVL=0 Limit 2 */
	.byte	0x00   /* Base 3 */

	.word	0
dl_gdtr:
	.word	23 /* Limit */
dl_gdt_base:
	.long	0  /* Base */
	.long	0

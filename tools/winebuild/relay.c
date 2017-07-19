/*
 * Relay calls helper routines
 *
 * Copyright 1993 Robert J. Amstadt
 * Copyright 1995 Martin von Loewis
 * Copyright 1995, 1996, 1997 Alexandre Julliard
 * Copyright 1997 Eric Youngdale
 * Copyright 1999 Ulrich Weigand
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <ctype.h>
#include <stdarg.h>

#include "build.h"

/* offset of the stack pointer relative to %fs:(0) */
#define STACKOFFSET 0xc0  /* FIELD_OFFSET(TEB,WOW32Reserved) */

/* fix this if the x86_thread_data structure is changed */
#define GS_OFFSET  0x1d8  /* FIELD_OFFSET(TEB,SystemReserved2) + FIELD_OFFSET(struct x86_thread_data,gs) */

#define DPMI_VIF_OFFSET      (0x1fc + 0) /* FIELD_OFFSET(TEB,GdiTebBatch) + FIELD_OFFSET(WINE_VM86_TEB_INFO,dpmi_vif) */
#define VM86_PENDING_OFFSET  (0x1fc + 4) /* FIELD_OFFSET(TEB,GdiTebBatch) + FIELD_OFFSET(WINE_VM86_TEB_INFO,vm86_pending) */

static void function_header( const char *name )
{
    output( "\n\t.align %d\n", get_alignment(4) );
    output( "\t%s\n", func_declaration(name) );
    output( "%s\n", asm_globl(name) );
}


/*******************************************************************
 *         BuildCallFrom16Core
 *
 * This routine builds the core routines used in 16->32 thunks:
 * CallFrom16Word, CallFrom16Long, CallFrom16Register, and CallFrom16Thunk.
 *
 * These routines are intended to be called via a far call (with 32-bit
 * operand size) from 16-bit code.  The 16-bit code stub must push %bp,
 * the 32-bit entry point to be called, and the argument conversion
 * routine to be used (see stack layout below).
 *
 * The core routine completes the STACK16FRAME on the 16-bit stack and
 * switches to the 32-bit stack.  Then, the argument conversion routine
 * is called; it gets passed the 32-bit entry point and a pointer to the
 * 16-bit arguments (on the 16-bit stack) as parameters. (You can either
 * use conversion routines automatically generated by BuildCallFrom16,
 * or write your own for special purposes.)
 *
 * The conversion routine must call the 32-bit entry point, passing it
 * the converted arguments, and return its return value to the core.
 * After the conversion routine has returned, the core switches back
 * to the 16-bit stack, converts the return value to the DX:AX format
 * (CallFrom16Long), and returns to the 16-bit call stub.  All parameters,
 * including %bp, are popped off the stack.
 *
 * The 16-bit call stub now returns to the caller, popping the 16-bit
 * arguments if necessary (pascal calling convention).
 *
 * In the case of a 'register' function, CallFrom16Register fills a
 * CONTEXT86 structure with the values all registers had at the point
 * the first instruction of the 16-bit call stub was about to be
 * executed.  A pointer to this CONTEXT86 is passed as third parameter
 * to the argument conversion routine, which typically passes it on
 * to the called 32-bit entry point.
 *
 * CallFrom16Thunk is a special variant used by the implementation of
 * the Win95 16->32 thunk functions C16ThkSL and C16ThkSL01 and is
 * implemented as follows:
 * On entry, the EBX register is set up to contain a flat pointer to the
 * 16-bit stack such that EBX+22 points to the first argument.
 * Then, the entry point is called, while EBP is set up to point
 * to the return address (on the 32-bit stack).
 * The called function returns with CX set to the number of bytes
 * to be popped of the caller's stack.
 *
 * Stack layout upon entry to the core routine (STACK16FRAME):
 *  ...           ...
 * (sp+24) word   first 16-bit arg
 * (sp+22) word   cs
 * (sp+20) word   ip
 * (sp+18) word   bp
 * (sp+14) long   32-bit entry point (reused for Win16 mutex recursion count)
 * (sp+12) word   ip of actual entry point (necessary for relay debugging)
 * (sp+8)  long   relay (argument conversion) function entry point
 * (sp+4)  long   cs of 16-bit entry point
 * (sp)    long   ip of 16-bit entry point
 *
 * Added on the stack:
 * (sp-2)  word   saved gs
 * (sp-4)  word   saved fs
 * (sp-6)  word   saved es
 * (sp-8)  word   saved ds
 * (sp-12) long   saved ebp
 * (sp-16) long   saved ecx
 * (sp-20) long   saved edx
 * (sp-24) long   saved previous stack
 */
static void BuildCallFrom16Core( int reg_func, int thunk )
{
    /* Function header */
    if (thunk) function_header( "__wine_call_from_16_thunk" );
    else if (reg_func) function_header( "__wine_call_from_16_regs" );
    else function_header( "__wine_call_from_16" );

    /* Create STACK16FRAME (except STACK32FRAME link) */
    output( "\tpushw %%gs\n" );
    output( "\tpushw %%fs\n" );
    output( "\tpushw %%es\n" );
    output( "\tpushw %%ds\n" );
    output( "\tpushl %%ebp\n" );
    output( "\tpushl %%ecx\n" );
    output( "\tpushl %%edx\n" );

    /* Save original EFlags register */
    if (reg_func) output( "\tpushfl\n" );

    if ( UsePIC )
    {
        output( "\tcall 1f\n" );
        output( "1:\tpopl %%ecx\n" );
        output( "\t.byte 0x2e\n\tmovl %s-1b(%%ecx),%%edx\n", asm_name("CallTo16_DataSelector") );
    }
    else
        output( "\t.byte 0x2e\n\tmovl %s,%%edx\n", asm_name("CallTo16_DataSelector") );

    /* Load 32-bit segment registers */
    output( "\tmovw %%dx, %%ds\n" );
    output( "\tmovw %%dx, %%es\n" );

    if ( UsePIC )
        output( "\tmovw %s-1b(%%ecx), %%fs\n", asm_name("CallTo16_TebSelector") );
    else
        output( "\tmovw %s, %%fs\n", asm_name("CallTo16_TebSelector") );

    output( "\t.byte 0x64\n\tmov (%d),%%gs\n", GS_OFFSET );

    /* Translate STACK16FRAME base to flat offset in %edx */
    output( "\tmovw %%ss, %%dx\n" );
    output( "\tandl $0xfff8, %%edx\n" );
    output( "\tshrl $1, %%edx\n" );
    if (UsePIC)
    {
        output( "\taddl wine_ldt_copy_ptr-1b(%%ecx),%%edx\n" );
        output( "\tmovl (%%edx), %%edx\n" );
    }
    else
        output( "\tmovl %s(%%edx), %%edx\n", asm_name("wine_ldt_copy") );
    output( "\tmovzwl %%sp, %%ebp\n" );
    output( "\tleal %d(%%ebp,%%edx), %%edx\n", reg_func ? 0 : -4 );

    /* Get saved flags into %ecx */
    if (reg_func) output( "\tpopl %%ecx\n" );

    /* Get the 32-bit stack pointer from the TEB and complete STACK16FRAME */
    output( "\t.byte 0x64\n\tmovl (%d), %%ebp\n", STACKOFFSET );
    output( "\tpushl %%ebp\n" );

    /* Switch stacks */
    output( "\t.byte 0x64\n\tmovw %%ss, (%d)\n", STACKOFFSET + 2 );
    output( "\t.byte 0x64\n\tmovw %%sp, (%d)\n", STACKOFFSET );
    output( "\tpushl %%ds\n" );
    output( "\tpopl %%ss\n" );
    output( "\tmovl %%ebp, %%esp\n" );
    output( "\taddl $0x20,%%ebp\n");  /* FIELD_OFFSET(STACK32FRAME,ebp) */


    /* At this point:
       STACK16FRAME is completely set up
       DS, ES, SS: flat data segment
       FS: current TEB
       ESP: points to last STACK32FRAME
       EBP: points to ebp member of last STACK32FRAME
       EDX: points to current STACK16FRAME
       ECX: contains saved flags
       all other registers: unchanged */

    /* Special case: C16ThkSL stub */
    if ( thunk )
    {
        /* Set up registers as expected and call thunk */
        output( "\tleal 0x1a(%%edx),%%ebx\n" );  /* sizeof(STACK16FRAME)-22 */
        output( "\tleal -4(%%esp), %%ebp\n" );

        output( "\tcall *0x26(%%edx)\n");  /* FIELD_OFFSET(STACK16FRAME,entry_point) */

        /* Switch stack back */
        output( "\t.byte 0x64\n\tmovw (%d), %%ss\n", STACKOFFSET+2 );
        output( "\t.byte 0x64\n\tmovzwl (%d), %%esp\n", STACKOFFSET );
        output( "\t.byte 0x64\n\tpopl (%d)\n", STACKOFFSET );

        /* Restore registers and return directly to caller */
        output( "\taddl $8, %%esp\n" );
        output( "\tpopl %%ebp\n" );
        output( "\tpopw %%ds\n" );
        output( "\tpopw %%es\n" );
        output( "\tpopw %%fs\n" );
        output( "\tpopw %%gs\n" );
        output( "\taddl $20, %%esp\n" );

        output( "\txorb %%ch, %%ch\n" );
        output( "\tpopl %%ebx\n" );
        output( "\taddw %%cx, %%sp\n" );
        output( "\tpush %%ebx\n" );

        output( "\t.byte 0x66\n" );
        output( "\tlret\n" );

        output_function_size( "__wine_call_from_16_thunk" );
        return;
    }


    /* Build register CONTEXT */
    if ( reg_func )
    {
        output( "\tsubl $0x2cc,%%esp\n" );       /* sizeof(CONTEXT86) */

        output( "\tmovl %%ecx,0xc0(%%esp)\n" );  /* EFlags */

        output( "\tmovl %%eax,0xb0(%%esp)\n" );  /* Eax */
        output( "\tmovl %%ebx,0xa4(%%esp)\n" );  /* Ebx */
        output( "\tmovl %%esi,0xa0(%%esp)\n" );  /* Esi */
        output( "\tmovl %%edi,0x9c(%%esp)\n" );  /* Edi */

        output( "\tmovl 0x0c(%%edx),%%eax\n");   /* FIELD_OFFSET(STACK16FRAME,ebp) */
        output( "\tmovl %%eax,0xb4(%%esp)\n" );  /* Ebp */
        output( "\tmovl 0x08(%%edx),%%eax\n");   /* FIELD_OFFSET(STACK16FRAME,ecx) */
        output( "\tmovl %%eax,0xac(%%esp)\n" );  /* Ecx */
        output( "\tmovl 0x04(%%edx),%%eax\n");   /* FIELD_OFFSET(STACK16FRAME,edx) */
        output( "\tmovl %%eax,0xa8(%%esp)\n" );  /* Edx */

        output( "\tmovzwl 0x10(%%edx),%%eax\n"); /* FIELD_OFFSET(STACK16FRAME,ds) */
        output( "\tmovl %%eax,0x98(%%esp)\n" );  /* SegDs */
        output( "\tmovzwl 0x12(%%edx),%%eax\n"); /* FIELD_OFFSET(STACK16FRAME,es) */
        output( "\tmovl %%eax,0x94(%%esp)\n" );  /* SegEs */
        output( "\tmovzwl 0x14(%%edx),%%eax\n"); /* FIELD_OFFSET(STACK16FRAME,fs) */
        output( "\tmovl %%eax,0x90(%%esp)\n" );  /* SegFs */
        output( "\tmovzwl 0x16(%%edx),%%eax\n"); /* FIELD_OFFSET(STACK16FRAME,gs) */
        output( "\tmovl %%eax,0x8c(%%esp)\n" );  /* SegGs */

        output( "\tmovzwl 0x2e(%%edx),%%eax\n"); /* FIELD_OFFSET(STACK16FRAME,cs) */
        output( "\tmovl %%eax,0xbc(%%esp)\n" );  /* SegCs */
        output( "\tmovzwl 0x2c(%%edx),%%eax\n"); /* FIELD_OFFSET(STACK16FRAME,ip) */
        output( "\tmovl %%eax,0xb8(%%esp)\n" );  /* Eip */

        output( "\t.byte 0x64\n\tmovzwl (%d), %%eax\n", STACKOFFSET+2 );
        output( "\tmovl %%eax,0xc8(%%esp)\n" );  /* SegSs */
        output( "\t.byte 0x64\n\tmovzwl (%d), %%eax\n", STACKOFFSET );
        output( "\taddl $0x2c,%%eax\n");         /* FIELD_OFFSET(STACK16FRAME,ip) */
        output( "\tmovl %%eax,0xc4(%%esp)\n" );  /* Esp */
#if 0
        output( "\tfsave 0x1c(%%esp)\n" ); /* FloatSave */
#endif

        /* Push address of CONTEXT86 structure -- popped by the relay routine */
        output( "\tmovl %%esp,%%eax\n" );
        output( "\tandl $~15,%%esp\n" );
        output( "\tsubl $4,%%esp\n" );
        output( "\tpushl %%eax\n" );
    }
    else
    {
        output( "\tsubl $8,%%esp\n" );
        output( "\tandl $~15,%%esp\n" );
        output( "\taddl $8,%%esp\n" );
    }

    /* Call relay routine (which will call the API entry point) */
    output( "\tleal 0x30(%%edx),%%eax\n" ); /* sizeof(STACK16FRAME) */
    output( "\tpushl %%eax\n" );
    output( "\tpushl 0x26(%%edx)\n");  /* FIELD_OFFSET(STACK16FRAME,entry_point) */
    output( "\tcall *0x20(%%edx)\n");  /* FIELD_OFFSET(STACK16FRAME,relay) */

    if ( reg_func )
    {
        output( "\tleal -748(%%ebp),%%ebx\n" ); /* sizeof(CONTEXT) + FIELD_OFFSET(STACK32FRAME,ebp) */

        /* Switch stack back */
        output( "\t.byte 0x64\n\tmovw (%d), %%ss\n", STACKOFFSET+2 );
        output( "\t.byte 0x64\n\tmovzwl (%d), %%esp\n", STACKOFFSET );
        output( "\t.byte 0x64\n\tpopl (%d)\n", STACKOFFSET );

        /* Get return address to CallFrom16 stub */
        output( "\taddw $0x14,%%sp\n" ); /* FIELD_OFFSET(STACK16FRAME,callfrom_ip)-4 */
        output( "\tpopl %%eax\n" );
        output( "\tpopl %%edx\n" );

        /* Restore all registers from CONTEXT */
        output( "\tmovw 0xc8(%%ebx),%%ss\n");   /* SegSs */
        output( "\tmovl 0xc4(%%ebx),%%esp\n");  /* Esp */
        output( "\taddl $4, %%esp\n" );  /* room for final return address */

        output( "\tpushw 0xbc(%%ebx)\n");  /* SegCs */
        output( "\tpushw 0xb8(%%ebx)\n");  /* Eip */
        output( "\tpushl %%edx\n" );
        output( "\tpushl %%eax\n" );
        output( "\tpushl 0xc0(%%ebx)\n");  /* EFlags */
        output( "\tpushl 0x98(%%ebx)\n");  /* SegDs */

        output( "\tpushl 0x94(%%ebx)\n");  /* SegEs */
        output( "\tpopl %%es\n" );
        output( "\tpushl 0x90(%%ebx)\n");  /* SegFs */
        output( "\tpopl %%fs\n" );
        output( "\tpushl 0x8c(%%ebx)\n");  /* SegGs */
        output( "\tpopl %%gs\n" );

        output( "\tmovl 0xb4(%%ebx),%%ebp\n");  /* Ebp */
        output( "\tmovl 0xa0(%%ebx),%%esi\n");  /* Esi */
        output( "\tmovl 0x9c(%%ebx),%%edi\n");  /* Edi */
        output( "\tmovl 0xb0(%%ebx),%%eax\n");  /* Eax */
        output( "\tmovl 0xa8(%%ebx),%%edx\n");  /* Edx */
        output( "\tmovl 0xac(%%ebx),%%ecx\n");  /* Ecx */
        output( "\tmovl 0xa4(%%ebx),%%ebx\n");  /* Ebx */

        output( "\tpopl %%ds\n" );
        output( "\tpopfl\n" );
        output( "\tlret\n" );

        output_function_size( "__wine_call_from_16_regs" );
    }
    else
    {
        /* Switch stack back */
        output( "\t.byte 0x64\n\tmovw (%d), %%ss\n", STACKOFFSET+2 );
        output( "\t.byte 0x64\n\tmovzwl (%d), %%esp\n", STACKOFFSET );
        output( "\t.byte 0x64\n\tpopl (%d)\n", STACKOFFSET );

        /* Restore registers */
        output( "\tpopl %%edx\n" );
        output( "\tpopl %%ecx\n" );
        output( "\tpopl %%ebp\n" );
        output( "\tpopw %%ds\n" );
        output( "\tpopw %%es\n" );
        output( "\tpopw %%fs\n" );
        output( "\tpopw %%gs\n" );

        /* Return to return stub which will return to caller */
        output( "\tlret $12\n" );

        output_function_size( "__wine_call_from_16" );
    }
}


/*******************************************************************
 *         BuildCallTo16Core
 *
 * This routine builds the core routines used in 32->16 thunks:
 *
 * extern DWORD WINAPI wine_call_to_16( FARPROC16 target, DWORD cbArgs, PEXCEPTION_HANDLER handler );
 * extern void WINAPI wine_call_to_16_regs( CONTEXT86 *context, DWORD cbArgs, PEXCEPTION_HANDLER handler );
 *
 * These routines can be called directly from 32-bit code.
 *
 * All routines expect that the 16-bit stack contents (arguments) and the
 * return address (segptr to CallTo16_Ret) were already set up by the
 * caller; nb_args must contain the number of bytes to be conserved.  The
 * 16-bit SS:SP will be set accordingly.
 *
 * All other registers are either taken from the CONTEXT86 structure
 * or else set to default values.  The target routine address is either
 * given directly or taken from the CONTEXT86.
 */
static void BuildCallTo16Core( int reg_func )
{
    const char *name = reg_func ? "wine_call_to_16_regs" : "wine_call_to_16";

    /* Function header */
    function_header( name );

    /* Function entry sequence */
    output_cfi( ".cfi_startproc" );
    output( "\tpushl %%ebp\n" );
    output_cfi( ".cfi_adjust_cfa_offset 4" );
    output_cfi( ".cfi_rel_offset %%ebp,0" );
    output( "\tmovl %%esp, %%ebp\n" );
    output_cfi( ".cfi_def_cfa_register %%ebp" );

    /* Save the 32-bit registers */
    output( "\tpushl %%ebx\n" );
    output_cfi( ".cfi_rel_offset %%ebx,-4" );
    output( "\tpushl %%esi\n" );
    output_cfi( ".cfi_rel_offset %%esi,-8" );
    output( "\tpushl %%edi\n" );
    output_cfi( ".cfi_rel_offset %%edi,-12" );
    output( "\t.byte 0x64\n\tmov %%gs,(%d)\n", GS_OFFSET );

    /* Setup exception frame */
    output( "\t.byte 0x64\n\tpushl (%d)\n", STACKOFFSET );
    output( "\tpushl 16(%%ebp)\n" ); /* handler */
    output( "\t.byte 0x64\n\tpushl (0)\n" );
    output( "\t.byte 0x64\n\tmovl %%esp,(0)\n" );

    /* Call the actual CallTo16 routine (simulate a lcall) */
    output( "\tpushl %%cs\n" );
    output( "\tcall .L%s\n", name );

    /* Remove exception frame */
    output( "\t.byte 0x64\n\tpopl (0)\n" );
    output( "\taddl $4, %%esp\n" );
    output( "\t.byte 0x64\n\tpopl (%d)\n", STACKOFFSET );

    if ( !reg_func )
    {
        /* Convert return value */
        output( "\tandl $0xffff,%%eax\n" );
        output( "\tshll $16,%%edx\n" );
        output( "\torl %%edx,%%eax\n" );
    }
    else
    {
        /*
         * Modify CONTEXT86 structure to contain new values
         *
         * NOTE:  We restore only EAX, EBX, EDX, EDX, EBP, and ESP.
         *        The segment registers as well as ESI and EDI should
         *        not be modified by a well-behaved 16-bit routine in
         *        any case.  [If necessary, we could restore them as well,
         *        at the cost of a somewhat less efficient return path.]
         */

        output( "\tmovl 0x14(%%esp),%%edi\n" ); /* FIELD_OFFSET(STACK32FRAME,target) - FIELD_OFFSET(STACK32FRAME,edi) */
                /* everything above edi has been popped already */

        output( "\tmovl %%eax,0xb0(%%edi)\n");  /* Eax */
        output( "\tmovl %%ebx,0xa4(%%edi)\n");  /* Ebx */
        output( "\tmovl %%ecx,0xac(%%edi)\n");  /* Ecx */
        output( "\tmovl %%edx,0xa8(%%edi)\n");  /* Edx */
        output( "\tmovl %%ebp,0xb4(%%edi)\n");  /* Ebp */
        output( "\tmovl %%esi,0xc4(%%edi)\n");  /* Esp */
                 /* The return glue code saved %esp into %esi */
    }

    /* Restore the 32-bit registers */
    output( "\tpopl %%edi\n" );
    output_cfi( ".cfi_same_value %%edi" );
    output( "\tpopl %%esi\n" );
    output_cfi( ".cfi_same_value %%esi" );
    output( "\tpopl %%ebx\n" );
    output_cfi( ".cfi_same_value %%ebx" );

    /* Function exit sequence */
    output( "\tpopl %%ebp\n" );
    output_cfi( ".cfi_def_cfa %%esp,4" );
    output_cfi( ".cfi_same_value %%ebp" );
    output( "\tret $12\n" );
    output_cfi( ".cfi_endproc" );


    /* Start of the actual CallTo16 routine */

    output( ".L%s:\n", name );

    /* Switch to the 16-bit stack */
    output( "\tmovl %%esp,%%edx\n" );
    output( "\t.byte 0x64\n\tmovw (%d),%%ss\n", STACKOFFSET + 2);
    output( "\t.byte 0x64\n\tmovw (%d),%%sp\n", STACKOFFSET );
    output( "\t.byte 0x64\n\tmovl %%edx,(%d)\n", STACKOFFSET );

    /* Make %bp point to the previous stackframe (built by CallFrom16) */
    output( "\tmovzwl %%sp,%%ebp\n" );
    output( "\tleal 0x2a(%%ebp),%%ebp\n");  /* FIELD_OFFSET(STACK16FRAME,bp) */

    /* Add the specified offset to the new sp */
    output( "\tsubw 0x2c(%%edx), %%sp\n");  /* FIELD_OFFSET(STACK32FRAME,nb_args) */

    if (reg_func)
    {
        /* Push the called routine address */
        output( "\tmovl 0x28(%%edx),%%edx\n");  /* FIELD_OFFSET(STACK32FRAME,target) */
        output( "\tpushw 0xbc(%%edx)\n");  /* SegCs */
        output( "\tpushw 0xb8(%%edx)\n");  /* Eip */

        /* Get the registers */
        output( "\tpushw 0x98(%%edx)\n");  /* SegDs */
        output( "\tpushl 0x94(%%edx)\n");  /* SegEs */
        output( "\tpopl %%es\n" );
        output( "\tpushl 0x90(%%edx)\n");  /* SegFs */
        output( "\tpopl %%fs\n" );
        output( "\tpushl 0x8c(%%edx)\n");  /* SegGs */
        output( "\tpopl %%gs\n" );
        output( "\tmovl 0xb4(%%edx),%%ebp\n");  /* Ebp */
        output( "\tmovl 0xa0(%%edx),%%esi\n");  /* Esi */
        output( "\tmovl 0x9c(%%edx),%%edi\n");  /* Edi */
        output( "\tmovl 0xb0(%%edx),%%eax\n");  /* Eax */
        output( "\tmovl 0xa4(%%edx),%%ebx\n");  /* Ebx */
        output( "\tmovl 0xac(%%edx),%%ecx\n");  /* Ecx */
        output( "\tmovl 0xa8(%%edx),%%edx\n");  /* Edx */

        /* Get the 16-bit ds */
        output( "\tpopw %%ds\n" );
    }
    else  /* not a register function */
    {
        /* Push the called routine address */
        output( "\tpushl 0x28(%%edx)\n"); /* FIELD_OFFSET(STACK32FRAME,target) */

        /* Set %fs and %gs to the value saved by the last CallFrom16 */
        output( "\tpushw -22(%%ebp)\n" ); /* FIELD_OFFSET(STACK16FRAME,fs)-FIELD_OFFSET(STACK16FRAME,bp) */
        output( "\tpopw %%fs\n" );
        output( "\tpushw -20(%%ebp)\n" ); /* FIELD_OFFSET(STACK16FRAME,gs)-FIELD_OFFSET(STACK16FRAME,bp) */
        output( "\tpopw %%gs\n" );

        /* Set %ds and %es (and %ax just in case) equal to %ss */
        output( "\tmovw %%ss,%%ax\n" );
        output( "\tmovw %%ax,%%ds\n" );
        output( "\tmovw %%ax,%%es\n" );
    }

    /* Jump to the called routine */
    output( "\t.byte 0x66\n" );
    output( "\tlret\n" );

    /* Function footer */
    output_function_size( name );
}


/*******************************************************************
 *         BuildRet16Func
 *
 * Build the return code for 16-bit callbacks
 */
static void BuildRet16Func(void)
{
    function_header( "__wine_call_to_16_ret" );

    /* Save %esp into %esi */
    output( "\tmovl %%esp,%%esi\n" );

    /* Restore 32-bit segment registers */

    output( "\t.byte 0x2e\n\tmovl %s", asm_name("CallTo16_DataSelector") );
    output( "-%s,%%edi\n", asm_name("__wine_call16_start") );
    output( "\tmovw %%di,%%ds\n" );
    output( "\tmovw %%di,%%es\n" );

    output( "\t.byte 0x2e\n\tmov %s", asm_name("CallTo16_TebSelector") );
    output( "-%s,%%fs\n", asm_name("__wine_call16_start") );

    output( "\t.byte 0x64\n\tmov (%d),%%gs\n", GS_OFFSET );

    /* Restore the 32-bit stack */

    output( "\tmovw %%di,%%ss\n" );
    output( "\t.byte 0x64\n\tmovl (%d),%%esp\n", STACKOFFSET );

    /* Return to caller */

    output( "\tlret\n" );
    output_function_size( "__wine_call_to_16_ret" );
}


/*******************************************************************
 *         BuildCallTo32CBClient
 *
 * Call a CBClient relay stub from 32-bit code (KERNEL.620).
 *
 * Since the relay stub is itself 32-bit, this should not be a problem;
 * unfortunately, the relay stubs are expected to switch back to a
 * 16-bit stack (and 16-bit code) after completion :-(
 *
 * This would conflict with our 16- vs. 32-bit stack handling, so
 * we simply switch *back* to our 32-bit stack before returning to
 * the caller ...
 *
 * The CBClient relay stub expects to be called with the following
 * 16-bit stack layout, and with ebp and ebx pointing into the 16-bit
 * stack at the designated places:
 *
 *    ...
 *  (ebp+14) original arguments to the callback routine
 *  (ebp+10) far return address to original caller
 *  (ebp+6)  Thunklet target address
 *  (ebp+2)  Thunklet relay ID code
 *  (ebp)    BP (saved by CBClientGlueSL)
 *  (ebp-2)  SI (saved by CBClientGlueSL)
 *  (ebp-4)  DI (saved by CBClientGlueSL)
 *  (ebp-6)  DS (saved by CBClientGlueSL)
 *
 *   ...     buffer space used by the 16-bit side glue for temp copies
 *
 *  (ebx+4)  far return address to 16-bit side glue code
 *  (ebx)    saved 16-bit ss:sp (pointing to ebx+4)
 *
 * The 32-bit side glue code accesses both the original arguments (via ebp)
 * and the temporary copies prepared by the 16-bit side glue (via ebx).
 * After completion, the stub will load ss:sp from the buffer at ebx
 * and perform a far return to 16-bit code.
 *
 * To trick the relay stub into returning to us, we replace the 16-bit
 * return address to the glue code by a cs:ip pair pointing to our
 * return entry point (the original return address is saved first).
 * Our return stub thus called will then reload the 32-bit ss:esp and
 * return to 32-bit code (by using and ss:esp value that we have also
 * pushed onto the 16-bit stack before and a cs:eip values found at
 * that position on the 32-bit stack).  The ss:esp to be restored is
 * found relative to the 16-bit stack pointer at:
 *
 *  (ebx-4)   ss  (flat)
 *  (ebx-8)   sp  (32-bit stack pointer)
 *
 * The second variant of this routine, CALL32_CBClientEx, which is used
 * to implement KERNEL.621, has to cope with yet another problem: Here,
 * the 32-bit side directly returns to the caller of the CBClient thunklet,
 * restoring registers saved by CBClientGlueSL and cleaning up the stack.
 * As we have to return to our 32-bit code first, we have to adapt the
 * layout of our temporary area so as to include values for the registers
 * that are to be restored, and later (in the implementation of KERNEL.621)
 * we *really* restore them. The return stub restores DS, DI, SI, and BP
 * from the stack, skips the next 8 bytes (CBClient relay code / target),
 * and then performs a lret NN, where NN is the number of arguments to be
 * removed. Thus, we prepare our temporary area as follows:
 *
 *     (ebx+22) 16-bit cs  (this segment)
 *     (ebx+20) 16-bit ip  ('16-bit' return entry point)
 *     (ebx+16) 32-bit ss  (flat)
 *     (ebx+12) 32-bit sp  (32-bit stack pointer)
 *     (ebx+10) 16-bit bp  (points to ebx+24)
 *     (ebx+8)  16-bit si  (ignored)
 *     (ebx+6)  16-bit di  (ignored)
 *     (ebx+4)  16-bit ds  (we actually use the flat DS here)
 *     (ebx+2)  16-bit ss  (16-bit stack segment)
 *     (ebx+0)  16-bit sp  (points to ebx+4)
 *
 * Note that we ensure that DS is not changed and remains the flat segment,
 * and the 32-bit stack pointer our own return stub needs fits just
 * perfectly into the 8 bytes that are skipped by the Windows stub.
 * One problem is that we have to determine the number of removed arguments,
 * as these have to be really removed in KERNEL.621. Thus, the BP value
 * that we place in the temporary area to be restored, contains the value
 * that SP would have if no arguments were removed. By comparing the actual
 * value of SP with this value in our return stub we can compute the number
 * of removed arguments. This is then returned to KERNEL.621.
 *
 * The stack layout of this function:
 * (ebp+20)  nArgs     pointer to variable receiving nr. of args (Ex only)
 * (ebp+16)  esi       pointer to caller's esi value
 * (ebp+12)  arg       ebp value to be set for relay stub
 * (ebp+8)   func      CBClient relay stub address
 * (ebp+4)   ret addr
 * (ebp)     ebp
 */
static void BuildCallTo32CBClient( int isEx )
{
    function_header( isEx ? "CALL32_CBClientEx" : "CALL32_CBClient" );

    /* Entry code */

    output_cfi( ".cfi_startproc" );
    output( "\tpushl %%ebp\n" );
    output_cfi( ".cfi_adjust_cfa_offset 4" );
    output_cfi( ".cfi_rel_offset %%ebp,0" );
    output( "\tmovl %%esp,%%ebp\n" );
    output_cfi( ".cfi_def_cfa_register %%ebp" );
    output( "\tpushl %%edi\n" );
    output_cfi( ".cfi_rel_offset %%edi,-4" );
    output( "\tpushl %%esi\n" );
    output_cfi( ".cfi_rel_offset %%esi,-8" );
    output( "\tpushl %%ebx\n" );
    output_cfi( ".cfi_rel_offset %%ebx,-12" );

    /* Get pointer to temporary area and save the 32-bit stack pointer */

    output( "\tmovl 16(%%ebp), %%ebx\n" );
    output( "\tleal -8(%%esp), %%eax\n" );

    if ( !isEx )
        output( "\tmovl %%eax, -8(%%ebx)\n" );
    else
        output( "\tmovl %%eax, 12(%%ebx)\n" );

    /* Set up registers and call CBClient relay stub (simulating a far call) */

    output( "\tmovl 20(%%ebp), %%esi\n" );
    output( "\tmovl (%%esi), %%esi\n" );

    output( "\tmovl 8(%%ebp), %%eax\n" );
    output( "\tmovl 12(%%ebp), %%ebp\n" );

    output( "\tpushl %%cs\n" );
    output( "\tcall *%%eax\n" );

    /* Return new esi value to caller */

    output( "\tmovl 32(%%esp), %%edi\n" );
    output( "\tmovl %%esi, (%%edi)\n" );

    /* Return argument size to caller */
    if ( isEx )
    {
        output( "\tmovl 36(%%esp), %%ebx\n" );
        output( "\tmovl %%ebp, (%%ebx)\n" );
    }

    /* Restore registers and return */

    output( "\tpopl %%ebx\n" );
    output_cfi( ".cfi_same_value %%ebx" );
    output( "\tpopl %%esi\n" );
    output_cfi( ".cfi_same_value %%esi" );
    output( "\tpopl %%edi\n" );
    output_cfi( ".cfi_same_value %%edi" );
    output( "\tpopl %%ebp\n" );
    output_cfi( ".cfi_def_cfa %%esp,4" );
    output_cfi( ".cfi_same_value %%ebp" );
    output( "\tret\n" );
    output_cfi( ".cfi_endproc" );
    output_function_size( isEx ? "CALL32_CBClientEx" : "CALL32_CBClient" );

    /* '16-bit' return stub */

    function_header( isEx ? "CALL32_CBClientEx_Ret" : "CALL32_CBClient_Ret" );
    if ( !isEx )
    {
        output( "\tmovzwl %%sp, %%ebx\n" );
        output( "\tlssl %%ss:-16(%%ebx), %%esp\n" );
    }
    else
    {
        output( "\tmovzwl %%bp, %%ebx\n" );
        output( "\tsubw %%bp, %%sp\n" );
        output( "\tmovzwl %%sp, %%ebp\n" );
        output( "\tlssl %%ss:-12(%%ebx), %%esp\n" );
    }
    output( "\tlret\n" );
    output_function_size( isEx ? "CALL32_CBClientEx_Ret" : "CALL32_CBClient_Ret" );
}


/*******************************************************************
 *         build_call_from_regs_x86
 *
 * Build a 32-bit-to-Wine call-back function for a 'register' function.
 * 'args' is the number of dword arguments.
 *
 * Stack layout:
 *   ...
 * (ebp+20)  first arg
 * (ebp+16)  ret addr to user code
 * (ebp+12)  func to call (relative to relay code ret addr)
 * (ebp+8)   number of args
 * (ebp+4)   ret addr to relay code
 * (ebp+0)   saved ebp
 * (ebp-128) buffer area to allow stack frame manipulation
 * (ebp-332) CONTEXT86 struct
 * (ebp-336) padding for stack alignment
 * (ebp-336-n) CONTEXT86 *argument
 *  ....     other arguments copied from (ebp+12)
 *
 * The entry point routine is called with a CONTEXT* extra argument,
 * following the normal args. In this context structure, EIP_reg
 * contains the return address to user code, and ESP_reg the stack
 * pointer on return (with the return address and arguments already
 * removed).
 */
static void build_call_from_regs_x86(void)
{
    static const int STACK_SPACE = 128 + 0x2cc /* sizeof(CONTEXT86) */;

    /* Function header */

    output( "\t.text\n" );
    function_header( "__wine_call_from_regs" );

    /* Allocate some buffer space on the stack */

    output_cfi( ".cfi_startproc" );
    output( "\tpushl %%ebp\n" );
    output_cfi( ".cfi_adjust_cfa_offset 4" );
    output_cfi( ".cfi_rel_offset %%ebp,0" );
    output( "\tmovl %%esp,%%ebp\n" );
    output_cfi( ".cfi_def_cfa_register %%ebp" );
    output( "\tleal -%d(%%esp),%%esp\n", STACK_SPACE );

    /* Build the context structure */

    output( "\tmovl %%eax,0xb0(%%esp)\n" );  /* Eax */
    output( "\tpushfl\n" );
    output( "\tpopl %%eax\n" );
    output( "\tmovl %%eax,0xc0(%%esp)\n");  /* EFlags */
    output( "\tmovl 0(%%ebp),%%eax\n" );
    output( "\tmovl %%eax,0xb4(%%esp)\n");  /* Ebp */
    output( "\tmovl %%ebx,0xa4(%%esp)\n");  /* Ebx */
    output( "\tmovl %%ecx,0xac(%%esp)\n");  /* Ecx */
    output( "\tmovl %%edx,0xa8(%%esp)\n");  /* Edx */
    output( "\tmovl %%esi,0xa0(%%esp)\n");  /* Esi */
    output( "\tmovl %%edi,0x9c(%%esp)\n");  /* Edi */

    output( "\txorl %%eax,%%eax\n" );
    output( "\tmovw %%cs,%%ax\n" );
    output( "\tmovl %%eax,0xbc(%%esp)\n");  /* SegCs */
    output( "\tmovw %%es,%%ax\n" );
    output( "\tmovl %%eax,0x94(%%esp)\n");  /* SegEs */
    output( "\tmovw %%fs,%%ax\n" );
    output( "\tmovl %%eax,0x90(%%esp)\n");  /* SegFs */
    output( "\tmovw %%gs,%%ax\n" );
    output( "\tmovl %%eax,0x8c(%%esp)\n");  /* SegGs */
    output( "\tmovw %%ss,%%ax\n" );
    output( "\tmovl %%eax,0xc8(%%esp)\n");  /* SegSs */
    output( "\tmovw %%ds,%%ax\n" );
    output( "\tmovl %%eax,0x98(%%esp)\n");  /* SegDs */
    output( "\tmovw %%ax,%%es\n" );  /* set %es equal to %ds just in case */

    output( "\tmovl $0x10007,0(%%esp)\n");  /* ContextFlags */

    output( "\tmovl 16(%%ebp),%%eax\n" ); /* Get %eip at time of call */
    output( "\tmovl %%eax,0xb8(%%esp)\n");  /* Eip */

    /* Transfer the arguments */

    output( "\tmovl 8(%%ebp),%%ecx\n" );    /* fetch number of args to copy */
    output( "\tleal 4(,%%ecx,4),%%edx\n" ); /* add 4 for context arg */
    output( "\tsubl %%edx,%%esp\n" );
    output( "\tandl $~15,%%esp\n" );
    output( "\tleal 20(%%ebp),%%esi\n" );  /* get %esp at time of call */
    output( "\tmovl %%esp,%%edi\n" );
    output( "\ttest %%ecx,%%ecx\n" );
    output( "\tjz 1f\n" );
    output( "\tcld\n" );
    output( "\trep\n\tmovsl\n" );  /* copy args */
    output( "1:\tleal %d(%%ebp),%%eax\n", -STACK_SPACE );  /* get addr of context struct */
    output( "\tmovl %%eax,(%%edi)\n" );    /* and pass it as extra arg */
    output( "\tmovl %%esi,%d(%%ebp)\n", 0xc4 /* Esp */ - STACK_SPACE );

    /* Call the entry point */

    output( "\tmovl 4(%%ebp),%%eax\n" );   /* get relay code addr */
    output( "\taddl 12(%%ebp),%%eax\n" );
    output( "\tcall *%%eax\n" );
    output( "\tleal -%d(%%ebp),%%ecx\n", STACK_SPACE );

    /* Restore the context structure */

    output( "2:\tpushl 0x94(%%ecx)\n" );    /* SegEs */
    output( "\tpopl %%es\n" );
    output( "\tpushl 0x90(%%ecx)\n" );      /* SegFs */
    output( "\tpopl %%fs\n" );
    output( "\tpushl 0x8c(%%ecx)\n" );      /* SegGs */
    output( "\tpopl %%gs\n" );

    output( "\tmovw %%ss,%%ax\n" );
    output( "\tcmpw 0xc8(%%ecx),%%ax\n" );  /* SegSs */
    output( "\tjne 3f\n" );

    /* As soon as we have switched stacks the context structure could
     * be invalid (when signal handlers are executed for example). Copy
     * values on the target stack before changing ESP. */

    output( "\tmovl 0xc4(%%ecx),%%eax\n" ); /* Esp */
    output( "\tleal -4*4(%%eax),%%eax\n" );

    output( "\tmovl 0xc0(%%ecx),%%edx\n" ); /* EFlags */
    output( "\t.byte 0x36\n\tmovl %%edx,3*4(%%eax)\n" );
    output( "\tmovl 0xbc(%%ecx),%%edx\n" ); /* SegCs */
    output( "\t.byte 0x36\n\tmovl %%edx,2*4(%%eax)\n" );
    output( "\tmovl 0xb8(%%ecx),%%edx\n" ); /* Eip */
    output( "\t.byte 0x36\n\tmovl %%edx,1*4(%%eax)\n" );
    output( "\tmovl 0xb0(%%ecx),%%edx\n" ); /* Eax */
    output( "\t.byte 0x36\n\tmovl %%edx,0*4(%%eax)\n" );

    output( "\tpushl 0x98(%%ecx)\n" );      /* SegDs */

    output( "\tmovl 0x9c(%%ecx),%%edi\n" ); /* Edi */
    output( "\tmovl 0xa0(%%ecx),%%esi\n" ); /* Esi */
    output( "\tmovl 0xa4(%%ecx),%%ebx\n" ); /* Ebx */
    output( "\tmovl 0xa8(%%ecx),%%edx\n" ); /* Edx */
    output( "\tmovl 0xb4(%%ecx),%%ebp\n" ); /* Ebp */
    output( "\tmovl 0xac(%%ecx),%%ecx\n" ); /* Ecx */

    output( "\tpopl %%ds\n" );
    output( "\tmovl %%eax,%%esp\n" );

    output( "\tpopl %%eax\n" );
    output( "\tiret\n" );

    output("3:\n");

    /* Restore the context when the stack segment changes. We can't use
     * the same code as above because we do not know if the stack segment
     * is 16 or 32 bit, and 'movl' will throw an exception when we try to
     * access memory above the limit. */

    output( "\tmovl 0x9c(%%ecx),%%edi\n" ); /* Edi */
    output( "\tmovl 0xa0(%%ecx),%%esi\n" ); /* Esi */
    output( "\tmovl 0xa4(%%ecx),%%ebx\n" ); /* Ebx */
    output( "\tmovl 0xa8(%%ecx),%%edx\n" ); /* Edx */
    output( "\tmovl 0xb0(%%ecx),%%eax\n" ); /* Eax */
    output( "\tmovl 0xb4(%%ecx),%%ebp\n" ); /* Ebp */

    output( "\tpushl 0xc8(%%ecx)\n" );      /* SegSs */
    output( "\tpopl %%ss\n" );
    output( "\tmovl 0xc4(%%ecx),%%esp\n" ); /* Esp */

    output( "\tpushl 0xc0(%%ecx)\n" );      /* EFlags */
    output( "\tpushl 0xbc(%%ecx)\n" );      /* SegCs */
    output( "\tpushl 0xb8(%%ecx)\n" );      /* Eip */
    output( "\tpushl 0x98(%%ecx)\n" );      /* SegDs */
    output( "\tmovl 0xac(%%ecx),%%ecx\n" ); /* Ecx */

    output( "\tpopl %%ds\n" );
    output( "\tiret\n" );
    output_cfi( ".cfi_endproc" );
    output_function_size( "__wine_call_from_regs" );

    function_header( "__wine_restore_regs" );
    output_cfi( ".cfi_startproc" );
    output( "\tmovl 4(%%esp),%%ecx\n" );
    output( "\tjmp 2b\n" );
    output_cfi( ".cfi_endproc" );
    output_function_size( "__wine_restore_regs" );
}


/*******************************************************************
 *         BuildPendingEventCheck
 *
 * Build a function that checks whether there are any
 * pending DPMI events.
 *
 * Stack layout:
 *   
 * (sp+12) long   eflags
 * (sp+6)  long   cs
 * (sp+2)  long   ip
 * (sp)    word   fs
 *
 * On entry to function, fs register points to a valid TEB.
 * On exit from function, stack will be popped.
 */
static void BuildPendingEventCheck(void)
{
    /* Function header */

    function_header( "DPMI_PendingEventCheck" );

    /* Check for pending events. */

    output( "\t.byte 0x64\n\ttestl $0xffffffff,(%d)\n", VM86_PENDING_OFFSET );
    output( "\tje %s\n", asm_name("DPMI_PendingEventCheck_Cleanup") );
    output( "\t.byte 0x64\n\ttestl $0xffffffff,(%d)\n", DPMI_VIF_OFFSET );
    output( "\tje %s\n", asm_name("DPMI_PendingEventCheck_Cleanup") );

    /* Process pending events. */

    output( "\tsti\n" );

    /* Start cleanup. Restore fs register. */

    output( "%s\n", asm_globl("DPMI_PendingEventCheck_Cleanup") );
    output( "\tpopw %%fs\n" );

    /* Return from function. */

    output( "%s\n", asm_globl("DPMI_PendingEventCheck_Return") );
    output( "\tiret\n" );

    output_function_size( "DPMI_PendingEventCheck" );
}


/*******************************************************************
 *         output_asm_relays16
 *
 * Build all the 16-bit relay callbacks
 */
void output_asm_relays16(void)
{
    /* File header */

    output( "\t.text\n" );
    output( "%s:\n\n", asm_name("__wine_spec_thunk_text_16") );

    output( "%s\n", asm_globl("__wine_call16_start") );

    /* Standard CallFrom16 routine */
    BuildCallFrom16Core( 0, 0 );

    /* Register CallFrom16 routine */
    BuildCallFrom16Core( 1, 0 );

    /* C16ThkSL CallFrom16 routine */
    BuildCallFrom16Core( 0, 1 );

    /* Standard CallTo16 routine */
    BuildCallTo16Core( 0 );

    /* Register CallTo16 routine */
    BuildCallTo16Core( 1 );

    /* Standard CallTo16 return stub */
    BuildRet16Func();

    /* CBClientThunkSL routine */
    BuildCallTo32CBClient( 0 );

    /* CBClientThunkSLEx routine */
    BuildCallTo32CBClient( 1  );

    /* Pending DPMI events check stub */
    BuildPendingEventCheck();

    output( "%s\n", asm_globl("__wine_call16_end") );
    output_function_size( "__wine_spec_thunk_text_16" );

    /* Declare the return address and data selector variables */
    output( "\n\t.data\n\t.align %d\n", get_alignment(4) );
    output( "%s\n\t.long 0\n", asm_globl("CallTo16_DataSelector") );
    output( "%s\n\t.long 0\n", asm_globl("CallTo16_TebSelector") );

    output( "\t.text\n" );
    output( "%s:\n", asm_name("__wine_spec_thunk_text_32") );
    build_call_from_regs_x86();
    output_function_size( "__wine_spec_thunk_text_32" );
}


/*******************************************************************
 *         output_asm_relays
 *
 * Build all the assembly relay callbacks
 */
void output_asm_relays(void)
{
    switch (target_cpu)
    {
    case CPU_x86:
        build_call_from_regs_x86();
        break;
    default:
        break;
    }
}

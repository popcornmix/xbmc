/*
 * Copyright (c) 2014 RISC OS Open Ltd
 * Author: Ben Avison <bavison@riscosopen.org>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

// This code uses too many ARM-only tricks to easily assemble as Thumb
.arm
#undef CONFIG_THUMB
#define CONFIG_THUMB 0

#include "libavutil/arm/asm.S"

#define MAX_CHANNELS        8
#define MAX_FIR_ORDER       8
#define MAX_IIR_ORDER       4
#define MAX_RATEFACTOR      4
#define MAX_BLOCKSIZE       (40 * MAX_RATEFACTOR)

PST     .req    a1
PCO     .req    a2
AC0     .req    a3
AC1     .req    a4
CO0     .req    v1
CO1     .req    v2
CO2     .req    v3
CO3     .req    v4
ST0     .req    v5
ST1     .req    v6
ST2     .req    sl
ST3     .req    fp
I       .req    ip
PSAMP   .req    lr


// Some macros that do loads/multiplies where the register number is determined
// from an assembly-time expression. Boy is GNU assembler's syntax ugly...

.macro load  group, index, base, offset
       .altmacro
       load_ \group, %(\index), \base, \offset
       .noaltmacro
.endm

.macro load_ group, index, base, offset
        ldr     \group\index, [\base, #\offset]
.endm

.macro loadd  group, index, base, offset
       .altmacro
       loadd_ \group, %(\index), %(\index+1), \base, \offset
       .noaltmacro
.endm

.macro loadd_ group, index0, index1, base, offset
A .if offset >= 256
A       ldr     \group\index0, [\base, #\offset]
A       ldr     \group\index1, [\base, #(\offset) + 4]
A .else
        ldrd    \group\index0, \group\index1, [\base, #\offset]
A .endif
.endm

.macro multiply  index, accumulate, long
        .altmacro
        multiply_ %(\index), \accumulate, \long
        .noaltmacro
.endm

.macro multiply_  index, accumulate, long
 .if \long
  .if \accumulate
        smlal   AC0, AC1, CO\index, ST\index
  .else
        smull   AC0, AC1, CO\index, ST\index
  .endif
 .else
  .if \accumulate
        mla     AC0, CO\index, ST\index, AC0
  .else
        mul     AC0, CO\index, ST\index
  .endif
 .endif
.endm

.macro loadregoffsh2  group, index, base, offgroup, offindex
       .altmacro
       loadregoffsh2_ \group, %(\index), \base, \offgroup, %(\offindex)
       .noaltmacro
.endm

.macro loadregoffsh2_ group, index, base, offgroup, offindex
        ldr     \group\index, [\base, \offgroup\offindex, lsl #2]
.endm

.macro eorlslreg  check, data, group, index
        .altmacro
        eorlslreg_ \check, \data, \group, %(\index)
        .noaltmacro
.endm

.macro eorlslreg_ check, data, group, index
        eor     \check, \check, \data, lsl \group\index
.endm

// A macro to update the load register number and load offsets

.macro inc  howmany
  .set LOAD_REG, (LOAD_REG + \howmany) & 3
  .set OFFSET_CO, OFFSET_CO + 4 * \howmany
  .set OFFSET_ST, OFFSET_ST + 4 * \howmany
  .if FIR_REMAIN > 0
    .set FIR_REMAIN, FIR_REMAIN - \howmany
    .if FIR_REMAIN == 0
      .set OFFSET_CO, 4 * MAX_FIR_ORDER
      .set OFFSET_ST, 4 * (MAX_BLOCKSIZE + MAX_FIR_ORDER)
    .endif
  .elseif IIR_REMAIN > 0
    .set IIR_REMAIN, IIR_REMAIN - \howmany
  .endif
.endm

// Macro to implement the inner loop for one specific combination of parameters

.macro implement_filter  mask_minus1, shift_0, shift_8, iir_taps, fir_taps
  .set TOTAL_TAPS, \iir_taps + \fir_taps

  // Deal with register allocation...
  .set DEFINED_SHIFT, 0
  .set DEFINED_MASK, 0
  .set SHUFFLE_SHIFT, 0
  .set SHUFFLE_MASK, 0
  .set SPILL_SHIFT, 0
  .set SPILL_MASK, 0
  .if TOTAL_TAPS == 0
    // Little register pressure in this case - just keep MASK where it was
    .if !\mask_minus1
      MASK .req ST1
      .set DEFINED_MASK, 1
    .endif
  .else
    .if \shift_0
      .if !\mask_minus1
        // AC1 is unused with shift 0
        MASK .req AC1
        .set DEFINED_MASK, 1
        .set SHUFFLE_MASK, 1
      .endif
    .elseif \shift_8
      .if !\mask_minus1
        .if TOTAL_TAPS <= 4
        // All coefficients are preloaded (so pointer not needed)
          MASK .req PCO
          .set DEFINED_MASK, 1
          .set SHUFFLE_MASK, 1
        .else
          .set SPILL_MASK, 1
        .endif
      .endif
    .else // shift not 0 or 8
      .if TOTAL_TAPS <= 3
        // All coefficients are preloaded, and at least one CO register is unused
        .if \fir_taps & 1
          SHIFT .req CO0
          .set DEFINED_SHIFT, 1
          .set SHUFFLE_SHIFT, 1
        .else
          SHIFT .req CO3
          .set DEFINED_SHIFT, 1
          .set SHUFFLE_SHIFT, 1
        .endif
        .if !\mask_minus1
          MASK .req PCO
          .set DEFINED_MASK, 1
          .set SHUFFLE_MASK, 1
        .endif
      .elseif TOTAL_TAPS == 4
        // All coefficients are preloaded
        SHIFT .req PCO
        .set DEFINED_SHIFT, 1
        .set SHUFFLE_SHIFT, 1
        .if !\mask_minus1
          .set SPILL_MASK, 1
        .endif
      .else
        .set SPILL_SHIFT, 1
        .if !\mask_minus1
          .set SPILL_MASK, 1
        .endif
      .endif
    .endif
  .endif
  .if SPILL_SHIFT
    SHIFT .req ST0
    .set DEFINED_SHIFT, 1
  .endif
  .if SPILL_MASK
    MASK .req ST1
    .set DEFINED_MASK, 1
  .endif

        // Preload coefficients if possible
  .if TOTAL_TAPS <= 4
    .set OFFSET_CO, 0
    .if \fir_taps & 1
      .set LOAD_REG, 1
    .else
      .set LOAD_REG, 0
    .endif
    .rept \fir_taps
        load    CO, LOAD_REG, PCO, OFFSET_CO
      .set LOAD_REG, (LOAD_REG + 1) & 3
      .set OFFSET_CO, OFFSET_CO + 4
    .endr
    .set OFFSET_CO, 4 * MAX_FIR_ORDER
    .rept \iir_taps
        load    CO, LOAD_REG, PCO, OFFSET_CO
      .set LOAD_REG, (LOAD_REG + 1) & 3
      .set OFFSET_CO, OFFSET_CO + 4
    .endr
  .endif

        // Move mask/shift to final positions if necessary
        // Need to do this after preloading, because in some cases we
        // reuse the coefficient pointer register
  .if SHUFFLE_SHIFT
        mov     SHIFT, ST0
  .endif
  .if SHUFFLE_MASK
        mov     MASK, ST1
  .endif

        // Begin loop
01:
  .if TOTAL_TAPS == 0
        // Things simplify a lot in this case
        // In fact this could be pipelined further if it's worth it...
        ldr     ST0, [PSAMP]
        subs    I, I, #1
    .if !\mask_minus1
        and     ST0, ST0, MASK
    .endif
        str     ST0, [PST, #-4]!
        str     ST0, [PST, #4 * (MAX_BLOCKSIZE + MAX_FIR_ORDER)]
        str     ST0, [PSAMP], #4 * MAX_CHANNELS
        bne     01b
  .else
    .if \fir_taps & 1
      .set LOAD_REG, 1
    .else
      .set LOAD_REG, 0
    .endif
    .set LOAD_BANK, 0
    .set FIR_REMAIN, \fir_taps
    .set IIR_REMAIN, \iir_taps
    .if FIR_REMAIN == 0 // only IIR terms
      .set OFFSET_CO, 4 * MAX_FIR_ORDER
      .set OFFSET_ST, 4 * (MAX_BLOCKSIZE + MAX_FIR_ORDER)
    .else
      .set OFFSET_CO, 0
      .set OFFSET_ST, 0
    .endif
    .set MUL_REG, LOAD_REG
    .set COUNTER, 0
    .rept TOTAL_TAPS + 2
        // Do load(s)
     .if FIR_REMAIN != 0 || IIR_REMAIN != 0
      .if COUNTER == 0
       .if TOTAL_TAPS > 4
        load    CO, LOAD_REG, PCO, OFFSET_CO
       .endif
        load    ST, LOAD_REG, PST, OFFSET_ST
        inc     1
      .elseif COUNTER == 1 && (\fir_taps & 1) == 0
       .if TOTAL_TAPS > 4
        load    CO, LOAD_REG, PCO, OFFSET_CO
       .endif
        load    ST, LOAD_REG, PST, OFFSET_ST
        inc     1
      .elseif LOAD_BANK == 0
       .if TOTAL_TAPS > 4
        .if FIR_REMAIN == 0 && IIR_REMAIN == 1
        load    CO, LOAD_REG, PCO, OFFSET_CO
        .else
        loadd   CO, LOAD_REG, PCO, OFFSET_CO
        .endif
       .endif
       .set LOAD_BANK, 1
      .else
       .if FIR_REMAIN == 0 && IIR_REMAIN == 1
        load    ST, LOAD_REG, PST, OFFSET_ST
        inc     1
       .else
        loadd   ST, LOAD_REG, PST, OFFSET_ST
        inc     2
       .endif
       .set LOAD_BANK, 0
      .endif
     .endif

        // Do interleaved multiplies, slightly delayed
     .if COUNTER >= 2
        multiply MUL_REG, COUNTER > 2, !\shift_0
      .set MUL_REG, (MUL_REG + 1) & 3
     .endif
     .set COUNTER, COUNTER + 1
    .endr

        // Post-process the result of the multiplies
    .if SPILL_SHIFT
        ldr     SHIFT, [sp, #9*4 + 0*4]
    .endif
    .if SPILL_MASK
        ldr     MASK, [sp, #9*4 + 1*4]
    .endif
        ldr     ST2, [PSAMP]
        subs    I, I, #1
    .if \shift_8
        mov     AC0, AC0, lsr #8
        orr     AC0, AC0, AC1, lsl #24
    .elseif !\shift_0
        rsb     ST3, SHIFT, #32
        mov     AC0, AC0, lsr SHIFT
        orr     AC0, AC0, AC1, lsl ST3
    .endif
    .if \mask_minus1
        add     ST3, ST2, AC0
    .else
        add     ST2, ST2, AC0
        and     ST3, ST2, MASK
        sub     ST2, ST3, AC0
    .endif
        str     ST3, [PST, #-4]!
        str     ST2, [PST, #4 * (MAX_BLOCKSIZE + MAX_FIR_ORDER)]
        str     ST3, [PSAMP], #4 * MAX_CHANNELS
        bne     01b
  .endif
        b       99f

  .if DEFINED_SHIFT
    .unreq SHIFT
  .endif
  .if DEFINED_MASK
    .unreq MASK
  .endif
.endm

.macro switch_on_fir_taps  mask_minus1, shift_0, shift_8, iir_taps
        ldr     pc, [pc, a3, LSL #2] // firorder is in range 0-(8-iir_taps)
        .word   0
        .word   70f
        .word   71f
        .word   72f
        .word   73f
        .word   74f
        .word   75f
 .if \iir_taps <= 2
        .word   76f
  .if \iir_taps <= 1
        .word   77f
   .if \iir_taps == 0
        .word   78f
   .endif
  .endif
 .endif
70:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 0
71:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 1
72:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 2
73:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 3
74:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 4
75:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 5
 .if \iir_taps <= 2
76:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 6
  .if \iir_taps <= 1
77:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 7
   .if \iir_taps == 0
78:     implement_filter  \mask_minus1, \shift_0, \shift_8, \iir_taps, 8
   .endif
  .endif
 .endif
.endm

.macro switch_on_iir_taps  mask_minus1, shift_0, shift_8
        ldr     pc, [pc, a4, LSL #2] // irorder is in range 0-3
        .word   0
        .word   60f
        .word   61f
        .word   62f
        .word   63f
60:     switch_on_fir_taps  \mask_minus1, \shift_0, \shift_8, 0
61:     switch_on_fir_taps  \mask_minus1, \shift_0, \shift_8, 1
62:     switch_on_fir_taps  \mask_minus1, \shift_0, \shift_8, 2
63:     switch_on_fir_taps  \mask_minus1, \shift_0, \shift_8, 3
.endm

/* void ff_mlp_filter_channel_arm(int32_t *state, const int32_t *coeff,
 *                                int firorder, int iirorder,
 *                                unsigned int filter_shift, int32_t mask,
 *                                int blocksize, int32_t *sample_buffer);
 */
function ff_mlp_filter_channel_arm, export=1
        push    {v1-fp,lr}
        add     v1, sp, #9*4 // point at arguments on stack
        ldm     v1, {ST0,ST1,I,PSAMP}
        cmp     ST1, #-1
        bne     30f
        movs    ST2, ST0, lsl #29 // shift is in range 0-15; we want to special-case 0 and 8
        bne     20f
        bcs     10f
        switch_on_iir_taps 1, 1, 0
10:     switch_on_iir_taps 1, 0, 1
20:     switch_on_iir_taps 1, 0, 0
30:     movs    ST2, ST0, lsl #29 // shift is in range 0-15; we want to special-case 0 and 8
        bne     50f
        bcs     40f
        switch_on_iir_taps 0, 1, 0
40:     switch_on_iir_taps 0, 0, 1
50:     switch_on_iir_taps 0, 0, 0
99:     pop     {v1-fp,pc}
endfunc

        .unreq  PST
        .unreq  PCO
        .unreq  AC0
        .unreq  AC1
        .unreq  CO0
        .unreq  CO1
        .unreq  CO2
        .unreq  CO3
        .unreq  ST0
        .unreq  ST1
        .unreq  ST2
        .unreq  ST3
        .unreq  I
        .unreq  PSAMP

/********************************************************************/

PSA     .req    a1 // samples
PCO     .req    a2 // coeffs
PBL     .req    a3 // bypassed_lsbs
INDEX   .req    a4
CO0     .req    v1
CO1     .req    v2
CO2     .req    v3
CO3     .req    v4
SA0     .req    v5
SA1     .req    v6
SA2     .req    sl
SA3     .req    fp
AC0     .req    ip
AC1     .req    lr
NOISE   .req    SA0
LSB     .req    SA1
DCH     .req    SA2 // dest_ch
MASK    .req    SA3

    // INDEX is used as follows:
    // bits 0..6   index2 (values up to 17, but wider so that we can
    //               add to index field without needing to mask)
    // bits 7..14  i (values up to 160)
    // bit 15      underflow detect for i
    // bits 25..31 (if access_unit_size_pow2 == 128)  \ index
    // bits 26..31 (if access_unit_size_pow2 == 64)   /

.macro implement_rematrix  shift, index_mask, mask_minus1, maxchan
    .if \maxchan == 1
        // We can just leave the coefficients in registers in this case
        ldrd    CO0, CO1, [PCO]
    .endif
1:
    .if \maxchan == 1
        ldrd    SA0, SA1, [PSA]
        smull   AC0, AC1, CO0, SA0
    .elseif \maxchan == 5
        ldr     CO0, [PCO, #0]
        ldr     SA0, [PSA, #0]
        ldr     CO1, [PCO, #4]
        ldr     SA1, [PSA, #4]
        ldrd    CO2, CO3, [PCO, #8]
        smull   AC0, AC1, CO0, SA0
        ldrd    SA2, SA3, [PSA, #8]
        smlal   AC0, AC1, CO1, SA1
        ldrd    CO0, CO1, [PCO, #16]
        smlal   AC0, AC1, CO2, SA2
        ldrd    SA0, SA1, [PSA, #16]
        smlal   AC0, AC1, CO3, SA3
        smlal   AC0, AC1, CO0, SA0
    .else // \maxchan == 7
        ldr     CO2, [PCO, #0]
        ldr     SA2, [PSA, #0]
        ldr     CO3, [PCO, #4]
        ldr     SA3, [PSA, #4]
        ldrd    CO0, CO1, [PCO, #8]
        smull   AC0, AC1, CO2, SA2
        ldrd    SA0, SA1, [PSA, #8]
        smlal   AC0, AC1, CO3, SA3
        ldrd    CO2, CO3, [PCO, #16]
        smlal   AC0, AC1, CO0, SA0
        ldrd    SA2, SA3, [PSA, #16]
        smlal   AC0, AC1, CO1, SA1
        ldrd    CO0, CO1, [PCO, #24]
        smlal   AC0, AC1, CO2, SA2
        ldrd    SA0, SA1, [PSA, #24]
        smlal   AC0, AC1, CO3, SA3
        smlal   AC0, AC1, CO0, SA0
    .endif
        ldm     sp, {NOISE, DCH, MASK}
        smlal   AC0, AC1, CO1, SA1
    .if \shift != 0
      .if \index_mask == 63
        add     NOISE, NOISE, INDEX, lsr #32-6
        ldrb    LSB, [PBL], #MAX_CHANNELS
        ldrsb   NOISE, [NOISE]
        add     INDEX, INDEX, INDEX, lsl #32-6
      .else // \index_mask == 127
        add     NOISE, NOISE, INDEX, lsr #32-7
        ldrb    LSB, [PBL], #MAX_CHANNELS
        ldrsb   NOISE, [NOISE]
        add     INDEX, INDEX, INDEX, lsl #32-7
      .endif
        sub     INDEX, INDEX, #1<<7
        adds    AC0, AC0, NOISE, lsl #\shift + 7
        adc     AC1, AC1, NOISE, asr #31
    .else
        ldrb    LSB, [PBL], #MAX_CHANNELS
        sub     INDEX, INDEX, #1<<7
    .endif
        add     PSA, PSA, #MAX_CHANNELS*4
        mov     AC0, AC0, lsr #14
        orr     AC0, AC0, AC1, lsl #18
    .if !\mask_minus1
        and     AC0, AC0, MASK
    .endif
        add     AC0, AC0, LSB
        tst     INDEX, #1<<15
        str     AC0, [PSA, DCH, lsl #2]  // DCH is precompensated for the early increment of PSA
        beq     1b
        b       98f
.endm

.macro switch_on_maxchan  shift, index_mask, mask_minus1
        cmp     v4, #5
        blo     51f
        beq     50f
        implement_rematrix  \shift, \index_mask, \mask_minus1, 7
50:     implement_rematrix  \shift, \index_mask, \mask_minus1, 5
51:     implement_rematrix  \shift, \index_mask, \mask_minus1, 1
.endm

.macro switch_on_mask  shift, index_mask
        cmp     sl, #-1
        bne     40f
        switch_on_maxchan  \shift, \index_mask, 1
40:     switch_on_maxchan  \shift, \index_mask, 0
.endm

.macro switch_on_au_size  shift
  .if \shift == 0
        switch_on_mask  \shift, undefined
  .else
        teq     v6, #64
        bne     30f
        orr     INDEX, INDEX, v1, lsl #32-6
        switch_on_mask  \shift, 63
30:     orr     INDEX, INDEX, v1, lsl #32-7
        switch_on_mask  \shift, 127
  .endif
.endm

/* void ff_mlp_rematrix_channel_arm(int32_t *samples,
 *                                  const int32_t *coeffs,
 *                                  const uint8_t *bypassed_lsbs,
 *                                  const int8_t *noise_buffer,
 *                                  int index,
 *                                  unsigned int dest_ch,
 *                                  uint16_t blockpos,
 *                                  unsigned int maxchan,
 *                                  int matrix_noise_shift,
 *                                  int access_unit_size_pow2,
 *                                  int32_t mask);
 */
function ff_mlp_rematrix_channel_arm, export=1
        push    {v1-fp,lr}
        add     v1, sp, #9*4 // point at arguments on stack
        ldm     v1, {v1-sl}
        teq     v4, #1
        teqne   v4, #5
        teqne   v4, #7
        bne     99f
        teq     v6, #64
        teqne   v6, #128
        bne     99f
        sub     v2, v2, #MAX_CHANNELS
        push    {a4,v2,sl}          // initialise NOISE,DCH,MASK; make sp dword-aligned
        movs    INDEX, v3, lsl #7
        beq     98f                 // just in case, do nothing if blockpos = 0
        subs    INDEX, INDEX, #1<<7 // offset by 1 so we borrow at the right time
        adc     lr, v1, v1          // calculate index2 (C was set by preceding subs)
        orr     INDEX, INDEX, lr
        // Switch on matrix_noise_shift: values 0 and 1 are
        // disproportionately common so do those in a form the branch
        // predictor can accelerate. Values can only go up to 15.
        cmp     v5, #1
        beq     11f
        blo     10f
        ldr     pc, [pc, v5, lsl #2]
        .word   0
        .word   0
        .word   0
        .word   12f
        .word   13f
        .word   14f
        .word   15f
        .word   16f
        .word   17f
        .word   18f
        .word   19f
        .word   20f
        .word   21f
        .word   22f
        .word   23f
        .word   24f
        .word   25f
10:     switch_on_au_size  0
11:     switch_on_au_size  1
12:     switch_on_au_size  2
13:     switch_on_au_size  3
14:     switch_on_au_size  4
15:     switch_on_au_size  5
16:     switch_on_au_size  6
17:     switch_on_au_size  7
18:     switch_on_au_size  8
19:     switch_on_au_size  9
20:     switch_on_au_size  10
21:     switch_on_au_size  11
22:     switch_on_au_size  12
23:     switch_on_au_size  13
24:     switch_on_au_size  14
25:     switch_on_au_size  15

98:     add     sp, sp, #3*4
        pop     {v1-fp,pc}
99:     // Can't handle these parameters, drop back to C
        pop     {v1-fp,lr}
        b       X(ff_mlp_rematrix_channel)
endfunc

        .unreq  PSA
        .unreq  PCO
        .unreq  PBL
        .unreq  INDEX
        .unreq  CO0
        .unreq  CO1
        .unreq  CO2
        .unreq  CO3
        .unreq  SA0
        .unreq  SA1
        .unreq  SA2
        .unreq  SA3
        .unreq  AC0
        .unreq  AC1
        .unreq  NOISE
        .unreq  LSB
        .unreq  DCH
        .unreq  MASK

/********************************************************************/

.macro decr_modulo var, by, modulus
 .set \var, \var - \by
 .if \var == 0
  .set \var, \modulus
 .endif
.endm

 .macro load_group1  size, channels, r0, r1, r2, r3, pointer_dead=0
  .if \size == 2
        ldrd    \r0, \r1, [IN], #(\size + 8 - \channels) * 4
  .else // size == 4
   .if IDX1 > 4 || \channels==8
        ldm     IN!, {\r0, \r1, \r2, \r3}
   .else
        ldm     IN, {\r0, \r1, \r2, \r3}
    .if !\pointer_dead
        add     IN, IN, #(4 + 8 - \channels) * 4
     .endif
   .endif
  .endif
        decr_modulo IDX1, \size, \channels
 .endm

 .macro load_group2  size, channels, r0, r1, r2, r3, pointer_dead=0
  .if \size == 2
   .if IDX1 > 2
        ldm     IN!, {\r2, \r3}
   .else
//A   .ifc \r2, ip
//A    .if \pointer_dead
//A       ldm     IN, {\r2, \r3}
//A    .else
//A       ldr     \r2, [IN], #4
//A       ldr     \r3, [IN], #(\size - 1 + 8 - \channels) * 4
//A    .endif
//A   .else
        ldrd    \r2, \r3, [IN], #(\size + 8 - \channels) * 4
//A   .endif
   .endif
  .endif
        decr_modulo IDX1, \size, \channels
 .endm

.macro implement_pack  inorder, channels, shift
.if \inorder
.ifc \shift, mixed

CHECK   .req    a1
IN      .req    a2
OUT     .req    a3
COUNT   .req    a4
DAT0    .req    v1
DAT1    .req    v2
DAT2    .req    v3
DAT3    .req    v4
SHIFT0  .req    v5
SHIFT1  .req    v6
SHIFT2  .req    sl
SHIFT3  .req    fp
SHIFT4  .req    ip
SHIFT5  .req    lr

 .macro output4words
  .set SIZE_GROUP1, IDX1
  .if SIZE_GROUP1 > 4
   .set SIZE_GROUP1, 4
  .endif
  .set SIZE_GROUP2, 4 - SIZE_GROUP1
        load_group1  SIZE_GROUP1, \channels, DAT0, DAT1, DAT2, DAT3
        load_group2  SIZE_GROUP2, \channels, DAT0, DAT1, DAT2, DAT3
   .if \channels == 2
        lsl     DAT0, SHIFT0
        lsl     DAT1, SHIFT1
        lsl     DAT2, SHIFT0
        lsl     DAT3, SHIFT1
   .elseif \channels == 6
    .if IDX2 == 6
        lsl     DAT0, SHIFT0
        lsl     DAT1, SHIFT1
        lsl     DAT2, SHIFT2
        lsl     DAT3, SHIFT3
    .elseif IDX2 == 2
        lsl     DAT0, SHIFT4
        lsl     DAT1, SHIFT5
        lsl     DAT2, SHIFT0
        lsl     DAT3, SHIFT1
    .else // IDX2 == 4
        lsl     DAT0, SHIFT2
        lsl     DAT1, SHIFT3
        lsl     DAT2, SHIFT4
        lsl     DAT3, SHIFT5
    .endif
   .elseif \channels == 8
    .if IDX2 == 8
        uxtb    SHIFT0, SHIFT4, ror #0
        uxtb    SHIFT1, SHIFT4, ror #8
        uxtb    SHIFT2, SHIFT4, ror #16
        uxtb    SHIFT3, SHIFT4, ror #24
    .else
        uxtb    SHIFT0, SHIFT5, ror #0
        uxtb    SHIFT1, SHIFT5, ror #8
        uxtb    SHIFT2, SHIFT5, ror #16
        uxtb    SHIFT3, SHIFT5, ror #24
    .endif
        lsl     DAT0, SHIFT0
        lsl     DAT1, SHIFT1
        lsl     DAT2, SHIFT2
        lsl     DAT3, SHIFT3
   .endif
        eor     CHECK, CHECK, DAT0, lsr #8 - (\channels - IDX2)
        eor     CHECK, CHECK, DAT1, lsr #7 - (\channels - IDX2)
   decr_modulo IDX2, 2, \channels
        eor     CHECK, CHECK, DAT2, lsr #8 - (\channels - IDX2)
        eor     CHECK, CHECK, DAT3, lsr #7 - (\channels - IDX2)
   decr_modulo IDX2, 2, \channels
        stm     OUT!, {DAT0 - DAT3}
 .endm

 .set WORDS_PER_LOOP, \channels  // calculate LCM (channels, 4)
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .set WORDS_PER_LOOP, WORDS_PER_LOOP * 4
 .set SAMPLES_PER_LOOP, WORDS_PER_LOOP / \channels

function ff_mlp_pack_output_inorder_\channels\()ch_mixedshift_arm, export=1
 .if SAMPLES_PER_LOOP > 1
        tst     COUNT, #SAMPLES_PER_LOOP - 1  // always seems to be in practice
        bne     X(ff_mlp_pack_output)         // but just in case, branch to C implementation if not
 .endif
        teq     COUNT, #0
        bxeq    lr
        push    {v1-v6,sl,fp,lr}
        ldr     SHIFT0, [sp, #(9+3)*4]  // get output_shift from stack
        ldr     SHIFT1, =0x08080808
        ldr     SHIFT4, [SHIFT0]
 .if \channels == 2
        uadd8   SHIFT4, SHIFT4, SHIFT1 // increase all shifts by 8
        uxtb    SHIFT0, SHIFT4, ror #0
        uxtb    SHIFT1, SHIFT4, ror #8
 .else
        ldr     SHIFT5, [SHIFT0, #4]
        uadd8   SHIFT4, SHIFT4, SHIFT1 // increase all shifts by 8
        uadd8   SHIFT5, SHIFT5, SHIFT1
  .if \channels == 6
        uxtb    SHIFT0, SHIFT4, ror #0
        uxtb    SHIFT1, SHIFT4, ror #8
        uxtb    SHIFT2, SHIFT4, ror #16
        uxtb    SHIFT3, SHIFT4, ror #24
        uxtb    SHIFT4, SHIFT5, ror #0
        uxtb    SHIFT5, SHIFT5, ror #8
  .endif
 .endif
 .set IDX1, \channels
 .set IDX2, \channels
0:
 .rept WORDS_PER_LOOP / 4
        output4words
 .endr
        subs    COUNT, COUNT, #SAMPLES_PER_LOOP
        bne     0b
        pop     {v1-v6,sl,fp,pc}
        .ltorg
endfunc
 .purgem output4words

        .unreq  CHECK
        .unreq  IN
        .unreq  OUT
        .unreq  COUNT
        .unreq  DAT0
        .unreq  DAT1
        .unreq  DAT2
        .unreq  DAT3
        .unreq  SHIFT0
        .unreq  SHIFT1
        .unreq  SHIFT2
        .unreq  SHIFT3
        .unreq  SHIFT4
        .unreq  SHIFT5

.else // not mixed

CHECK   .req    a1
IN      .req    a2
OUT     .req    a3
COUNT   .req    a4
DAT0    .req    v1
DAT1    .req    v2
DAT2    .req    v3
DAT3    .req    v4
DAT4    .req    v5
DAT5    .req    v6
DAT6    .req    sl // use these rather than the otherwise unused
DAT7    .req    fp // ip and lr so that we can load them usinf LDRD

 .macro output4words  tail, head, r0, r1, r2, r3, r4, r5, r6, r7, pointer_dead=0
  .if \head
   .set SIZE_GROUP1, IDX1
   .if SIZE_GROUP1 > 4
    .set SIZE_GROUP1, 4
   .endif
   .set SIZE_GROUP2, 4 - SIZE_GROUP1
        load_group1  SIZE_GROUP1, \channels, \r0, \r1, \r2, \r3, \pointer_dead
  .endif
  .if \tail
        eor     CHECK, CHECK, \r4, lsr #8 - (\channels - IDX2)
        eor     CHECK, CHECK, \r5, lsr #7 - (\channels - IDX2)
   decr_modulo IDX2, 2, \channels
  .endif
  .if \head
        load_group2  SIZE_GROUP2, \channels, \r0, \r1, \r2, \r3, \pointer_dead
  .endif
  .if \tail
        eor     CHECK, CHECK, \r6, lsr #8 - (\channels - IDX2)
        eor     CHECK, CHECK, \r7, lsr #7 - (\channels - IDX2)
   decr_modulo IDX2, 2, \channels
        stm     OUT!, {\r4, \r5, \r6, \r7}
  .endif
  .if \head
        lsl     \r0, #8 + \shift
        lsl     \r1, #8 + \shift
        lsl     \r2, #8 + \shift
        lsl     \r3, #8 + \shift
  .endif
 .endm

 .set WORDS_PER_LOOP, \channels  // calculate LCM (channels, 8)
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .set WORDS_PER_LOOP, WORDS_PER_LOOP * 8
 .set SAMPLES_PER_LOOP, WORDS_PER_LOOP / \channels

function ff_mlp_pack_output_inorder_\channels\()ch_\shift\()shift_arm, export=1
 .if SAMPLES_PER_LOOP > 1
        tst     COUNT, #SAMPLES_PER_LOOP - 1  // always seems to be in practice
        bne     X(ff_mlp_pack_output)         // but just in case, branch to C implementation if not
 .endif
        subs    COUNT, COUNT, #SAMPLES_PER_LOOP
        bxlo    lr
        push    {v1-v6,sl,fp,lr}
 .set IDX1, \channels
 .set IDX2, \channels
        output4words  0, 1, DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, DAT7
0:      beq     1f
 .rept WORDS_PER_LOOP / 8
        output4words  1, 1, DAT4, DAT5, DAT6, DAT7, DAT0, DAT1, DAT2, DAT3
        output4words  1, 1, DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, DAT7
 .endr
        subs    COUNT, COUNT, #SAMPLES_PER_LOOP
        bne     0b
1:
 .rept WORDS_PER_LOOP / 8 - 1
        output4words  1, 1, DAT4, DAT5, DAT6, DAT7, DAT0, DAT1, DAT2, DAT3
        output4words  1, 1, DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, DAT7
 .endr
        output4words  1, 1, DAT4, DAT5, DAT6, DAT7, DAT0, DAT1, DAT2, DAT3, pointer_dead=1
        output4words  1, 0, DAT0, DAT1, DAT2, DAT3, DAT4, DAT5, DAT6, DAT7
        pop     {v1-v6,sl,fp,pc}
endfunc
 .purgem output4words

        .unreq  CHECK
        .unreq  IN
        .unreq  OUT
        .unreq  COUNT
        .unreq  DAT0
        .unreq  DAT1
        .unreq  DAT2
        .unreq  DAT3
        .unreq  DAT4
        .unreq  DAT5
        .unreq  DAT6
        .unreq  DAT7

.endif // mixed
.else // not inorder
.ifc \shift, mixed

// This case not currently handled

.else // not mixed

CHECK   .req    a1
IN      .req    a2
OUT     .req    a3
COUNT   .req    a4
DAT0    .req    v1
DAT1    .req    v2
DAT2    .req    v3
DAT3    .req    v4
CHAN0   .req    v5
CHAN1   .req    v6
CHAN2   .req    sl
CHAN3   .req    fp
CHAN4   .req    ip
CHAN5   .req    lr

 .macro output4words
  .if \channels == 8
   .if IDX1 == 8
        uxtb    CHAN0, CHAN4, ror #0
        uxtb    CHAN1, CHAN4, ror #8
        uxtb    CHAN2, CHAN4, ror #16
        uxtb    CHAN3, CHAN4, ror #24
   .else
        uxtb    CHAN0, CHAN5, ror #0
        uxtb    CHAN1, CHAN5, ror #8
        uxtb    CHAN2, CHAN5, ror #16
        uxtb    CHAN3, CHAN5, ror #24
   .endif
        ldr     DAT0, [IN, CHAN0, lsl #2]
        ldr     DAT1, [IN, CHAN1, lsl #2]
        ldr     DAT2, [IN, CHAN2, lsl #2]
        ldr     DAT3, [IN, CHAN3, lsl #2]
   .if IDX1 == 4
        add     IN, IN, #8*4
   .endif
        decr_modulo IDX1, 4, \channels
  .else
   .set SIZE_GROUP1, IDX1
   .if SIZE_GROUP1 > 4
    .set SIZE_GROUP1, 4
   .endif
   .set SIZE_GROUP2, 4 - SIZE_GROUP1
   .if SIZE_GROUP1 == 2
        loadregoffsh2  DAT, 0, IN, CHAN, 0 + (\channels - IDX1)
        loadregoffsh2  DAT, 1, IN, CHAN, 1 + (\channels - IDX1)
        add     IN, IN, #8*4
   .else // SIZE_GROUP1 == 4
        loadregoffsh2  DAT, 0, IN, CHAN, 0 + (\channels - IDX1)
        loadregoffsh2  DAT, 1, IN, CHAN, 1 + (\channels - IDX1)
        loadregoffsh2  DAT, 2, IN, CHAN, 2 + (\channels - IDX1)
        loadregoffsh2  DAT, 3, IN, CHAN, 3 + (\channels - IDX1)
    .if IDX1 == 4
        add     IN, IN, #8*4
    .endif
   .endif
        decr_modulo IDX1, SIZE_GROUP1, \channels
   .if SIZE_GROUP2 == 2
        loadregoffsh2  DAT, 2, IN, CHAN, 0 + (\channels - IDX1)
        loadregoffsh2  DAT, 3, IN, CHAN, 1 + (\channels - IDX1)
    .if IDX1 == 2
        add     IN, IN, #8*4
    .endif
   .endif
        decr_modulo IDX1, SIZE_GROUP2, \channels
  .endif
  .if \channels == 8 // in this case we can corrupt CHAN0-3
        rsb     CHAN0, CHAN0, #8
        rsb     CHAN1, CHAN1, #8
        rsb     CHAN2, CHAN2, #8
        rsb     CHAN3, CHAN3, #8
        lsl     DAT0, #8 + \shift
        lsl     DAT1, #8 + \shift
        lsl     DAT2, #8 + \shift
        lsl     DAT3, #8 + \shift
        eor     CHECK, CHECK, DAT0, lsr CHAN0
        eor     CHECK, CHECK, DAT1, lsr CHAN1
        eor     CHECK, CHECK, DAT2, lsr CHAN2
        eor     CHECK, CHECK, DAT3, lsr CHAN3
  .else
   .if \shift != 0
        lsl     DAT0, #\shift
        lsl     DAT1, #\shift
        lsl     DAT2, #\shift
        lsl     DAT3, #\shift
   .endif
        bic     DAT0, DAT0, #0xff000000
        bic     DAT1, DAT1, #0xff000000
        bic     DAT2, DAT2, #0xff000000
        bic     DAT3, DAT3, #0xff000000
        eorlslreg CHECK, DAT0, CHAN, 0 + (\channels - IDX2)
        eorlslreg CHECK, DAT1, CHAN, 1 + (\channels - IDX2)
   decr_modulo IDX2, 2, \channels
        eorlslreg CHECK, DAT2, CHAN, 0 + (\channels - IDX2)
        eorlslreg CHECK, DAT3, CHAN, 1 + (\channels - IDX2)
   decr_modulo IDX2, 2, \channels
        lsl     DAT0, #8
        lsl     DAT1, #8
        lsl     DAT2, #8
        lsl     DAT3, #8
  .endif
        stm     OUT!, {DAT0 - DAT3}
 .endm

 .set WORDS_PER_LOOP, \channels  // calculate LCM (channels, 4)
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .if (WORDS_PER_LOOP % 2) == 0
  .set WORDS_PER_LOOP, WORDS_PER_LOOP / 2
 .endif
 .set WORDS_PER_LOOP, WORDS_PER_LOOP * 4
 .set SAMPLES_PER_LOOP, WORDS_PER_LOOP / \channels

function ff_mlp_pack_output_outoforder_\channels\()ch_\shift\()shift_arm, export=1
 .if SAMPLES_PER_LOOP > 1
        tst     COUNT, #SAMPLES_PER_LOOP - 1  // always seems to be in practice
        bne     X(ff_mlp_pack_output)         // but just in case, branch to C implementation if not
 .endif
        teq     COUNT, #0
        bxeq    lr
        push    {v1-v6,sl,fp,lr}
        ldr     CHAN0, [sp, #(9+2)*4]  // get ch_assign from stack
        ldr     CHAN4, [CHAN0]
 .if \channels == 2
        uxtb    CHAN0, CHAN4, ror #0
        uxtb    CHAN1, CHAN4, ror #8
 .else
        ldr     CHAN5, [CHAN0, #4]
  .if \channels == 6
        uxtb    CHAN0, CHAN4, ror #0
        uxtb    CHAN1, CHAN4, ror #8
        uxtb    CHAN2, CHAN4, ror #16
        uxtb    CHAN3, CHAN4, ror #24
        uxtb    CHAN4, CHAN5, ror #0
        uxtb    CHAN5, CHAN5, ror #8
  .endif
 .endif
 .set IDX1, \channels
 .set IDX2, \channels
0:
 .rept WORDS_PER_LOOP / 4
        output4words
 .endr
        subs    COUNT, COUNT, #SAMPLES_PER_LOOP
        bne     0b
        pop     {v1-v6,sl,fp,pc}
        .ltorg
endfunc
 .purgem output4words

        .unreq  CHECK
        .unreq  IN
        .unreq  OUT
        .unreq  COUNT
        .unreq  DAT0
        .unreq  DAT1
        .unreq  DAT2
        .unreq  DAT3
        .unreq  CHAN0
        .unreq  CHAN1
        .unreq  CHAN2
        .unreq  CHAN3
        .unreq  CHAN4
        .unreq  CHAN5

.endif // mixed
.endif // inorder
.endm // implement_pack

.macro pack_channels  inorder, channels
        implement_pack  \inorder, \channels, 0
        implement_pack  \inorder, \channels, 1
        implement_pack  \inorder, \channels, 2
        implement_pack  \inorder, \channels, 3
        implement_pack  \inorder, \channels, 4
        implement_pack  \inorder, \channels, 5
        implement_pack  \inorder, \channels, mixed
.endm

.macro pack_order  inorder
        pack_channels  \inorder, 2
        pack_channels  \inorder, 6
        pack_channels  \inorder, 8
.endm

        pack_order  0
        pack_order  1

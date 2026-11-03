#  Custom NDS ARM7/ARM9 BIOS replacement
#  Copyright (c) 2013, Gilead Kutnick
#  All rights reserved.
#  
#  Redistribution and use in source and binary forms, with or without
#   modification, are permitted provided that the following conditions are met:
#  
#  1) Redistributions of source code must retain the above copyright notice,
#     this list of conditions and the following disclaimer.
#  2) Redistributions in binary form must reproduce the above copyright notice,
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#  
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
#  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
#  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
#  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
#  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
#  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
#  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
#  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
#  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
#  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
#  POSSIBILITY OF SUCH DAMAGE.

// Upstream uses "axR" (retain) for its --gc-sections link; clang rejects
// changing the flags of .text, and build.sh does not gc sections.
.section .text

// Vector table

b boot_handler                        // 0x00 Reset
b unhandled_exception                 // 0x04 Undefined
b swi_handler                         // 0x08 SWI
b unhandled_exception                 // 0x0C Abort Prefetch
b unhandled_exception                 // 0x10 Abort Data
b unhandled_exception                 // 0x14 Reserved
b interrupt_handler                   // 0x18 IRQ
b unhandled_exception                 // 0x1C FIQ

#ifdef TARGET_ARM9
// ARM9 BIOS has a logo here (0x9C bytes), we don't want to include the logo
// but we'll leave a space for it.

.fill 0xe0, 0x1, 0x0
#endif

#ifdef TARGET_ARM7
// ARM7 BIOS has encryption stuff at 0x30, we don't want to include it but we'll
// leave a space for it.

.fill 0x1058, 0x1, 0x0
#endif


// Deliberate hangs. Every place where FreeBIOS stops on purpose is its own
// "b ." at a hang_* label (hang_dsi7_intrwait_if is a halt loop), so the PC
// of a frozen game names the cause.
// build.sh prints the address of every hang_* label per image, and the
// "Where FreeBIOS hangs on purpose" table in UPGRADE_PLAN.md lists them
// (check.sh verifies the table against the build).

// Reset vector. Not needed for emulators that HLE bootup. On the NDS7 an
// invalid SWI also ends up here (it jumps to 0).
boot_handler:
hang_reset:
  b hang_reset

// Undefined instruction, aborts and FIQ.
unhandled_exception:
hang_exception:
  b hang_exception


#ifdef FREEBIOS_EXPERIMENTAL
  // EXPERIMENTAL (safe choice): absent SWI numbers return with the
  // registers unchanged, on NDS and DSi.
  #define swi_absent swi_invalid_return
#elif defined(PLATFORM_TWL)
  // DSi: SWI numbers the BIOS does not list hang (GBATEK)
  #define swi_absent hang_swi_invalid
#else
  // NDS: SWI numbers the BIOS does not list jump to 0 (GBATEK)
  #define swi_absent swi_invalid_jump0
#endif

#ifdef TARGET_ARM7
  #define swi_label(function) .word function
  #define swi_label_arm7_only(function) swi_label(function)
#else
  #define swi_label_arm7_only(function) swi_label(swi_absent)
#endif

#ifdef TARGET_ARM9
  #define swi_label(function) .word (function + 0xFFFF0000)
  #define swi_label_arm9_only(function) swi_label(function)
#else
  #define swi_label_arm9_only(function) swi_label(swi_absent)
#endif

// SWI calling convention:
// Parameters are passed in via r0 - r3
// Called SWI can modify r0 - r3 (and return things here) and r4, r12, and r14.
// They can't modify anything else.

#define swi_comment     r12
#define saved_spsr      r4
#define modified_spsr   r4

swi_handler:
  // Save these as temporaries
  stmdb sp!, { r4, r12, lr }

  // get SPSR and enter system mode, interrupts on
  mrs saved_spsr, SPSR
  // This must be stacked and not just saved, because otherwise SWI won't
  // be reentrant, which can happen if you're waiting for interrupts and the
  // interrupt handler triggers the SWI.
  stmdb sp!, { saved_spsr }
  and modified_spsr, saved_spsr, #0x80
  orr modified_spsr, modified_spsr, #0x1F

  // Load comment from SWI instruction, which indicates which SWI
  // to use.
  ldrb swi_comment, [ lr, #-2 ]
  msr CPSR_fc, modified_spsr

  // We have to now save system-mode lr register as well
  stmdb sp!, { lr }

  // Numbers past the table (GBATEK "Invalid Functions"):
  //   DSi: 40h-FFh mirror 00h-3Fh, and the table covers 00h-3Fh.
  //   NDS9: 20h and up crash (they jump to garbage addresses).
  //   NDS7: 20h and 22h jump to 0 like the other absent numbers, 21h and
  //         23h and up crash (SWI test ROM, every number 20h-2Ah tested).
  // A crash cannot be reproduced without the official BIOS content, so it
  // becomes a deliberate hang (hang_swi_crash).
#ifdef PLATFORM_TWL
  and swi_comment, swi_comment, #0x3F
#else
  cmp swi_comment, #0x20
  bhs swi_past_table
#endif

  // Branch to SWI handler
  ldr pc, [ pc, swi_comment, lsl #2 ]
  nop

  // SWI table begins here
#ifdef PLATFORM_TWL
  swi_label(swi_absent)                                      // 00
  swi_label(swi_lz77_decompress_callback8)                   // 01
#else
  swi_label(swi_soft_reset)                                  // 00
  swi_label(swi_absent)                                      // 01
#endif
#ifdef PLATFORM_TWL
  swi_label(swi_lz77_decompress_vram)                        // 02
#else
  swi_label(swi_absent)                                      // 02
#endif
  swi_label(swi_wait_by_loop)                                // 03
  swi_label(swi_interrupt_wait)                              // 04
  swi_label(swi_vblank_interrupt_wait)                       // 05
  swi_label(swi_halt)                                        // 06
  swi_label_arm7_only(swi_stop)                              // 07
  swi_label_arm7_only(swi_sound_bias)                        // 08
  swi_label(swi_divide)                                      // 09
  swi_label(swi_absent)                                      // 0A
  swi_label(swi_cpu_set)                                     // 0B
  swi_label(swi_cpu_fast_set)                                // 0C
  swi_label(swi_sqrt)                                        // 0D
  swi_label(swi_get_crc16)                                   // 0E
#ifdef PLATFORM_TWL
  swi_label(swi_absent)                                      // 0F
#else
  swi_label(swi_is_debugger)                                 // 0F
#endif
  swi_label(swi_bit_unpack)                                  // 10
  swi_label(swi_lz77_decompress_wram)                        // 11
#ifdef PLATFORM_TWL
  swi_label(swi_ignored)                                     // 12
#else
  swi_label(swi_lz77_decompress_vram)                        // 12
#endif
  swi_label(swi_huffman_decompress)                          // 13
  swi_label(swi_runlength_decompress_wram)                   // 14
  swi_label(swi_runlength_decompress_vram)                   // 15
  swi_label_arm9_only(swi_diff_8bit_unfilter_wram)           // 16
  swi_label(swi_absent)                                      // 17
  swi_label_arm9_only(swi_diff_16bit_unfilter)               // 18
#ifdef PLATFORM_TWL
  swi_label(swi_lz77_decompress_vram)                        // 19
#else
  swi_label(swi_absent)                                      // 19
#endif
  swi_label_arm7_only(swi_get_sine_table)                    // 1A
  swi_label_arm7_only(swi_get_pitch_table)                   // 1B
  swi_label_arm7_only(swi_get_volume_table)                  // 1C
  swi_label_arm7_only(swi_get_boot_procs)                    // 1D
  swi_label(swi_absent)                                      // 1E
  swi_label(swi_custom_halt_post)                            // 1F
#ifdef PLATFORM_TWL
  // 20h-29h: RSA and SHA-1, not implemented (they return)
  .rept 10
  swi_label(swi_unimplemented)                               // 20-29
  .endr
  swi_label(swi_absent)                                      // 2A
  swi_label(swi_ignored)                                     // 2B
  .rept 0x14
  swi_label(swi_absent)                                      // 2C-3F
  .endr
#endif

// TWL 20h-29h (RSA, SHA-1) are not implemented and just return.
swi_unimplemented:
#ifdef FREEBIOS_EXPERIMENTAL
// EXPERIMENTAL: absent SWI numbers (and NDS numbers past the table) return.
swi_invalid_return:
swi_past_table:
#endif
// TWL 12h and 2Bh: "no function (ignored)" (GBATEK).
swi_ignored:

// SWI returns here
swi_complete:
  // Restore system mode lr
  ldmia sp!, { lr }

  // Go back to supervisor mode to get back to that stack
  mov modified_spsr, #0xD3
  msr CPSR_fc, modified_spsr

  // SPSR has to be restored because the transition to system mode broke it
  ldmia sp!, { saved_spsr }
  msr SPSR, saved_spsr

  // Restore stuff we saved
  ldmia sp!, { r4, r12, lr }

  // Return from exception handler
  movs pc, lr


#if defined(FREEBIOS_EXPERIMENTAL)
// (EXPERIMENTAL: no invalid-SWI hang or jump to 0, see swi_invalid_return)
#elif defined(PLATFORM_TWL)
// DSi: absent SWI numbers hang (GBATEK), the table points here.
hang_swi_invalid:
  b hang_swi_invalid
#else
// NDS: an absent SWI jumps to 0 (GBATEK): on the NDS9 that is the ITCM
// (a debug handler, if the game put one there; when the protection unit
// blocks execution at 0, as in normal SDK games, the jump gets a prefetch
// abort and ends in hang_exception), on the NDS7 the reset vector
// (hang_reset). As the SWI test ROM saw on the official BIOS, the
// jump is made in system mode with the condition flags clear and lr pointing
// into the BIOS; lr = swi_complete, so code at 0 that returns ends the SWI
// normally.
swi_invalid_jump0:
  msr CPSR_f, #0
  adr lr, swi_complete
  mov pc, #0

// Numbers past the NDS table: the NDS7 sends 20h and 22h to 0 as well; the
// rest crash on the official BIOS (a jump to a garbage address read from
// BIOS memory, GBATEK), which cannot be reproduced, so they hang here
// instead.
swi_past_table:
#ifdef TARGET_ARM7
  cmp swi_comment, #0x20
  cmpne swi_comment, #0x22
  beq swi_invalid_jump0
#endif
hang_swi_crash:
  b hang_swi_crash
#endif


padding_a:
  .word 0x0

swi_halt:
#ifdef TARGET_ARM7
  // GBATEK: NDS7 Halt keeps all registers, so only use r4 and r12 (both
  // restored by swi_complete)
  mov r12, #0x04000000
  mov r4, #0x80
  strb r4, [ r12, #0x301 ]
#else
  // GBATEK: NDS9 Halt destroys r0. With IME=0 the CP15 halt never wakes
  // up (GBATEK), so the PC then stays here (halt_arm9_cp15).
  mov r0, #0x0
halt_arm9_cp15:
  mcr p15, 0, r0, cr7, cr0, 4
#endif
  b swi_complete

swi_wait_by_loop:
 0:
  subs r0, r0, #1
  bgt 0b

  b swi_complete

#define check_immediately r0
#define irq_wait_mask     r1

#ifdef TARGET_ARM7
  #ifdef PLATFORM_TWL
    #define irq_wait_aux_mask r2
  #endif
  #define irq_flag_base   r3
#else
  #define irq_flag_base   r2
#endif

#define io_base           r3
#define const_0x1         r12

// The flags are written back before const_0x1 is needed, so both share r12.
// This keeps r0 and r1, so VBlankIntrWait returns r0 = r1 = 1 (GBATEK: it
// sets them and then runs IntrWait).
#define irq_flags         r12

interrupt_check:
#ifdef TARGET_ARM9
  // Get DTCM base
  mrc p15, 0, irq_flag_base, cr9, cr1, 0
  bic irq_flag_base, irq_flag_base, #0xFF

  // Software IRQ flag is at DTCM[0x3FF8]
  add irq_flag_base, irq_flag_base, #0x4000
#endif

  // Set IME (0x04000208) to 0 before reading the flags, so an IRQ cannot
  // set a bit between the read and the write-back
  str io_base, [ io_base, #0x208 ]

  // Load software IRQ flag
  ldr irq_flags, [ irq_flag_base, #-8 ]

  // Check if IRQs were risen, to see if the loop can exit
  tst irq_wait_mask, irq_flags

  // Clear IRQs that were risen and write back
  bic irq_flags, irq_flags, irq_wait_mask
  str irq_flags, [ irq_flag_base, #-8 ]

  mov const_0x1, #0x1
  // Set IME (0x04000208) to 1 and return
  str const_0x1, [ io_base, #0x208 ]
  bx lr

#if defined(TARGET_ARM7) && defined(PLATFORM_TWL)
interrupt_check_aux:
  // Set IME (0x04000208) to 0 before reading the flags, so an IRQ cannot
  // set a bit between the read and the write-back
  str io_base, [ io_base, #0x208 ]

  // Load software IRQ flag
  ldr irq_flags, [ irq_flag_base, #-64 ]

  // Check if IRQs were risen, to see if the loop can exit
  tst irq_wait_aux_mask, irq_flags

  // Clear IRQs that were risen and write back
  bic irq_flags, irq_flags, irq_wait_aux_mask
  str irq_flags, [ irq_flag_base, #-64 ]

  mov const_0x1, #0x1
  // Set IME (0x04000208) to 1 and return
  str const_0x1, [ io_base, #0x208 ]
  bx lr
#endif

#define halt_value        r12

// Check and acknowledge the requested IRQ flags; Z is clear if one was set.
// The DSi ARM7 takes the IE2/IF2 flags in irq_wait_aux_mask (GBATEK: r2,
// BIOS flags at 0380FFC0h). GBATEK: the DSi7 BIOS "ignores the old IF
// interrupts, and works only with new IF2 ones", so in the accurate TWL
// ARM7 build only the IF2 flags end the wait. The requested IF flags (r1)
// are still cleared in the BIOS IF check bits, but they never end the wait
// (official DSi BIOS, SWI test ROM: an enabled timer 2 IRQ with r1=T2 does
// not end a wait for an AES IF2 IRQ, and its check bit is cleared).
// EXPERIMENTAL: either word ends the wait; r4 (stacked by the SWI entry)
// keeps the first result.
.macro check_irq_flags
#if defined(TARGET_ARM7) && defined(PLATFORM_TWL) && !defined(FREEBIOS_EXPERIMENTAL)
  bl interrupt_check
  bl interrupt_check_aux
#else
  bl interrupt_check
#if defined(TARGET_ARM7) && defined(PLATFORM_TWL)
  movne r4, #1
  moveq r4, #0
  bl interrupt_check_aux
  teqeq r4, #0
#endif
#endif
.endm

swi_vblank_interrupt_wait:
  // Check immediately for VBLANK interrupt
  mov check_immediately, #1
  mov irq_wait_mask, #1
#if defined(TARGET_ARM7) && defined(PLATFORM_TWL)
  mov irq_wait_aux_mask, #0
#endif

  // Fall through

swi_interrupt_wait:
  mov io_base, #0x4000000
#if defined(TARGET_ARM7) && defined(PLATFORM_TWL) && !defined(FREEBIOS_EXPERIMENTAL)
  // Since the IF flags are ignored (see check_irq_flags), a wait with no IF2
  // bits (r2 = 0, which includes VBlankIntrWait) can never end, as on the
  // official DSi BIOS (SWI test ROM): dsi7_intrwait_forever.
  // (EXPERIMENTAL: returns on the IF bits as well.)
  cmp irq_wait_aux_mask, #0
  beq dsi7_intrwait_forever
#endif

  // r0 != 0: discard the requested flags, then wait for a new one.
  // r0 == 0, ARM7: return at once if a requested flag is already set.
  // r0 == 0, ARM9: always halt at least once, even if a requested flag is
  // already set, as the real NDS9 BIOS does (GBATEK).
  // r0 and r1 are not changed.
  cmp check_immediately, #0
#ifdef TARGET_ARM7
  beq swi_interrupt_check_first
  check_irq_flags
#else
  blne interrupt_check
#endif

  // Perform this loop until the interrupt is risen
 0:
#ifdef TARGET_ARM9
  // Halt ARM9 via coprocessor instruction
  mov halt_value, #0
  mcr p15, 0, halt_value, cr7, cr0, 4
#else
  mov halt_value, #0x80
  // Set HALTCNT to 0x80
  strb halt_value, [ io_base, #0x301 ]
#endif

  check_irq_flags
  beq 0b

  b swi_complete

#ifdef TARGET_ARM7
swi_interrupt_check_first:
  // Check for IRQ (and acknowledge it)
  check_irq_flags
  // If set exit.
  bne swi_complete

  // If not wait for interrupt.
  b 0b
#endif

#if defined(TARGET_ARM7) && defined(PLATFORM_TWL) && !defined(FREEBIOS_EXPERIMENTAL)
// A DSi7 wait that never ends: IME=1 as IntrWait sets it (GBATEK), then
// halt forever, so the game's IRQ handlers keep running. The PC stays in
// the halt loop at hang_dsi7_intrwait_if.
dsi7_intrwait_forever:
  mov const_0x1, #0x1
  str const_0x1, [ io_base, #0x208 ]
hang_dsi7_intrwait_if:
  mov halt_value, #0x80
  strb halt_value, [ io_base, #0x301 ]
  b hang_dsi7_intrwait_if
#endif


#ifdef TARGET_ARM7

swi_stop:
  mov r0, #0x04000000
  mov r1, #0xC0
  strb r1, [ r0, #0x301 ]
  b swi_complete

// SoundBias (GBATEK): r0 = 0 moves the level (SOUNDBIAS bits 0-9) to 000h,
// any other value to 200h, one step at a time with r1 as the delay count
// between steps; the upper bits of the register are kept. GBATEK gives no
// unit for the delay, so it is a plain countdown loop of r1 iterations
// (r1 = 0: no delay).
#define bias_target     r0
#define bias_delay      r1
#define bias_value      r2
#define bias_ptr        r3
#define bias_tmp        r12

swi_sound_bias:
  mov bias_ptr, #0x4000000
  add bias_ptr, bias_ptr, #0x500
  cmp bias_target, #0
  movne bias_target, #0x200

 0:
  ldrh bias_value, [ bias_ptr, #4 ]
  bic bias_tmp, bias_value, #0xFC00
  cmp bias_tmp, bias_target
  beq swi_complete
  // The level stays within 000h-3FFh, so +-1 never touches bits 10-15
  addlo bias_value, bias_value, #1
  subhi bias_value, bias_value, #1
  strh bias_value, [ bias_ptr, #4 ]

  movs bias_tmp, bias_delay
 1:
  subsne bias_tmp, bias_tmp, #1
  bne 1b
  b 0b

#undef bias_target
#undef bias_delay
#undef bias_value
#undef bias_ptr
#undef bias_tmp

#endif


#define numerator           r0
#define denominator         r1
#define accumulator         r2
#define current_bit         r3

#define numerator_signed    r12
#define denominator_signed  r3

#define sign_flip           r4

#define result              r0
#define remainder           r1
#define result_abs          r3

swi_divide:
  // Division by zero: GBATEK says the BIOS is usually caught in an endless
  // loop. As on the official BIOS (SWI test ROM), 0/0 returns r0 = 1,
  // r1 = 0, r3 = 1 and a non-zero numerator hangs (hang_div_by_zero).
  cmp denominator, #0
  beq div_by_zero

  // Set if numerator is signed, and abs numerator
  ands numerator_signed, numerator, #0x80000000
  rsbmi numerator, numerator, #0

  // Same with denominator
  ands denominator_signed, denominator, #0x80000000
  rsbmi denominator, denominator, #0

  // Gets set if sign(numerator) != sign(denominator)
  eor sign_flip, numerator_signed, denominator_signed

  mov accumulator, #0
  mov current_bit, #1

  // This moves out the current bit to the MSB of the denominator,
  // and aligns the denominator up to the same bit-length as the
  // numerator. Stop once bit 31 is set so a numerator of 0x80000000
  // (abs(INT_MIN)) cannot shift the denominator out to 0 and loop forever.
 0:
  tst denominator, #0x80000000
  bne 1f
  cmp denominator, numerator
  movls denominator, denominator, lsl #1
  movls current_bit, current_bit, lsl #1
  bls 0b

  // Basically the grade-school algorithm, for unsigned integers in binary
 1:
  cmp numerator, denominator
  subcs numerator, numerator, denominator
  orrcs accumulator, accumulator, current_bit
  movs current_bit, current_bit, lsr #1
  movcc denominator, denominator, lsr #1
  bcc 1b

  mov remainder, numerator
  mov result_abs, accumulator
  mov result, accumulator

  tst sign_flip, #0x80000000
  rsbmi result, result, #0

  // The remainder takes the sign of the numerator
  tst numerator_signed, #0x80000000
  rsbmi remainder, remainder, #0

  b swi_complete

 div_by_zero:
  cmp numerator, #0
#ifdef FREEBIOS_EXPERIMENTAL
  // EXPERIMENTAL (safe choice): x/0 returns r0 = 0, r1 = x, r3 = 0
  movne remainder, numerator
  movne result, #0
  movne result_abs, #0
  bne swi_complete
#else
  bne hang_div_by_zero
#endif
  mov result, #1
  mov remainder, #0
  mov result_abs, #1
  b swi_complete

#ifndef FREEBIOS_EXPERIMENTAL
hang_div_by_zero:
  b hang_div_by_zero
#endif


#define source        r0
#define dest          r1
#define copy_control  r2
#define value         r3

#define length        r12

swi_cpu_set:
  // Only take 21 bits for length. A count of 0 writes nothing.
  bic length, copy_control, #0xFF000000
  bics length, length, #0x00E00000
  beq swi_complete

  tst copy_control, #(1 << 26)
  bne copy_32bit

 copy_16bit:
  tst copy_control, #(1 << 24)
  bic source, #0x1
  bic dest, #0x1
  bne set_16bit

 0:
  ldrh value, [ source ], #2
  subs length, length, #1
  strh value, [ dest ], #2
  bne 0b

  b swi_complete

 set_16bit:
  ldrh value, [ source ]

 0:
  strh value, [ dest ], #2
  subs length, length, #1
  bne 0b

  b swi_complete

 copy_32bit:
  tst copy_control, #(1 << 24)
  bic source, #0x3
  bic dest, #0x3
  bne set_32bit

 0:
  ldr value, [ source ], #4
  subs length, length, #1
  str value, [ dest ], #4
  bne 0b

  b swi_complete

 set_32bit:
  ldr value, [ source ]

 0:
  str value, [ dest ], #4
  subs length, length, #1
  bne 0b

  b swi_complete


// Length is in words (bits 0-20). Only the GBA rounds it up to a multiple of
// 8 words; GBATEK: on the NDS the length may be any number of words, and the
// words left after the 32-byte blocks are processed as 4-byte blocks. So
// count/8 blocks of 8 words are moved with ldm/stm, then the rest one word
// at a time (GBATEK's slow-path bug of the real NDS BIOS only costs time and
// is not copied). A count of 0 writes nothing. r5-r10 are stacked for the
// burst.
#define fast_count    r12

swi_cpu_fast_set:
  // Only take 21 bits for length.
  bic fast_count, copy_control, #0xFF000000
  bic fast_count, fast_count, #0x00E00000

  stmdb sp!, { r5 - r10 }
  tst copy_control, #(1 << 24)
  bic source, #0x3
  bic dest, #0x3
  bne fast_set_32bit

  subs fast_count, fast_count, #8
  blo 1f
 0:
  ldmia source!, { r3 - r10 }
  stmia dest!, { r3 - r10 }
  subs fast_count, fast_count, #8
  bhs 0b

  // 0-7 words left
 1:
  adds fast_count, fast_count, #8
  beq 9f
 2:
  ldr r4, [ source ], #4
  str r4, [ dest ], #4
  subs fast_count, fast_count, #1
  bne 2b

 9:
  ldmia sp!, { r5 - r10 }
  b swi_complete

 fast_set_32bit:
  ldr r3, [ source ]
  mov r4, r3
  mov r5, r3
  mov r6, r3
  mov r7, r3
  mov r8, r3
  mov r9, r3
  mov r10, r3

  subs fast_count, fast_count, #8
  blo 1f
 0:
  stmia dest!, { r3 - r10 }
  subs fast_count, fast_count, #8
  bhs 0b

 1:
  adds fast_count, fast_count, #8
  beq 9b
 2:
  str r3, [ dest ], #4
  subs fast_count, fast_count, #1
  bne 2b
  b 9b

#undef fast_count


#undef remainder

#define sqrt_value      r0
#define remainder       r0
#define result          r0

#define square_index    r1
#define root            r2
#define root_check      r3

// r0 is an unsigned 32-bit value (GBATEK: Sqrt(FFFFFFFFh) = FFFFh), so the
// compare is unsigned.
swi_sqrt:
  mov square_index, #0x40000000
  mov root, #0x0

 0:
  orr root_check, square_index, root
  cmp remainder, root_check
  subhs remainder, remainder, root_check
  mov root, root, lsr #1
  orrhs root, root, square_index
  movs square_index, square_index, lsr #2
  bne 0b

  mov result, root
  b swi_complete

 
#undef current_value

#define crc_value       r0
#define crc_ptr         r1
#define crc_length      r2

#define current_value   r3
#define const_0x1E      r4
#define crc_index       r5
#define crc_table_ptr   r14

#define crc_lookup      r12

#define crc_nibble(shift)                                                     ;\
  and crc_index, const_0x1E, crc_value, lsl #1                                ;\
  ldrh crc_lookup, [ crc_table_ptr, crc_index ]                               ;\
  mov crc_value, crc_value, lsr #4                                            ;\
  eor crc_value, crc_value, crc_lookup                                        ;\
                                                                              ;\
  and crc_index, const_0x1E, current_value, shift                             ;\
  ldrh crc_lookup, [ crc_table_ptr, crc_index ]                               ;\
  eor crc_value, crc_value, crc_lookup                                        ;\

crc_table:
	.hword 0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401
  .hword 0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400

swi_get_crc16:
  stmdb sp!, { r5 }
  mov const_0x1E, #0x1E
  adr crc_table_ptr, crc_table

  bic crc_value, crc_value, #0xFF000000
  bic crc_value, crc_value, #0x00FF0000

  movs crc_length, crc_length, lsr #1
  beq 1f

 0:
  ldrh current_value, [ crc_ptr ], #2

  crc_nibble(lsl #1)
  crc_nibble(lsr #3)
  crc_nibble(lsr #7)
  crc_nibble(lsr #11)

  subs crc_length, crc_length, #1
  bne 0b

 1:
  ldmia sp!, { r5 }
  b swi_complete


// IsDebugger: 0 = normal 4MB console. GBATEK says it destroys the halfword
// at 27FFFF8h (NDS9) / 27FFFFAh (NDS7); the official BIOS leaves 0 there.
swi_is_debugger:
  mov r0, #0
  mov r12, #0x02800000
#ifdef TARGET_ARM9
  strh r0, [ r12, #-8 ]
#else
  strh r0, [ r12, #-6 ]
#endif
  b swi_complete


#undef length

#define source              r0
#define dest                r1
#define unpack_data         r2
#define length              r3

#define source_bit_width    r12
#define dest_bit_width      r14
#define data_offset         r4
#define offset_zero         r2
#define source_mask         r5
#define source_bit_buffer   r6
#define dest_bit_buffer     r7
#define dest_bits_loaded    r8
#define source_value        r9

swi_bit_unpack:
  stmdb sp!, { r5 - r9 }

  ldrh length, [ unpack_data, #0 ]
  ldrb source_bit_width, [ unpack_data, #2 ]
  ldrb dest_bit_width, [ unpack_data, #3 ]
  ldr data_offset, [ unpack_data, #4 ]

  // A source length of 0 writes nothing
  cmp length, #0
  beq 1f

  // TODO: Check that bits are <= 32 and a power of 2?
  mov offset_zero, data_offset, lsr #31
  bic data_offset, data_offset, #0x80000000

  mov source_mask, #0x1
  mov source_mask, source_mask, lsl source_bit_width
  sub source_mask, source_mask, #1

  mov source_bit_buffer, #0x1
  mov dest_bit_buffer, #0
  mov dest_bits_loaded, #0

  cmp source_bit_width, #1
  moveq length, length, lsl #3

  cmp source_bit_width, #2
  moveq length, length, lsl #2

  cmp source_bit_width, #4
  moveq length, length, lsl #1

 0:
  // Reload bit buffer if necessary
  cmp source_bit_buffer, #0x1
  ldrbeq source_bit_buffer, [ source ], #1
  orreq source_bit_buffer, source_bit_buffer, #0x100

  // Get next from source
  and source_value, source_bit_buffer, source_mask
  mov source_bit_buffer, source_bit_buffer, lsr source_bit_width

  cmp source_value, #0
  tsteq offset_zero, #0x1
  addne source_value, source_value, data_offset
  
  // Put it on dest buffer
  orr dest_bit_buffer, dest_bit_buffer, source_value, lsl dest_bits_loaded
  add dest_bits_loaded, dest_bits_loaded, dest_bit_width

  // If dest buffer is full write out
  cmp dest_bits_loaded, #32
  streq dest_bit_buffer, [ dest ], #4
  moveq dest_bit_buffer, #0
  moveq dest_bits_loaded, #0

  subs length, length, #1
  bne 0b

 1:
  ldmia sp!, { r5 - r9 }
  b swi_complete


#undef length

// Decompressors. LZ77 and RLE work a block at a time: a block that runs past
// the size in the header is still written in full (as the official BIOS
// does: a run of 10 with size 5 writes 10 bytes), and the routine stops once
// the signed remaining size is 0 or less. A size of 0 writes nothing.

#define source            r0
#define dest              r1
#define length            r2
#define header            r3

#define lz77_control      r3
#define lz77_value        r4
#define lz77_value_b      r14
#define window_length     r14
#define window_offset     r12
#define window_ptr        r12

swi_lz77_decompress_wram:
  ldr header, [ source ], #4
  movs length, header, lsr #8
  beq swi_complete

 0:
  ldrb lz77_control, [ source ], #1
  // This will hit the MSB after 8 iterations
  orr lz77_control, lz77_control, #(1 << 23)

 1:
  tst lz77_control, #0x80
  beq 2f

  // Load a window of data that was loaded before

  // Read 16-bit unaligned value
  ldrb lz77_value, [ source ], #1
  ldrb lz77_value_b, [ source ], #1
  orr lz77_value, lz77_value_b, lz77_value, lsl #8

  // Get length and offset from lz77 value
  mov window_length, lz77_value, lsr #12
  bic window_offset, lz77_value, #0xF000

  add window_length, window_length, #3
  sub window_ptr, dest, window_offset
  sub window_ptr, window_ptr, #1
  sub length, length, window_length

 3:
  ldrb lz77_value, [ window_ptr ], #1
  subs window_length, window_length, #1
  strb lz77_value, [ dest ], #1
  bne 3b
  b 4f

  // Load a single value
 2:
  ldrb lz77_value, [ source ], #1
  sub length, length, #1
  strb lz77_value, [ dest ], #1

 4:
  cmp length, #0
  ble swi_complete
  movs lz77_control, lz77_control, lsl #1
  bpl 1b
  b 0b

#undef lz77_control
#undef lz77_value
#undef lz77_value_b
#undef window_length
#undef window_offset
#undef window_ptr


// Decompression callbacks (GBATEK "NDS/DSi Decompression Callbacks"), used
// by SWI 12h (LZ77), 13h (Huffman) and 15h (RLE); in the TWL build also by
// the LZ77 entries 01h (8-bit writes), 02h and 19h.
// r3 points to five function pointers: Open, Close (optional, 0 = none),
// Get8, Get16 (not used) and Get32 (Huffman only); each may be ARM or Thumb
// (address bit 0). Open(r0 = source, r1 = dest, r2 = user parameter)
// returns the header word, or a negative error code that aborts before
// anything is written. The data is then read through Get8/Get32, each
// called with the current source address in r0; that address starts after
// the 4-byte header and moves on by 1 or 4 per call. At the end Close is
// called (if present) with the current source address; a negative result is
// returned in r0, otherwise r0 = the decompressed size from the header. The
// official BIOS also calls Close after a failed Open (with the original
// source address); then the Open error is returned.
// Callbacks follow the ARM calling convention and may change r0-r3, r12 and
// lr, so the state lives in r4-r11 (r5-r11 are stacked here, r4 by the SWI
// entry): r4 current source, r5 dest, r6 remaining size, r7 callbacks,
// r8 user parameter (Huffman temp buffer) or buffered VRAM byte, r9 header
// (or the Open error), r10 per-format state, r11 return address of
// cb_read8/cb_read32.
// The format routine starts right after "bl cb_decompress" in each SWI and
// ends with a branch to cb_finish.

// Call the function at the address in reg (bit 0 set = Thumb)
.macro call_reg reg
#ifdef TARGET_ARM9
  blx \reg
#else
  mov lr, pc
  bx \reg
#endif
.endm

cb_decompress:
  stmdb sp!, { r5 - r11 }
  mov r4, r0
  mov r5, r1
  mov r7, r3
  mov r8, r2
  mov r10, lr

  // Open(source, dest, user parameter): r0-r2 are still the SWI arguments
  ldr r12, [ r3 ]
  call_reg r12
  movs r9, r0
  bmi cb_finish

  add r4, r4, #4
  mov r6, r9, lsr #8
  bx r10

cb_finish:
  ldr r12, [ r7, #4 ]
  movs r6, r12
  beq 1f
  mov r0, r4
  call_reg r12
  mov r6, r0
 1:
  // r0 = size, or the Close error, or the Open error
  mov r0, r9, lsr #8
  cmp r6, #0
  movlt r0, r6
  cmp r9, #0
  movlt r0, r9
  ldmia sp!, { r5 - r11 }
  b swi_complete

// r0 = Get8(current source), source + 1
cb_read8:
  mov r11, lr
  mov r0, r4
  add r4, r4, #1
  ldr r12, [ r7, #8 ]
  call_reg r12
  and r0, r0, #0xFF
  bx r11

// r0 = Get32(current source), source + 4
cb_read32:
  mov r11, lr
  mov r0, r4
  add r4, r4, #4
  ldr r12, [ r7, #16 ]
  call_reg r12
  bx r11

// Store the byte in r0 at r5 with halfword writes only (VRAM ignores 8-bit
// writes): a byte for an even address is kept in r8 and stored together
// with the following odd byte. An output that ends on an even address
// leaves that last byte unwritten (as the official BIOS does). Uses r1.
.macro vram_put_byte
  tst r5, #1
  moveq r8, r0
  orrne r1, r8, r0, lsl #8
  strhne r1, [ r5, #-1 ]
  add r5, r5, #1
.endm


// Store the byte in r0 at r5 with an 8-bit write (TWL SWI 01h).
.macro byte_put_8bit
  strb r0, [ r5 ], #1
.endm

// LZ77 by callback (GBATEK LZ77UnCompReadByCallback...), same format as the
// WRAM routine. "put" is the macro that stores the byte in r0 at r5 and
// moves r5 on (r1 is free for it); the window is read back from the
// destination.
.macro lz77_callback_body put
  cmp r6, #0
  ble cb_finish

 0:
  bl cb_read8
  // This will hit the MSB after 8 iterations
  orr r10, r0, #(1 << 23)

 1:
  tst r10, #0x80
  beq 2f

  // Window block: 16-bit value, high byte first
  bl cb_read8
  str r0, [ sp, #-4 ]!
  bl cb_read8
  ldr r1, [ sp ], #4
  orr r0, r0, r1, lsl #8

  // r2 = length, r3 = window pointer
  mov r2, r0, lsr #12
  add r2, r2, #3
  bic r0, r0, #0xF000
  sub r3, r5, r0
  sub r3, r3, #1
  sub r6, r6, r2

 3:
  ldrb r0, [ r3 ], #1
  \put
  subs r2, r2, #1
  bne 3b
  b 4f

  // Single byte
 2:
  bl cb_read8
  \put
  sub r6, r6, #1

 4:
  cmp r6, #0
  ble cb_finish
  movs r10, r10, lsl #1
  bpl 1b
  b 0b
.endm

// LZ77UnCompReadByCallbackWrite16bit (NTR 12h, TWL 02h/19h). GBATEK wants a
// halfword-aligned destination; an odd one is aligned down, as RLE does. A
// displacement of 0 does not work here (GBATEK), since the byte it refers
// to may still be buffered in r8.
swi_lz77_decompress_vram:
  bl cb_decompress
  bic r5, r5, #1
  lz77_callback_body vram_put_byte

#ifdef PLATFORM_TWL
// LZ77UnCompReadByCallbackWrite8bit (TWL 01h): byte writes, so a
// displacement of 0 works.
swi_lz77_decompress_callback8:
  bl cb_decompress
  lz77_callback_body byte_put_8bit
#endif


// HuffUnCompReadByCallback (GBATEK HuffUnComp).
// Header: bits 0-3 data size in bits (4 or 8), bits 4-7 type (2), bits 8-31
// decompressed size in bytes. Then the tree: a tree-size byte (table length
// (byte+1)*2, counted from that byte), the root node, and the rest of the
// nodes. The table is copied with Get8 into the temp buffer (r2 of the SWI,
// 200h bytes) and the tree is walked there. Node: bits 0-5 offset, bit 6
// node1 end flag, bit 7 node0 end flag; the children of a node at A are at
// (A & ~1) + 2 + offset*2 (+1 for node1). The bitstream follows the tree as
// 32-bit words read with Get32, MSB first.
// Output is written in 32-bit units filled LSB first. A word is only written
// once 32 bits are collected, and the remaining size is decreased by 4 per
// word, so a size that is not a multiple of 4 is rounded up to the next word
// (the stream has to carry the padding symbols, as with the real BIOS). A size
// of 0, or a data size of 0, writes nothing (the tree is still copied).
// Registers in the decode loop: r0 bits, r1 bits left in r0, r2 node
// address, r3 node, r8 root, r10 output word, r12 bits left to fill in r10,
// lr scratch; r2 and r12 are stacked around Get32.

swi_huffman_decompress:
  bl cb_decompress

  // Copy the tree table, the tree-size byte first
  mov r10, #0
 0:
  bl cb_read8
  strb r0, [ r8, r10 ]
  add r10, r10, #1
  ldrb r0, [ r8 ]
  add r0, r0, #1
  cmp r10, r0, lsl #1
  blo 0b

  ands r0, r9, #0xF
  cmpne r6, #0
  beq cb_finish

  add r8, r8, #1
  mov r2, r8
  mov r12, #32

  // Load the next 32 bits of the stream
 0:
  stmdb sp!, { r2, r12 }
  bl cb_read32
  ldmia sp!, { r2, r12 }
  ldrb r3, [ r2 ]
  mov r1, #32

  // Step to the child picked by the next bit
 1:
  bic r2, r2, #1
  and lr, r3, #0x3F
  add r2, r2, lr, lsl #1
  add r2, r2, #2
  movs r0, r0, lsl #1
  addcs r2, r2, #1
  // Z is cleared if the child taken is a data node
  tstcs r3, #0x40
  tstcc r3, #0x80
  ldrb r3, [ r2 ]
  beq 2f

  // Data node: shift the symbol into the top of the output word
  and lr, r9, #0xF
  mov r10, r10, lsr lr
  rsb lr, lr, #32
  orr r10, r10, r3, lsl lr
  rsb lr, lr, #32
  subs r12, r12, lr
  bgt 3f

  str r10, [ r5 ], #4
  subs r6, r6, #4
  ble cb_finish
  mov r12, #32

  // Back to the root for the next symbol
 3:
  mov r2, r8
  ldrb r3, [ r8 ]

 2:
  subs r1, r1, #1
  bne 1b
  b 0b


// RLUnCompReadByCallbackWrite16bit. GBATEK wants a halfword-aligned
// destination; an odd one is aligned down, as the official BIOS does (the
// first byte goes to dest & ~1, nothing is read back). Writes are halfwords
// only (see vram_put_byte), so an odd size leaves the last byte unwritten.

swi_runlength_decompress_vram:
  bl cb_decompress
  cmp r6, #0
  ble cb_finish
  bic r5, r5, #1

 0:
  bl cb_read8
  and r10, r0, #0x7F
  tst r0, #0x80
  beq 1f

  // Run: flag + 3 copies of the next byte
  add r10, r10, #3
  sub r6, r6, r10
  bl cb_read8
 2:
  vram_put_byte
  subs r10, r10, #1
  bne 2b
  b 3f

  // Literal block: flag + 1 bytes
 1:
  add r10, r10, #1
  sub r6, r6, r10
 4:
  bl cb_read8
  vram_put_byte
  subs r10, r10, #1
  bne 4b

 3:
  cmp r6, #0
  bgt 0b
  b cb_finish


#define rle_control   r3
#define run_length    r3
#define rle_value     r12

swi_runlength_decompress_wram:
  ldr header, [ source ], #4
  movs length, header, lsr #8
  beq swi_complete

 0:
  ldrb rle_control, [ source ], #1
  tst rle_control, #0x80
  and run_length, rle_control, #0x7F
  beq 1f

  ldrb rle_value, [ source ], #1
  add run_length, run_length, #3
  sub length, length, run_length

 2:
  strb rle_value, [ dest ], #1
  subs run_length, run_length, #1
  bne 2b
  b 3f

 1:
  add run_length, run_length, #1
  sub length, length, run_length

 4:
  ldrb rle_value, [ source ], #1
  subs run_length, run_length, #1
  strb rle_value, [ dest ], #1
  bne 4b

 3:
  cmp length, #0
  bgt 0b
  b swi_complete

#undef rle_control
#undef run_length
#undef rle_value


#ifdef TARGET_ARM9

#undef accumulator
#undef length

#define source        r0
#define dest          r1
#define length        r2
#define header        r3

#define accumulator   r12
#define current_value r3

// The first unit is always written, also for a size of 0 (as the official
// BIOS does); after that units are written while the signed remaining size
// is above 0, so Diff16 rounds an odd size up to a whole halfword (also as
// the official BIOS does).

swi_diff_8bit_unfilter_wram:
  ldr header, [ source ], #4

  ldrb accumulator, [ source ], #1
  mov length, header, lsr #8

  strb accumulator, [ dest ], #1
  subs length, length, #1
  ble swi_complete

 0:
  ldrb current_value, [ source ], #1
  add accumulator, accumulator, current_value
  strb accumulator, [ dest ], #1
  subs length, length, #1
  bgt 0b

  b swi_complete


swi_diff_16bit_unfilter:
  ldr header, [ source ], #4

  ldrh accumulator, [ source ], #2
  mov length, header, lsr #8

  strh accumulator, [ dest ], #2
  subs length, length, #2
  ble swi_complete

 0:
  ldrh current_value, [ source ], #2
  add accumulator, accumulator, current_value
  strh accumulator, [ dest ], #2
  subs length, length, #2
  bgt 0b

  b swi_complete

#endif


#ifdef TARGET_ARM7
sine_table:
  .hword 0x0000, 0x0324, 0x0648, 0x096A, 0x0C8C, 0x0FAB, 0x12C8, 0x15E2
  .hword 0x18F9, 0x1C0B, 0x1F1A, 0x2223, 0x2528, 0x2826, 0x2B1F, 0x2E11
  .hword 0x30FB, 0x33DF, 0x36BA, 0x398C, 0x3C56, 0x3F17, 0x41CE, 0x447A
  .hword 0x471C, 0x49B4, 0x4C3F, 0x4EBF, 0x5133, 0x539B, 0x55F5, 0x5842
  .hword 0x5A82, 0x5CB3, 0x5ED7, 0x60EB, 0x62F1, 0x64E8, 0x66CF, 0x68A6
  .hword 0x6A6D, 0x6C23, 0x6DC9, 0x6F5E, 0x70E2, 0x7254, 0x73B5, 0x7504
  .hword 0x7641, 0x776B, 0x7884, 0x7989, 0x7A7C, 0x7B5C, 0x7C29, 0x7CE3
  .hword 0x7D89, 0x7E1D, 0x7E9C, 0x7F09, 0x7F61, 0x7FA6, 0x7FD8, 0x7FF5

swi_get_sine_table:
  add r0, r0, r0
  adr r1, sine_table
  // Should some protection be here?
  ldrh r0, [ r1, r0 ]
  b swi_complete

pitch_table:
  .hword 0x0000, 0x003B, 0x0076, 0x00B2, 0x00ED, 0x0128, 0x0164, 0x019F
  .hword 0x01DB, 0x0217, 0x0252, 0x028E, 0x02CA, 0x0305, 0x0341, 0x037D
  .hword 0x03B9, 0x03F5, 0x0431, 0x046E, 0x04AA, 0x04E6, 0x0522, 0x055F
  .hword 0x059B, 0x05D8, 0x0614, 0x0651, 0x068D, 0x06CA, 0x0707, 0x0743
  .hword 0x0780, 0x07BD, 0x07FA, 0x0837, 0x0874, 0x08B1, 0x08EF, 0x092C
  .hword 0x0969, 0x09A7, 0x09E4, 0x0A21, 0x0A5F, 0x0A9C, 0x0ADA, 0x0B18
  .hword 0x0B56, 0x0B93, 0x0BD1, 0x0C0F, 0x0C4D, 0x0C8B, 0x0CC9, 0x0D07
  .hword 0x0D45, 0x0D84, 0x0DC2, 0x0E00, 0x0E3F, 0x0E7D, 0x0EBC, 0x0EFA
  .hword 0x0F39, 0x0F78, 0x0FB6, 0x0FF5, 0x1034, 0x1073, 0x10B2, 0x10F1
  .hword 0x1130, 0x116F, 0x11AE, 0x11EE, 0x122D, 0x126C, 0x12AC, 0x12EB
  .hword 0x132B, 0x136B, 0x13AA, 0x13EA, 0x142A, 0x146A, 0x14A9, 0x14E9
  .hword 0x1529, 0x1569, 0x15AA, 0x15EA, 0x162A, 0x166A, 0x16AB, 0x16EB
  .hword 0x172C, 0x176C, 0x17AD, 0x17ED, 0x182E, 0x186F, 0x18B0, 0x18F0
  .hword 0x1931, 0x1972, 0x19B3, 0x19F5, 0x1A36, 0x1A77, 0x1AB8, 0x1AFA
  .hword 0x1B3B, 0x1B7D, 0x1BBE, 0x1C00, 0x1C41, 0x1C83, 0x1CC5, 0x1D07
  .hword 0x1D48, 0x1D8A, 0x1DCC, 0x1E0E, 0x1E51, 0x1E93, 0x1ED5, 0x1F17
  .hword 0x1F5A, 0x1F9C, 0x1FDF, 0x2021, 0x2064, 0x20A6, 0x20E9, 0x212C
  .hword 0x216F, 0x21B2, 0x21F5, 0x2238, 0x227B, 0x22BE, 0x2301, 0x2344
  .hword 0x2388, 0x23CB, 0x240E, 0x2452, 0x2496, 0x24D9, 0x251D, 0x2561
  .hword 0x25A4, 0x25E8, 0x262C, 0x2670, 0x26B4, 0x26F8, 0x273D, 0x2781
  .hword 0x27C5, 0x280A, 0x284E, 0x2892, 0x28D7, 0x291C, 0x2960, 0x29A5
  .hword 0x29EA, 0x2A2F, 0x2A74, 0x2AB9, 0x2AFE, 0x2B43, 0x2B88, 0x2BCD
  .hword 0x2C13, 0x2C58, 0x2C9D, 0x2CE3, 0x2D28, 0x2D6E, 0x2DB4, 0x2DF9
  .hword 0x2E3F, 0x2E85, 0x2ECB, 0x2F11, 0x2F57, 0x2F9D, 0x2FE3, 0x302A
  .hword 0x3070, 0x30B6, 0x30FD, 0x3143, 0x318A, 0x31D0, 0x3217, 0x325E
  .hword 0x32A5, 0x32EC, 0x3332, 0x3379, 0x33C1, 0x3408, 0x344F, 0x3496
  .hword 0x34DD, 0x3525, 0x356C, 0x35B4, 0x35FB, 0x3643, 0x368B, 0x36D3
  .hword 0x371A, 0x3762, 0x37AA, 0x37F2, 0x383A, 0x3883, 0x38CB, 0x3913
  .hword 0x395C, 0x39A4, 0x39ED, 0x3A35, 0x3A7E, 0x3AC6, 0x3B0F, 0x3B58
  .hword 0x3BA1, 0x3BEA, 0x3C33, 0x3C7C, 0x3CC5, 0x3D0E, 0x3D58, 0x3DA1
  .hword 0x3DEA, 0x3E34, 0x3E7D, 0x3EC7, 0x3F11, 0x3F5A, 0x3FA4, 0x3FEE
  .hword 0x4038, 0x4082, 0x40CC, 0x4116, 0x4161, 0x41AB, 0x41F5, 0x4240
  .hword 0x428A, 0x42D5, 0x431F, 0x436A, 0x43B5, 0x4400, 0x444B, 0x4495
  .hword 0x44E1, 0x452C, 0x4577, 0x45C2, 0x460D, 0x4659, 0x46A4, 0x46F0
  .hword 0x473B, 0x4787, 0x47D3, 0x481E, 0x486A, 0x48B6, 0x4902, 0x494E
  .hword 0x499A, 0x49E6, 0x4A33, 0x4A7F, 0x4ACB, 0x4B18, 0x4B64, 0x4BB1
  .hword 0x4BFE, 0x4C4A, 0x4C97, 0x4CE4, 0x4D31, 0x4D7E, 0x4DCB, 0x4E18
  .hword 0x4E66, 0x4EB3, 0x4F00, 0x4F4E, 0x4F9B, 0x4FE9, 0x5036, 0x5084
  .hword 0x50D2, 0x5120, 0x516E, 0x51BC, 0x520A, 0x5258, 0x52A6, 0x52F4
  .hword 0x5343, 0x5391, 0x53E0, 0x542E, 0x547D, 0x54CC, 0x551A, 0x5569
  .hword 0x55B8, 0x5607, 0x5656, 0x56A5, 0x56F4, 0x5744, 0x5793, 0x57E2
  .hword 0x5832, 0x5882, 0x58D1, 0x5921, 0x5971, 0x59C1, 0x5A10, 0x5A60
  .hword 0x5AB0, 0x5B01, 0x5B51, 0x5BA1, 0x5BF1, 0x5C42, 0x5C92, 0x5CE3
  .hword 0x5D34, 0x5D84, 0x5DD5, 0x5E26, 0x5E77, 0x5EC8, 0x5F19, 0x5F6A
  .hword 0x5FBB, 0x600D, 0x605E, 0x60B0, 0x6101, 0x6153, 0x61A4, 0x61F6
  .hword 0x6248, 0x629A, 0x62EC, 0x633E, 0x6390, 0x63E2, 0x6434, 0x6487
  .hword 0x64D9, 0x652C, 0x657E, 0x65D1, 0x6624, 0x6676, 0x66C9, 0x671C
  .hword 0x676F, 0x67C2, 0x6815, 0x6869, 0x68BC, 0x690F, 0x6963, 0x69B6
  .hword 0x6A0A, 0x6A5E, 0x6AB1, 0x6B05, 0x6B59, 0x6BAD, 0x6C01, 0x6C55
  .hword 0x6CAA, 0x6CFE, 0x6D52, 0x6DA7, 0x6DFB, 0x6E50, 0x6EA4, 0x6EF9
  .hword 0x6F4E, 0x6FA3, 0x6FF8, 0x704D, 0x70A2, 0x70F7, 0x714D, 0x71A2
  .hword 0x71F7, 0x724D, 0x72A2, 0x72F8, 0x734E, 0x73A4, 0x73FA, 0x7450
  .hword 0x74A6, 0x74FC, 0x7552, 0x75A8, 0x75FF, 0x7655, 0x76AC, 0x7702
  .hword 0x7759, 0x77B0, 0x7807, 0x785E, 0x78B4, 0x790C, 0x7963, 0x79BA
  .hword 0x7A11, 0x7A69, 0x7AC0, 0x7B18, 0x7B6F, 0x7BC7, 0x7C1F, 0x7C77
  .hword 0x7CCF, 0x7D27, 0x7D7F, 0x7DD7, 0x7E2F, 0x7E88, 0x7EE0, 0x7F38
  .hword 0x7F91, 0x7FEA, 0x8042, 0x809B, 0x80F4, 0x814D, 0x81A6, 0x81FF
  .hword 0x8259, 0x82B2, 0x830B, 0x8365, 0x83BE, 0x8418, 0x8472, 0x84CB
  .hword 0x8525, 0x857F, 0x85D9, 0x8633, 0x868E, 0x86E8, 0x8742, 0x879D
  .hword 0x87F7, 0x8852, 0x88AC, 0x8907, 0x8962, 0x89BD, 0x8A18, 0x8A73
  .hword 0x8ACE, 0x8B2A, 0x8B85, 0x8BE0, 0x8C3C, 0x8C97, 0x8CF3, 0x8D4F
  .hword 0x8DAB, 0x8E07, 0x8E63, 0x8EBF, 0x8F1B, 0x8F77, 0x8FD4, 0x9030
  .hword 0x908C, 0x90E9, 0x9146, 0x91A2, 0x91FF, 0x925C, 0x92B9, 0x9316
  .hword 0x9373, 0x93D1, 0x942E, 0x948C, 0x94E9, 0x9547, 0x95A4, 0x9602
  .hword 0x9660, 0x96BE, 0x971C, 0x977A, 0x97D8, 0x9836, 0x9895, 0x98F3
  .hword 0x9952, 0x99B0, 0x9A0F, 0x9A6E, 0x9ACD, 0x9B2C, 0x9B8B, 0x9BEA
  .hword 0x9C49, 0x9CA8, 0x9D08, 0x9D67, 0x9DC7, 0x9E26, 0x9E86, 0x9EE6
  .hword 0x9F46, 0x9FA6, 0xA006, 0xA066, 0xA0C6, 0xA127, 0xA187, 0xA1E8
  .hword 0xA248, 0xA2A9, 0xA30A, 0xA36B, 0xA3CC, 0xA42D, 0xA48E, 0xA4EF
  .hword 0xA550, 0xA5B2, 0xA613, 0xA675, 0xA6D6, 0xA738, 0xA79A, 0xA7FC
  .hword 0xA85E, 0xA8C0, 0xA922, 0xA984, 0xA9E7, 0xAA49, 0xAAAC, 0xAB0E
  .hword 0xAB71, 0xABD4, 0xAC37, 0xAC9A, 0xACFD, 0xAD60, 0xADC3, 0xAE27
  .hword 0xAE8A, 0xAEED, 0xAF51, 0xAFB5, 0xB019, 0xB07C, 0xB0E0, 0xB145
  .hword 0xB1A9, 0xB20D, 0xB271, 0xB2D6, 0xB33A, 0xB39F, 0xB403, 0xB468
  .hword 0xB4CD, 0xB532, 0xB597, 0xB5FC, 0xB662, 0xB6C7, 0xB72C, 0xB792
  .hword 0xB7F7, 0xB85D, 0xB8C3, 0xB929, 0xB98F, 0xB9F5, 0xBA5B, 0xBAC1
  .hword 0xBB28, 0xBB8E, 0xBBF5, 0xBC5B, 0xBCC2, 0xBD29, 0xBD90, 0xBDF7
  .hword 0xBE5E, 0xBEC5, 0xBF2C, 0xBF94, 0xBFFB, 0xC063, 0xC0CA, 0xC132
  .hword 0xC19A, 0xC202, 0xC26A, 0xC2D2, 0xC33A, 0xC3A2, 0xC40B, 0xC473
  .hword 0xC4DC, 0xC544, 0xC5AD, 0xC616, 0xC67F, 0xC6E8, 0xC751, 0xC7BB
  .hword 0xC824, 0xC88D, 0xC8F7, 0xC960, 0xC9CA, 0xCA34, 0xCA9E, 0xCB08
  .hword 0xCB72, 0xCBDC, 0xCC47, 0xCCB1, 0xCD1B, 0xCD86, 0xCDF1, 0xCE5B
  .hword 0xCEC6, 0xCF31, 0xCF9C, 0xD008, 0xD073, 0xD0DE, 0xD14A, 0xD1B5
  .hword 0xD221, 0xD28D, 0xD2F8, 0xD364, 0xD3D0, 0xD43D, 0xD4A9, 0xD515
  .hword 0xD582, 0xD5EE, 0xD65B, 0xD6C7, 0xD734, 0xD7A1, 0xD80E, 0xD87B
  .hword 0xD8E9, 0xD956, 0xD9C3, 0xDA31, 0xDA9E, 0xDB0C, 0xDB7A, 0xDBE8
  .hword 0xDC56, 0xDCC4, 0xDD32, 0xDDA0, 0xDE0F, 0xDE7D, 0xDEEC, 0xDF5B
  .hword 0xDFC9, 0xE038, 0xE0A7, 0xE116, 0xE186, 0xE1F5, 0xE264, 0xE2D4
  .hword 0xE343, 0xE3B3, 0xE423, 0xE493, 0xE503, 0xE573, 0xE5E3, 0xE654
  .hword 0xE6C4, 0xE735, 0xE7A5, 0xE816, 0xE887, 0xE8F8, 0xE969, 0xE9DA
  .hword 0xEA4B, 0xEABC, 0xEB2E, 0xEB9F, 0xEC11, 0xEC83, 0xECF5, 0xED66
  .hword 0xEDD9, 0xEE4B, 0xEEBD, 0xEF2F, 0xEFA2, 0xF014, 0xF087, 0xF0FA
  .hword 0xF16D, 0xF1E0, 0xF253, 0xF2C6, 0xF339, 0xF3AD, 0xF420, 0xF494
  .hword 0xF507, 0xF57B, 0xF5EF, 0xF663, 0xF6D7, 0xF74C, 0xF7C0, 0xF834
  .hword 0xF8A9, 0xF91E, 0xF992, 0xFA07, 0xFA7C, 0xFAF1, 0xFB66, 0xFBDC
  .hword 0xFC51, 0xFCC7, 0xFD3C, 0xFDB2, 0xFE28, 0xFE9E, 0xFF14, 0xFF8A

pitch_table_ptr:
  .word pitch_table

swi_get_pitch_table:
  add r0, r0, r0
  ldr r1, pitch_table_ptr
  // Should some protection be here?
  ldrh r0, [ r1, r0 ]
  b swi_complete

volume_table:
  .byte 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01
  .byte 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02
  .byte 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02
  .byte 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02
  .byte 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02
  .byte 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02
  .byte 0x02, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03
  .byte 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03
  .byte 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03
  .byte 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03
  .byte 0x03, 0x03, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04
  .byte 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04
  .byte 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04
  .byte 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05
  .byte 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05
  .byte 0x05, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06
  .byte 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06
  .byte 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07
  .byte 0x07, 0x07, 0x07, 0x07, 0x08, 0x08, 0x08, 0x08
  .byte 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x09
  .byte 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09
  .byte 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A
  .byte 0x0A, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B, 0x0B
  .byte 0x0B, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C
  .byte 0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0E
  .byte 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0F, 0x0F
  .byte 0x0F, 0x0F, 0x0F, 0x10, 0x10, 0x10, 0x10, 0x10
  .byte 0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12, 0x12
  .byte 0x12, 0x12, 0x12, 0x13, 0x13, 0x13, 0x13, 0x14
  .byte 0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15, 0x15
  .byte 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17, 0x18
  .byte 0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19, 0x1A
  .byte 0x1A, 0x1A, 0x1B, 0x1B, 0x1B, 0x1C, 0x1C, 0x1C
  .byte 0x1D, 0x1D, 0x1D, 0x1E, 0x1E, 0x1E, 0x1F, 0x1F
  .byte 0x1F, 0x20, 0x20, 0x20, 0x21, 0x21, 0x22, 0x22
  .byte 0x22, 0x23, 0x23, 0x24, 0x24, 0x24, 0x25, 0x25
  .byte 0x26, 0x26, 0x27, 0x27, 0x27, 0x28, 0x28, 0x29
  .byte 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D
  .byte 0x2D, 0x2E, 0x2E, 0x2F, 0x2F, 0x30, 0x31, 0x31
  .byte 0x32, 0x32, 0x33, 0x33, 0x34, 0x35, 0x35, 0x36
  .byte 0x36, 0x37, 0x38, 0x38, 0x39, 0x3A, 0x3A, 0x3B
  .byte 0x3C, 0x3C, 0x3D, 0x3E, 0x3F, 0x3F, 0x40, 0x41
  .byte 0x42, 0x42, 0x43, 0x44, 0x45, 0x45, 0x46, 0x47
  .byte 0x48, 0x49, 0x4A, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E
  .byte 0x4F, 0x50, 0x51, 0x52, 0x52, 0x53, 0x54, 0x55
  .byte 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5D, 0x5E
  .byte 0x5F, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x67
  .byte 0x68, 0x69, 0x6A, 0x6B, 0x6D, 0x6E, 0x6F, 0x71
  .byte 0x72, 0x73, 0x75, 0x76, 0x77, 0x79, 0x7A, 0x7B
  .byte 0x7D, 0x7E, 0x7F, 0x20, 0x21, 0x21, 0x21, 0x22
  .byte 0x22, 0x23, 0x23, 0x23, 0x24, 0x24, 0x25, 0x25
  .byte 0x26, 0x26, 0x26, 0x27, 0x27, 0x28, 0x28, 0x29
  .byte 0x29, 0x2A, 0x2A, 0x2B, 0x2B, 0x2C, 0x2C, 0x2D
  .byte 0x2D, 0x2E, 0x2E, 0x2F, 0x2F, 0x30, 0x30, 0x31
  .byte 0x31, 0x32, 0x33, 0x33, 0x34, 0x34, 0x35, 0x36
  .byte 0x36, 0x37, 0x37, 0x38, 0x39, 0x39, 0x3A, 0x3B
  .byte 0x3B, 0x3C, 0x3D, 0x3E, 0x3E, 0x3F, 0x40, 0x40
  .byte 0x41, 0x42, 0x43, 0x43, 0x44, 0x45, 0x46, 0x47
  .byte 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4D
  .byte 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55
  .byte 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D
  .byte 0x5E, 0x5F, 0x60, 0x62, 0x63, 0x64, 0x65, 0x66
  .byte 0x67, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6F, 0x70
  .byte 0x71, 0x73, 0x74, 0x75, 0x77, 0x78, 0x79, 0x7B
  .byte 0x7C, 0x7E, 0x7E, 0x40, 0x41, 0x42, 0x43, 0x43
  .byte 0x44, 0x45, 0x46, 0x47, 0x47, 0x48, 0x49, 0x4A
  .byte 0x4B, 0x4C, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51
  .byte 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59
  .byte 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61
  .byte 0x62, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6B
  .byte 0x6C, 0x6D, 0x6E, 0x70, 0x71, 0x72, 0x74, 0x75
  .byte 0x76, 0x78, 0x79, 0x7B, 0x7C, 0x7D, 0x7E, 0x40
  .byte 0x41, 0x42, 0x42, 0x43, 0x44, 0x45, 0x46, 0x46
  .byte 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4B, 0x4C, 0x4D
  .byte 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55
  .byte 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D
  .byte 0x5E, 0x5F, 0x60, 0x61, 0x62, 0x63, 0x65, 0x66
  .byte 0x67, 0x68, 0x69, 0x6A, 0x6C, 0x6D, 0x6E, 0x6F
  .byte 0x71, 0x72, 0x73, 0x75, 0x76, 0x77, 0x79, 0x7A
  .byte 0x7C, 0x7D, 0x7E, 0x7F

padding_b:
  .word 0x0

swi_get_boot_procs:
  mov r0, #0x2E
  mov r1, #0x3C
  mov r2, #0xFF

  orr r0, r0, #0x0A00
  orr r1, r1, #0x2C00
  orr r2, r2, #0x0500

  b swi_complete
#endif

// ARM9 CustomPost writes r0 to POSTFLG. ARM7 CustomHalt writes the 8-bit
// parameter in r2 to HALTCNT (GBATEK) and keeps the other registers.
swi_custom_halt_post:
#ifdef TARGET_ARM7
  mov r12, #0x4000000
  strb r2, [ r12, #0x301 ]
#else
  mov r1, #0x4000000
  strb r0, [ r1, #0x300 ]
#endif

  b swi_complete


interrupt_handler:
  // Save these registers, IRQ functions will be allowed to modify them w/o
  // saving.
  stmdb sp!, { r0 - r3, r12, lr }

#ifdef TARGET_ARM9
  // Get DTCM base
  mrc p15, 0, r0, cr9, cr1, 0
  bic r0, r0, #0xFF

  // Pointer to IRQ handler is in DTCM[0x3FFC]
  add r0, r0, #0x4000
#else
  // Pointer to IRQ handler is at 0x03FFFFFC (mirrored WRAM)
  mov r0, #0x4000000
#endif

  // Store return address and branch to handler
  mov lr, pc
  ldr pc, [ r0, #-4 ]

  // Return from IRQ
  ldmia sp!, { r0 - r3, r12, lr }
  subs pc, lr, #4

#ifdef TARGET_ARM7
swi_get_volume_table:
  adr r1, volume_table
  // Should some protection be here?
  ldrb r0, [ r1, r0 ]
  b swi_complete
#endif

#ifndef PLATFORM_TWL

// SoftReset (GBATEK, NDS7/NDS9): clears the 200h bytes at the top of the
// stack area (stacks, IRQ handler pointer and IRQ check flags), sets up the
// svc, irq and sys stacks, zeroes r0-r12 and jumps to the entry address in
// system mode with IRQs enabled. bx is used so a Thumb entry works.
// lr_svc, lr_irq and the svc/irq SPSRs are cleared; lr_sys holds the entry.
// Both CPUs mask IRQ/FIQ first: the SWI keeps the caller's I bit, and an IRQ
// during the clear would call the zeroed IRQ handler pointer.
//   ARM7: clears 0380FE00h-0380FFFFh (via the 03FFFExxh mirror),
//         sp_svc = 0380FFDCh, sp_irq = 0380FFB0h, sp_sys = 0380FF00h,
//         entry from [027FFE34h].
//   ARM9: (with IRQs masked, which also matters because the ITCM, where
//         games keep their IRQ handler, goes off below) cleans and
//         invalidates the DCache, invalidates the ICache, drains the
//         write buffer and sets the CP15 control register to 12078h
//         (GBATEK only says "flushes caches and write buffer"; clean +
//         invalidate is used because it cannot lose data). Then clears
//         DTCM+3E00h-DTCM+3FFFh and sets the stacks GBATEK calls
//         hardcoded, whatever the DTCM base (as the official BIOS does):
//         sp_svc = 00803FC0h, sp_irq = 00803FA0h, sp_sys = 00803EC0h.
//         Entry from [027FFE24h]. (EXPERIMENTAL: DTCM+3FC0h/3FA0h/3EC0h.)
// Both CPUs enter with Z and C set, as the official BIOS leaves them.
// The DSi BIOS has no SWI 00h (GBATEK), so the TWL build leaves it out.

#ifdef TARGET_ARM7
stack_pointer_svc:
  .word 0x0380ffdc
#endif

swi_soft_reset:
  // System mode, IRQ and FIQ off until the jump to the entry
  msr CPSR_c, #0xdf

#ifdef TARGET_ARM9
  // Clean and invalidate the 4KB, 4-way DCache by segment/index:
  // segment in bits 30-31, line index (32 lines of 32 bytes) in bits 5-9
  mov r1, #0
 1:
  mcr p15, 0, r1, cr7, cr14, 2
  add r1, r1, #0x20
  tst r1, #0x400
  beq 1b
  bic r1, r1, #0x400
  adds r1, r1, #0x40000000
  bne 1b

  // Invalidate the ICache, drain the write buffer
  mcr p15, 0, r1, cr7, cr5, 0
  mcr p15, 0, r1, cr7, cr10, 4

  // Control register = 12078h: MPU, caches and ITCM off, DTCM on,
  // exception vectors at FFFF0000h
  mov r1, #0x12000
  orr r1, r1, #0x78
  mcr p15, 0, r1, cr1, cr0, 0

  // r0 = DTCM base + 0x4000
  mrc p15, 0, r0, cr9, cr1, 0
  mov r0, r0, lsr #12
  mov r0, r0, lsl #12
  add r0, r0, #0x4000
#else
  // r0 = 0x04000000, the top of the 0x03FFFE00 mirror of 0x0380FE00
  mov r0, #0x4000000
#endif
  mov r1, #0x80
  mov r2, #0

  // Clear 0x200 bytes downwards; r0 ends at the start of the cleared area
 0:
  str r2, [ r0, #-4 ]!
  subs r1, r1, #1
  bne 0b

  // r1 = sp_svc
#ifdef TARGET_ARM9
#ifdef FREEBIOS_EXPERIMENTAL
  // EXPERIMENTAL (safe choice): DTCM-relative stacks, DTCM+3FC0h, so they
  // stay valid when a game has moved the DTCM. r0 = DTCM+3E00h.
  add r1, r0, #0x1C0
#else
  // GBATEK: the NDS9 stacks are hardcoded, 00803FC0h whatever the DTCM base
  // (the official BIOS does the same, SWI test ROM). r0 = DTCM+3E00h.
  mov r1, #0x00800000
  orr r1, r1, #0x3FC0
#endif
#else
  ldr r1, stack_pointer_svc
#endif

  // Supervisor mode, interrupts disabled
  mov r3, #0xd3
  msr CPSR_fsxc, r3
  mov sp, r1
  mov lr, r2
  msr SPSR_fsxc, r2

  // IRQ mode, interrupts disabled
  mov r3, #0xd2
  msr CPSR_fsxc, r3
#ifdef TARGET_ARM9
  // sp_irq = sp_svc - 20h (00803FA0h)
  sub sp, r1, #0x20
#else
  // sp_irq = 0x0380FFB0
  sub sp, r1, #(0xdc - 0xb0)
#endif
  mov lr, r2
  msr SPSR_fsxc, r2

  // System mode, interrupts enabled; Z and C set, as the official BIOS
  // leaves them (SWI test ROM)
  mov r3, #0x5f
  orr r3, r3, #0x60000000
  msr CPSR_fsxc, r3
#ifdef TARGET_ARM9
  // sp_sys = sp_svc - 100h (00803EC0h)
  sub sp, r1, #0x100
  // Entry address at 0x027FFE24
  mov r3, #0x02800000
  ldr lr, [ r3, #-0x1DC ]
#else
  // sp_sys = 0x0380FF00
  sub sp, r1, #0xdc
  // Entry address at 0x027FFE34
  mov r3, #0x02800000
  ldr lr, [ r3, #-0x1CC ]
#endif

  // Set r0-r12 to 0, r0 still points to the cleared area
  ldmia r0, { r0 - r12 }
  bx lr

#endif

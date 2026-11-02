# FreeBIOS review and upgrade plan

Review of `freebios/bios_common.S`, the committed `drastic_bios_arm{7,9}.bin`
files and the arrays in `src/FreeBIOS.cpp`, compared against current upstream
melonDS (Nov 2026). Phases 0, 1 and 2 are done (see the Plan section); the
rest is the backlog.

## State of the code

- The assembly source, the committed `.bin` files and the arrays in
  `src/FreeBIOS.cpp` agree byte for byte.
- The source now uses unified suffix order (`ldrbeq`, `strhne`, `ldmiaeq`),
  which both clang and GNU as accept. `freebios/build.sh` builds it with
  clang + ld.lld + llvm-objcopy (or a GNU toolchain via
  `PREFIX=arm-none-eabi-`) and regenerates `src/FreeBIOS.cpp`;
  `freebios/check.sh` verifies that the source, the `.bin` files and the
  arrays agree. Before Phase 1 the rebuild was byte-identical to the
  original 2011 binaries.

- Upstream melonDS replaced this layout in Feb 2026 (PR #2523, asie):
  `freebios/src/bios_common.s` + `Makefile.bios`, a generated
  `src/FreeBIOS_Data.h`, `FreeBIOSGetNtrArm7/9()` and `FreeBIOSGetTwlArm7/9()`
  accessors, and a TWL (DSi) build that allows DSi direct boot without
  BIOS/firmware/NAND dumps. The SWI code itself is the same lineage, so the
  bugs below exist upstream as well. Upstream commit 1124603 (Apr 2026) touched
  the IntrWait path and its message says it is still broken.

## Confirmed bugs

Items 1, 2, 3 and 6 are fixed (Phase 1); items 4 and 5 are fixed (Phase 2).

1. **SWI jump table is off by one.** Entry `0x1E` is missing. SWI `0x1E`
   runs `swi_custom_halt_post`; SWI `0x1F` (GBATEK CustomPost) loads the first
   instruction of `swi_complete` (`0xE8BD4000`) as its target and crashes.
   Verified in `drastic_bios_arm9.bin` at table offset `0x138 + 0x1F*4`.
2. **`swi_divide` hangs** on division by zero and on any numerator equal to
   `INT_MIN`: the alignment loop shifts the denominator until it wraps to 0 and
   `cmp denominator, numerator` is then always `ls`.
3. **`swi_interrupt_wait` with r0=0** always halts at least once instead of
   returning immediately when the requested flag is already set
   (`blne interrupt_check` result is ignored).
4. **`swi_soft_reset` is not a real SoftReset.** It clears stacks and sets up
   modes, then does `movs pc, lr` from system mode; it never jumps to the entry
   address at `0x027FFE24` (ARM9) / `0x027FFE34` (ARM7).
5. **`swi_huffman_decompress` (SWI 0x13) is an empty stub.**
6. **`swi_runlength_decompress_vram` (SWI 0x15) is the WRAM routine.** It uses
   byte stores, and VRAM ignores 8-bit writes, so output is corrupt. The LZ77
   VRAM routine already shows the correct 16-bit store pattern.
7. **`swi_sound_bias`** has no ramp delay (existing TODO). Minor.
8. **No bounds checks** on `GetSineTable` / `GetPitchTable` /
   `GetVolumeTable`. The real BIOS does not check either. Low priority.

Other notes from the source: `boot_handler` and `unhandled_exception` are
infinite loops (fine while melonDS HLEs boot); `swi_bit_unpack` does not validate
bit widths (existing TODO).

## Plan

### Phase 0: tooling (done)
- [x] `freebios/build.sh` builds both `.bin` files (asserting 16384 / 4096
  bytes) and regenerates the arrays in `src/FreeBIOS.cpp`. `make` calls it.
- [x] `freebios/check.sh` (`make check`) rebuilds into a temp dir and fails if
  the committed `.bin` files or the arrays differ, so fixes are never
  hand-patched hex. Not wired into CI yet.

### Phase 1: low-risk fixes (done)
- [x] Inserted the missing `0x1E` table entry (`swi_invalid`); `0x1F` now
  reaches `swi_custom_halt_post`.
- [x] `swi_divide` no longer hangs. Decisions:
  - Denominator 0 (hangs/undefined on real hardware): returns r0 = 0,
    r1 = numerator, r3 = 0.
  - The alignment loop stops once the denominator's bit 31 is set, so
    numerator `INT_MIN` works. `INT_MIN / -1` wraps to r0 = `0x80000000`,
    r3 = `0x80000000`.
  - The remainder now takes the sign of the numerator (it was always
    positive before), e.g. -7/2 gives r0 = -3, r1 = -1, r3 = 3.
  - `sign_flip` moved from r12 to r4 to keep the numerator sign for the
    remainder; r4 is stacked by the SWI entry.
  - Checked with an instruction-level Python model on edge cases and 200k
    random inputs.
- [x] IntrWait: r0 = 0 returns immediately if a requested flag is already set
  (acknowledging it), otherwise halts until one is raised; r0 != 0 discards
  the requested flags first, then halts. Reuses `swi_interrupt_check_first`.
- [x] `swi_runlength_decompress_vram` is a separate routine that only does
  halfword stores (buffers the even byte in r5, stores on the odd byte).
  An odd start address keeps the existing low byte, and an odd final
  address is flushed with the existing high byte. A zero length returns
  at once. The WRAM routine is unchanged.

### Phase 2: missing features (done)
- [x] Huffman decompression (SWI 0x13, `swi_huffman_decompress`) per GBATEK
  HuffUnComp: 4- or 8-bit data, tree walked from the root at tree+1,
  children at `(A & ~1) + 2 + offset*2` (+1 for node1), bitstream read as
  32-bit words MSB first, output collected LSB first and stored as 32-bit
  words. Decisions:
  - A word is only stored once 32 bits are collected and the remaining size
    drops by 4 per word, so a size that is not a multiple of 4 is rounded up
    to whole words (the stream must carry the padding symbols). This is the
    32-bit-unit behaviour GBATEK describes; nothing partial is flushed.
  - Size 0 or a data-size field of 0 writes nothing (the latter avoids an
    endless loop). Other data sizes are not rejected; a size that does not
    divide 32 stores a word as soon as 32 or more bits are collected.
  - Uses r5-r10 (stacked).
  - Checked by running the assembled ARM9 and ARM7 code in a small Python
    ARM interpreter against a Python GBATEK-format encoder and an
    independent reference decoder: 366 cases, 4- and 8-bit data, sizes
    1-1000 including non-multiples of 4, single-symbol trees and skewed
    trees up to code length 255; callee registers and sp are preserved and
    only the expected words are written.
- [x] SoftReset (SWI 0x00) clears the 200h bytes at the top of the stack
  area, sets the svc/irq/sys stacks, clears lr_svc/lr_irq and the svc/irq
  SPSRs, zeroes r0-r12 and jumps with `bx` (Thumb entries work) to the
  address at `0x027FFE24` (ARM9) / `0x027FFE34` (ARM7) in system mode with
  IRQs enabled. lr_sys holds the entry address. Stack values:
  - ARM7: svc `0x0380FFDC`, irq `0x0380FFB0`, sys `0x0380FF00` (the existing
    values, matching GBATEK); clears `0x0380FE00-0x0380FFFF` via the
    `0x03FFFExx` mirror.
  - ARM9: relative to the DTCM base read from CP15 c9,c1: svc DTCM+3FC0h,
    irq DTCM+3F80h, sys DTCM+2F7Ch; clears DTCM+3E00h-3FFFh. GBATEK could
    not be fetched from this environment, so these are the values melonDS's
    direct boot (`NDS::SetupDirectBoot`, DTCM at `0x03000000`) sets. To
    re-check against GBATEK "BIOS RAM Usage", especially the sys stack.
  - The old "return-address flag" idea is not implemented: the NDS
    SoftReset always uses the entry address above.
  - Checked in the Python ARM interpreter (with MSR/MRC support) for both
    CPUs and ARM/Thumb entries: cleared range, banked sp/lr, SPSRs, r0-r12.
- [x] CpuFastSet (SWI 0x0C) uses `ldmia`/`stmia` of 8 registers (r3-r10,
  r5-r10 stacked); fill mode stores 8 copies per iteration. The 21-bit
  length is rounded up to a multiple of 8 words; a length of 0 now returns
  at once (it used to loop 2^32 times). Checked for lengths 0-39, 0x1FFFF9
  and 0x1FFFFF, copy and fill, with junk in bits 21-31.
- ARM9 image now uses 0x90C of 0x1000 bytes (1780 free); ARM7 uses 0x21DC
  of 0x4000.

### Phase 3: upstream alignment
- Adopt upstream's `FreeBIOS_Data.h` + accessor layout.
- Bring in the TWL build if DSi direct boot without dumps is wanted for the
  Android app (note `src/DSi.cpp` and `src/Args.h` still default DSi BIOS to
  `BrokenBIOS`).

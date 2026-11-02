# FreeBIOS review and upgrade plan

Review of `freebios/bios_common.S`, the committed `drastic_bios_arm{7,9}.bin`
files and the arrays in `src/FreeBIOS.cpp`, compared against current upstream
melonDS (Nov 2026). Nothing here has been implemented yet; this is the backlog.

## State of the code

- The assembly source, the committed `.bin` files and the arrays in
  `src/FreeBIOS.cpp` agree byte for byte.
- The source reassembles to identical binaries with a modern toolchain, so the
  original 2011 Pandora toolchain in the Makefile is not required:

  ```bash
  # clang's assembler wants unified suffix order (ldrb<cond>, not ldr<cond>b)
  sed -E 's/\b(ldr|str)(eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)(b|h)\b/\1\3\2/g; s/\bldm(eq|ne)ia\b/ldmia\1/g' bios_common.S > bios_unified.S
  clang --target=arm-none-eabi -march=armv4t  -mcpu=arm7tdmi  -DBIOS_ARM7 -c bios_unified.S -o arm7.o
  clang --target=arm-none-eabi -march=armv5te -mcpu=arm946e-s -DBIOS_ARM9 -c bios_unified.S -o arm9.o
  ld.lld -Ttext=0 --entry=0 -o arm7.elf arm7.o && llvm-objcopy -O binary --only-section=.text arm7.elf arm7.bin
  ld.lld -Ttext=0 --entry=0 -o arm9.elf arm9.o && llvm-objcopy -O binary --only-section=.text arm9.elf arm9.bin
  cmp arm7.bin drastic_bios_arm7.bin && cmp arm9.bin drastic_bios_arm9.bin
  ```

- Upstream melonDS replaced this layout in Feb 2026 (PR #2523, asie):
  `freebios/src/bios_common.s` + `Makefile.bios`, a generated
  `src/FreeBIOS_Data.h`, `FreeBIOSGetNtrArm7/9()` and `FreeBIOSGetTwlArm7/9()`
  accessors, and a TWL (DSi) build that allows DSi direct boot without
  BIOS/firmware/NAND dumps. The SWI code itself is the same lineage, so the
  bugs below exist upstream as well. Upstream commit 1124603 (Apr 2026) touched
  the IntrWait path and its message says it is still broken.

## Confirmed bugs

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
infinite loops (fine while melonDS HLEs boot); `swi_cpu_fast_set` could use
ldm/stm (existing TODO, performance only); `swi_bit_unpack` does not validate
bit widths (existing TODO).

## Plan

### Phase 0: tooling
- Add a build script (clang/lld as above, or arm-none-eabi-gcc) under
  `freebios/` that regenerates the `.bin` files and the arrays in
  `src/FreeBIOS.cpp`.
- Add a check (CI or script) that `.bin` == embedded arrays, so fixes are never
  hand-patched hex.

### Phase 1: low-risk fixes
- Insert the missing `0x1E` table entry (`swi_invalid`).
- Guard `swi_divide` against zero denominators and `INT_MIN`; decide the
  return values by checking GBATEK/real-BIOS behaviour first.
- Fix the r0=0 IntrWait path (check, return if set, otherwise halt loop).
- Add a real VRAM variant of RLE decompression.

### Phase 2: missing features
- Implement Huffman decompression (SWI 0x13).
- Implement SoftReset per GBATEK (entry address + return-address flag).
- Optional: ldm/stm fast path for `swi_cpu_fast_set`.

### Phase 3: upstream alignment
- Adopt upstream's `FreeBIOS_Data.h` + accessor layout.
- Bring in the TWL build if DSi direct boot without dumps is wanted for the
  Android app (note `src/DSi.cpp` and `src/Args.h` still default DSi BIOS to
  `BrokenBIOS`).

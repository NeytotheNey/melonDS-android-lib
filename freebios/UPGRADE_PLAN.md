# FreeBIOS review and upgrade plan

Review of the FreeBIOS source and data, compared against current upstream
melonDS (Nov 2026). Phases 0 to 3 are done (see the Plan section); the rest
is the backlog.

## State of the code

- Since Phase 3 the layout follows upstream melonDS (PR #2523, asie, Feb
  2026; checked against upstream master 1e654443): the source is
  `freebios/src/bios_common.s`, built four times (NTR/TWL x ARM9/ARM7), and
  the images live only in the generated `src/FreeBIOS_Data.h` (xxd -i
  format), wrapped by `FreeBIOSGetNtrArm9/7()` and `FreeBIOSGetTwlArm9/7()` in
  `src/FreeBIOS.cpp`. No `.bin` files are committed any more.
- The source uses unified suffix order (`ldrbeq`, `strhne`, `ldmiaeq`),
  which both clang and GNU as accept. `freebios/build.sh` builds it with
  clang + ld.lld + llvm-objcopy (or a GNU toolchain via
  `PREFIX=arm-none-eabi-`) and regenerates `src/FreeBIOS_Data.h`;
  `freebios/check.sh` verifies that the header matches the source. Before
  Phase 1 the rebuild was byte-identical to the original 2011 binaries, and
  build.sh run on upstream's own source reproduces upstream's
  `FreeBIOS_Data.h` byte for byte.
- The SWI code upstream is the same lineage, so upstream still has the bugs
  below. Upstream commit 1124603 (Apr 2026) only undid a regression in the
  IntrWait path from #2523; its message says the code is still broken.

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
   (`blne interrupt_check` result is ignored). Since Phase 3 this is fixed on
   the ARM7 only; on the ARM9 it is kept on purpose, because GBATEK says the
   real NDS9 BIOS does the same.
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
  hand-patched hex. Not wired into CI yet. (Phase 3 changed both scripts to
  the upstream layout; see there.)

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
  Since Phase 3 the immediate return is ARM7 only: GBATEK says the real NDS9
  BIOS always waits for at least one IRQ when r0 = 0, so the ARM9 (NTR and
  TWL) halts first, as the original code and upstream do.
- [x] `swi_runlength_decompress_vram` is a separate routine that only does
  halfword stores (buffers the even byte in r5, stores on the odd byte).
  An odd start address keeps the existing low byte, and an odd final
  address is flushed with the existing high byte (changed 2026-11-02, see
  Test ROM fixes). A zero length returns at once. The WRAM routine is
  unchanged.

### Phase 2: missing features (done)
- [x] Huffman decompression (SWI 0x13, `swi_huffman_decompress`) per GBATEK
  HuffUnComp: 4- or 8-bit data, tree walked from the root at tree+1,
  children at `(A & ~1) + 2 + offset*2` (+1 for node1), bitstream read as
  32-bit words MSB first, output collected LSB first and stored as 32-bit
  words. Decisions:
  - A word is only stored once 32 bits are collected and the remaining size
    drops by 4 per word, so a size that is not a multiple of 4 is rounded up
    to whole words (the stream must carry the padding symbols). This is the
    32-bit-unit behavior GBATEK describes; nothing partial is flushed.
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
    `0x03FFFExx` mirror. Since Phase 3 it first masks IRQ and FIQ
    (`msr CPSR_c, #0xDF`), since the SWI entry keeps the caller's I bit and
    an IRQ during the clear would call the zeroed handler pointer at
    `0x0380FFFC`. The switch to system mode with IRQs on at the end enables
    them again.
  - ARM9 (changed in Phase 3 to follow GBATEK's reset page): first masks
    IRQ and FIQ (`msr CPSR_c, #0xDF`), because the SWI entry keeps the
    caller's I bit and an IRQ after the control register write would jump
    into the game's handler in the now disabled ITCM. Then cleans and
    invalidates the DCache, invalidates the ICache, drains the write buffer
    and sets the CP15 control register to 12078h (MPU, caches and ITCM off,
    DTCM on, vectors at FFFF0000h). Then clears DTCM+3E00h-3FFFh (relative
    to the DTCM base read from CP15 c9,c1, as GBATEK gives it) and sets
    svc DTCM+3FC0h, irq DTCM+3FA0h, sys DTCM+3EC0h. The switch to system
    mode with IRQs on at the end enables them again. Phase 2 used melonDS's
    direct-boot values (irq DTCM+3F80h, sys DTCM+2F7Ch) because GBATEK could
    not be fetched then.
  - Deliberate deviation from GBATEK: GBATEK calls the NDS9 stacks
    hardcoded (`0x00803FC0`, `0x00803FA0`, `0x00803EC0`). Ours are
    DTCM-relative with the same offsets, which gives exactly those values
    with the default DTCM base `0x00800000` and stays valid when a game has
    moved the DTCM (most do). With fixed addresses the stacks would then hit
    unmapped memory, since the ITCM is off after 12078h.
  - GBATEK is not precise about the cache step ("flushes caches and write
    buffer": which caches, and clean or invalidate, is not said, nor is the
    order of the steps). Clean + invalidate of the whole 4KB 4-way DCache
    (c7,c14,2 by segment/index), invalidate of the whole ICache (c7,c5,0) and
    a write-buffer drain (c7,c10,4) were chosen because they cannot lose
    data, and they run before the control register write and the clear.
    melonDS treats the DCache operations as no-ops.
  - The old "return-address flag" idea is not implemented: the NDS
    SoftReset always uses the entry address above.
  - Intentional difference, do not "fix": melonDS direct boot sets the ARM7
    user stack to `0x0380FD80` (what the real firmware leaves when it
    launches a cartridge), while SoftReset sets `0x0380FF00` (what the real
    BIOS SWI 0 sets). Both match hardware; a game that soft-resets sees the
    same change on a real DS.
  - Checked in the Python ARM interpreter (with MSR/MRC support) for both
    CPUs and ARM/Thumb entries: cleared range, banked sp/lr, SPSRs, r0-r12.
- [x] CpuFastSet (SWI 0x0C) uses `ldmia`/`stmia` of 8 registers (r3-r10,
  r5-r10 stacked); fill mode stores 8 copies per iteration. The 21-bit
  length is rounded up to a multiple of 8 words (changed 2026-11-02, see
  Test ROM fixes); a length of 0 now returns
  at once (it used to loop 2^32 times). Checked for lengths 0-39, 0x1FFFF9
  and 0x1FFFFF, copy and fill, with junk in bits 21-31.
- After Phase 2 the ARM9 image used 0x90C of 0x1000 bytes and the ARM7
  0x21DC of 0x4000 (Phase 3 sizes are listed there).

### Phase 3: upstream alignment (done)
- [x] Upstream layout (upstream master 1e654443, i.e. PR #2523 plus
  1124603): `freebios/bios_common.S` moved to `freebios/src/bios_common.s`
  with upstream's `TARGET_ARM9/ARM7` and `PLATFORM_NTR/TWL` defines;
  `src/FreeBIOS.cpp` and `src/FreeBIOS.h` are upstream's files;
  `src/FreeBIOS_Data.h` is generated. `drastic_bios_arm{7,9}.bin` are gone
  (upstream does not commit images either). Users updated: `src/Args.h`,
  `src/NDS.cpp`, `src/frontend/qt_sdl/EmuInstance.cpp` and the app's
  `EmulatorArgsBuilder.cpp` now call `FreeBIOSGetNtrArm9/7()`. The commented
  out `DSi::DSi()` in `src/DSi.cpp` still names the old arrays, as upstream.
  The swi-harness needed no change (it uses the `NDSArgs` defaults).
- [x] Tooling: `build.sh` builds all four images into `freebios/build/`
  (git-ignored) and writes `src/FreeBIOS_Data.h` in exactly the
  `xxd -i -c 16` format upstream's `Makefile` produces. Images are no longer
  padded (`FreeBIOSGet*()` zero-fills), so build.sh asserts each one fits
  its BIOS size instead of an exact size. `check.sh` compares the header,
  ignoring CRLF. Upstream's `Makefile.bios` (GNU toolchain, `--gc-sections`)
  is not used; build.sh keeps the clang/GNU choice. Upstream's
  `.section .text, "axR"` became `.section .text` because clang refuses to
  change the flags of `.text` and nothing here gc's sections. Decisions
  checked: build.sh on upstream's own source (with unified suffixes)
  reproduces upstream's `FreeBIOS_Data.h` byte for byte. Before the
  IntrWait and SoftReset follow-ups below, our NTR images were
  byte-identical to the Phase 2 `.bin` files apart from the dropped zero
  padding; both have changed since.
- [x] TWL (DSi) build from upstream: SWI table up to 0x29 (0x20-0x29 are
  `swi_invalid`, `swi_handler` accepts < 0x2A), LZ77-to-VRAM at 0x02 and
  0x19 instead of 0x12, 0x0F (IsDebugger) invalid, and the ARM7
  `interrupt_check_aux` for the IE2/IF2 BIOS flags at `0x0380FFC0`; r2 is the
  IE2/IF2 mask and VBlankIntrWait sets it to 0, as GBATEK says.
- [x] Deviations from upstream's TWL table, both from GBATEK's DSi SWI list:
  - 0x01 is LZ77UnCompReadByCallbackWrite8bit (GBATEK, DSi7/DSi9);
    upstream has `swi_invalid`. Since 2026-11-02 it is
    `swi_lz77_decompress_callback8`, which reads through the callbacks like
    0x02/0x19 but writes bytes (see Test ROM fixes); before, all three
    reused routines that read the source directly.
  - 0x00 is `swi_invalid`: the DSi BIOS has no SoftReset. GBATEK says SWI
    numbers the DSi BIOS does not list hang in an endless loop (0x12 and
    0x2B are "ignored"); every absent TWL entry here, 0x00 included, returns
    at once instead, as upstream does for the others. `swi_soft_reset` is
    not assembled in the TWL build.
- [x] Phase 1/2 fixes carried over. NTR and TWL both get the SWI 0x1E
  entry, Divide, RLE-to-VRAM, HuffUnComp and CpuFastSet; the IntrWait r0 = 0
  immediate return is ARM7 only (NTR and TWL), and SoftReset is NTR only.
  Conflicts with upstream and what was kept:
  - SWI table: upstream's TWL table also lacks 0x1E, so its 0x1F..0x29 are
    shifted by one and SWI 0x29 reads past the table. With our 0x1E entry the
    TWL table has exactly 0x2A entries.
  - IntrWait: upstream (after 1124603) halts at least once for r0 = 0 and
    discards for r0 != 0, which is the original behavior. The ARM9 now does
    the same, because GBATEK says the real NDS9 BIOS always waits for at
    least one IRQ with r0 = 0. The ARM7 keeps the Phase 1 immediate return.
  - IntrWait on the TWL ARM7: upstream only leaves the loop when a main
    *and* an aux flag are set, so VBlankIntrWait (aux mask 0) can never
    return. Ours checks and acknowledges both words and returns if a flag in
    either is set (GBATEK: r2 holds extra flags). This is done by a
    `check_irq_flags` macro used at all three check points; it keeps the first
    result in r4. GBATEK's note that the real DSi7 BIOS ignores old IF flags
    is not emulated.
  - SoftReset: upstream still has the old stub. NTR only (see above); GBATEK
    lists no SWI 0x00 on DSi7/DSi9.
  - Upstream removed the `.org` padding at the end; so do we.
- Image sizes: NTR ARM9 2352 (0x930) of 4096, NTR ARM7 8672 (0x21E0) of
  16384, TWL ARM9 2228 (0x8B4) of 65536, TWL ARM7 8692 (0x21F4) of 65536.
  Upstream's are 1876 / 8240 / 1916 / 8328.
- [x] `.gitattributes` has `freebios/*.sh text eol=lf`, so build.sh and
  check.sh keep LF on Windows checkouts with core.autocrlf=true.
- Not wired into the app: DSi mode in the app still uses the user's DSi
  BIOS and NAND dumps (the NAND needs keys from the real ARM7i BIOS), and
  `Args.h` still defaults the DSi BIOS to `BrokenBIOS`, as upstream. Upstream
  uses the TWL images as the default DSi BIOS when "external DSi BIOS" is
  off, together with a generated DSi firmware and NAND-less boot (also in
  #2523: `DSi::NeedsDirectBoot()`, optional NAND in `DSiArgs`, `DSi_SD`,
  `SPI_Firmware`). That only boots DSi-mode homebrew from the cart slot; no
  DSiWare, no NAND, no DSi crypto SWIs (0x20-0x29 are stubs). For the app it
  would at most allow DSi homebrew without dumps, and only after porting the
  NAND-less boot changes too. DS games in DSi mode run with the NTR BIOS, so
  they would gain nothing.
- Regression check (PC swi-harness, Dragon Quest IX, 1800 frames, DS mode
  and DSi mode with direct boot, DSi dumps and a fresh NAND copy per run):
  - melonade 9bd2090b vs this change with FreeBIOS: identical `outputs.bin`
    and screenshots, no SWI result differences. DQ9 calls neither IntrWait,
    VBlankIntrWait nor SoftReset. The only change is timing: the ARM9 CpuSet
    moved 16 bytes down in the BIOS (0x2D4 to 0x2C4, because IntrWait got
    shorter) and takes 68 instead of 70 cycles, which shifts one ARM7 Halt
    across the end of the run and a frame boundary in `frames.tsv`.
  - Official BIOS vs new FreeBIOS: no data-SWI result differences, the same
    timing-only wait-SWI differences as before Phase 3.
  - Advance Wars DS (ARM9 VBlankIntrWait, 1595 calls): no result
    differences; median cycles official 1089969, old FreeBIOS 1089970.5, new
    1089974. VBlankIntrWait uses r0 = 1, so the ARM9 IntrWait change does not
    apply to it. None of the test ROMs calls IntrWait with r0 = 0.
  - None of the nine other test ROMs (Nintendogs, Pokemon Diamond, Advance
    Wars DS, Kirby Super Star Ultra, Phantom Hourglass, Mario Kart DS, New
    Super Mario Bros., Rune Factory, Animal Crossing) calls a decompression
    SWI in its first 1800 frames without input, so HuffUnComp and
    RLE-to-VRAM are still untested in a real game. With L+R+START+SELECT held
    at frame 1500, none of them calls SWI 0x00 on either BIOS (the games
    reset in their own code), so SoftReset is untested in a real game too.
  - The TWL images are only checked by rebuild and disassembly.

## Status / how to resume

Phases 0-3 are complete and merged into `melonade` (4452c00e) and picked up
by the app. The PC swi-harness shows no result differences against the
official BIOS for Dragon Quest IX in DS and DSi mode. On the phone, 13 games
launched and ran with FreeBIOS as the DS-mode BIOS in DSi mode (2026-11-02),
but none of them calls SoftReset, HuffUnComp or RLE-to-VRAM.

Suggested test order on the phone:
1. Any game with FreeBIOS, no native BIOS: boots and runs (IntrWait change,
   SWI table change).
2. A game that soft-resets to its title/menu (SoftReset).
3. Games known to use BIOS decompression (Huffman SWI 0x13, RLE-to-VRAM
   SWI 0x15): graphics should no longer be missing or corrupt. None of the
   current test ROMs uses them at startup.

Open items:
- `check.sh` is not wired into CI.
- Divide by zero: 0/0 matches the official BIOS (r0=1, r1=0, r3=1); x/0
  returns r0=0, r1=x, r3=0 where the official BIOS hangs (SWI test ROM).
- The ARM9 SoftReset cache step and step order are an interpretation of
  GBATEK (see Phase 2). SoftReset has not run in a real game, but the SWI test ROM matches the official BIOS except the ARM9 stack placement.
- Absent DSi SWIs return instead of hanging as GBATEK describes.
- After a failed Open, the official LZ77 (12h) and RLE (15h) callback SWIs
  pass source+1 to Close (Huffman passes source+0); FreeBIOS passes source+0
  for all three. Not matched, since there is no explanation for the +1
  (SWI test ROM #243/#244, #319/#320).

## SWI test ROM (DS mode, 2026-11-02)

`tools/swi-testrom/` is our own DS program that calls every DS-mode SWI with
known inputs (436 tests, 72 isolated hang-prone cases, 7 SoftReset variants)
and stores the results in main RAM. `sh tools/swi-testrom/build.sh` builds it
reproducibly; `sh tools/swi-testrom/run.sh` runs it in the swi-harness with
the official BIOS and with FreeBIOS and writes
`logs/swi/testrom/report.txt` in the app repo. Results store no BIOS bytes or
BIOS addresses (table SWIs by hash, BIOS-range register values as flags), and
the runs pass `--no-swi-log --no-core-log`. The official BIOS matched the
GBATEK model on every test that has one.

FreeBIOS bugs it found (all against GBATEK; all fixed 2026-11-02, see
"Test ROM fixes" below):
- Sqrt: signed compare, so inputs >= 80000000h return 0 (GBATEK: unsigned).
- CpuFastSet: rounds the count up to 8 words; NDS takes any word count.
- CpuSet count 0, BitUnPack length 0, RLE Wram size 0 and Diff8/16 size 0
  wrap around and overwrite all of main RAM (official: no writes, Diff writes
  one unit).
- The callback decompressors (12h, 13h, 15h) never call Open/Get8/Get32/
  Close, ignore Open's header and errors, and return the source pointer in r0
  instead of the length. Output is only right for plain memory sources.
- ARM7 CustomHalt writes r0 to HALTCNT instead of r2 (r0=12345678h selects GBA
  mode and stops the emulator).
- SoundBias ignores r0 and the r1 delay.
- ARM7 Halt changes r0 and r2 (GBATEK: all registers unchanged).
- IsDebugger doesn't destroy the halfword at 27FFFF8h/27FFFFAh. Harmless.

Official behavior GBATEK doesn't cover (decide keep or match): 0/0 returns
r0=1, r1=0, r3=1; x/0 hangs (FreeBIOS returns on purpose); LZ77 and RLE write
the whole last block past the size; Diff16 rounds an odd size up; RLE-to-VRAM
drops a trailing odd byte and aligns an odd destination down; ARM9 SoftReset
uses fixed stacks at 803FC0h/803FA0h/803EC0h whatever the DTCM base; invalid
SWIs jump to 0 or crash (FreeBIOS returns). SoftReset otherwise matches
(DTCM+3E00h-3FFFh cleared, CP15 control, r0-r12, lr, mode). Not FreeBIOS
bugs: CustomPost r0=3 (melonDS ignores 32-bit POSTFLG writes), out-of-range
table indices and GetBootProcs (garbage/addresses by design).

### Test ROM fixes (2026-11-02)

All bugs above are fixed, and these official behaviors are now matched too:
0/0 returns r0=1, r1=0, r3=1 (x/0 with x != 0 still returns r0=0,
r1=x, r3=0 instead of hanging); LZ77 (11h/12h) and RLE (14h/15h) write the
whole last block past the size (block at a time while the signed remaining
size is above 0; size 0 writes nothing); Diff8/Diff16 always write the first
unit (also for size 0) and Diff16 rounds an odd size up; VBlankIntrWait
returns r0 = r1 = 1 (IntrWait keeps r0/r1; its flags moved to r12).
RLE-to-VRAM (15h) now drops a trailing odd byte and aligns an odd
destination down without reading back the low byte. This reverses the
Phase 1 design (odd start kept the existing low byte, odd end was flushed
with the existing high byte). Other earlier decisions that changed: Phase 1
Div by zero (0/0 only), Phase 2 CpuFastSet rounding (now 8-word blocks then
single words, as GBATEK says for NDS) and Phase 2 HuffUnComp reading the
source directly.
- 12h/13h/15h use GBATEK's callback interface (Open, Close, Get8, Get16,
  Get32 at r3+0..10h, ARM or Thumb). Open(r0=source, r1=dest, r2=param)
  gives the header; a negative value aborts with no writes. Get8/Get32 get
  the current source address (header+4 on). Huffman copies the
  (byte+1)*2-byte tree table into the r2 temp buffer with Get8 (also for
  size 0) and reads the bitstream with Get32. Close (if non-zero) gets the
  current source address; r0 = Open error, else Close error, else the size.
  As on the official BIOS, Close is also called after a failed Open. Shared
  code: `cb_decompress` / `cb_finish` / `cb_read8` / `cb_read32` and the
  `vram_put_byte` macro. LZ77 to VRAM aligns an odd destination down, like
  RLE. In the TWL build the LZ77 entries 01h (8-bit writes, disp 0 works),
  02h and 19h are callback-based too (as GBATEK lists them); the LZ77 body
  is one macro (`lz77_callback_body`) used with `vram_put_byte` or
  `byte_put_8bit`, and the 8-bit copy is only assembled for TWL.
- `interrupt_check`/`interrupt_check_aux` set IME=0 before reading the IRQ
  flags, so an IRQ between the read and the write-back cannot lose its bit.
- Sqrt compares unsigned; ARM7 Halt keeps all registers; ARM7 CustomHalt
  writes r2; SoundBias steps the level by 1 toward 000h/200h with an r1
  countdown between steps (GBATEK gives no delay unit) and keeps bits 10-15;
  IsDebugger writes 0 to the halfword at 27FFFF8h/27FFFFAh; CpuSet,
  BitUnPack and RLE Wram return at once for a count/size of 0.
- Result: 211 mismatches before, 52 after. Left: x/0 hangs, invalid SWIs,
  CustomPost, GetBootProcs, out-of-range table indices (all expected), and
  one quirk not matched: after a failed Open the official LZ77/RLE pass
  source+1 to Close (Huffman passes source+0; ours passes source+0 for
  all). SoftReset results unchanged (ARM9 stacks DTCM-relative on purpose).
- Sizes: NTR ARM9 2552 (was 2352) of 4096, NTR ARM7 8916 (was 8672),
  TWL ARM9 2428 (was 2228), TWL ARM7 8936 (was 8692).
- Dragon Quest IX, 600 frames, official vs FreeBIOS: no data-SWI result
  differences.

Cycle differences: ARM9 WaitByLoop is about 1.5x slower (GBATEK: the NDS
BIOS runs it in Thumb), ARM9 Div 1.3x and Sqrt 1.26x slower. CpuFastSet is
faster because the official BIOS has its documented slow path.

Not covered yet: ARM7 Sleep, and the NTR ARM7 BIOS-range source check.

## SWI test ROM, DSi mode (2026-11-02)

`build.sh` also builds `build/twl/swi-testrom-twl.nds` (TWL header, `-DTWL`),
and `run.sh` runs it in DSi mode with the official DSi BIOS and with our TWL
FreeBIOS (`--dsi-bios free`), writing `logs/swi/testrom/report-dsi.txt`.
The DSi direct boot still needs the NAND and DSi firmware; in free mode the
harness passes the official bios7.bin to the NAND code for its ES key only,
the same way the app does. All DSi runs share one read-only NAND copy
(`--nand-readonly`), deleted at the end. The ROM clears every area direct
boot fills from the BIOS or NAND, hashes are salted per test and platform so
they can't be compared across tests, and pointer-0, source-check and
GetBootProcs tests are flags only. The DSi BIOS holds console keys, so SHA-1
and RSA only ever see our own data and keys (FIPS 180, textbook RSA).

666 tests (125 isolated). The official BIOS matched the GBATEK/FIPS/RSA model
everywhere except the source-check boundary cases, where it follows the
address-bit rule below. 239 mismatches, all against our TWL build:
- SHA-1 24h-29h and RSA 20h-23h are stubs (most of the mismatches).
- SWIs 2Ah and up go to table entry 01h, which on TWL is LZ77 Callback8, so
  they crash or hang. GBATEK: 40h-FFh mirror 00h-3Fh (49h = Div, CDh = Sqrt),
  12h and 2Bh are ignored. Fix: mask with 3Fh, send 2Ah-3Fh to swi_invalid.
- DSi7 CpuSet, CpuFastSet and BitUnPack reject a source whose start or end
  has none of address bits 25-27 set (end counted at 4 bytes per unit even
  for 16-bit CpuSet); the output is untouched and r0-r2 kept. DSi9 has no
  check. FreeBIOS has none.
- DSi9 IntrWait with r0=0 and the flag already set returns at once; FreeBIOS
  still waits for one IRQ (the NDS9 rule).
- DSi7 IntrWait/VBlankIntrWait waiting only on IF bits (r2=0) never return
  on the official BIOS (GBATEK's DSi7 bug); a real IF2 IRQ (AES) does end the
  wait on both. FreeBIOS returns. Decision needed; returning is harmless.
- IntrWait registers: official returns r0=1, keeps r2, flags left in r3.
- DSi7 GetPitchTable 0-3Fh returns sine values (GBATEK: bugged on DSi7).
  Entries from 40h up, out-of-range indices and the -46Ah case are marked
  "won't match, keep": matching them would mean reconstructing BIOS content.
- GetBootProcs: official XORs with r2 (GBATEK), FreeBIOS doesn't.
- GetCRC16 length 0/1: the DSi BIOS sets r3=0.
- Scratch: the official BIOS keeps r2 in every one-call SWI; FreeBIOS
  changes it in Div, Sqrt, BitUnPack, CRC and the decompressors.
- Same as DS mode: x/0, invalid SWIs below 2Ah, CustomPost, the Close
  source+1 quirk.

Redacted-spec claims confirmed: the mirrors and ignored numbers, the bits
25-27 source check, IntrWait r0=1 and r3 = flags, DSi9 Halt r0=0, CRC r3=0,
and the SHA-1/RSA edge cases. Refuted: the IntrWait "leftover bits" exit
rule, the DSi9 "r0=0 discard bug", "Finish leaves the length big-endian",
and "a rejected call leaves r0-r3 untouched" (CpuSet changes r3).

Next testing ideas (not started):
- A debug-build log line on the phone when a game calls SWI 0x00, to see
  whether games that return to their title (Dragon Quest IX after a
  quicksave) use the BIOS SoftReset or their own reset code.

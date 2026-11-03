Custom NDS ARM7/ARM9 BIOS replacement
Copyright (c) 2013, Gilead Kutnick
All rights reserved.

Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

1) Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2) Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

-- Info --

This archive contains source code and assembly for a custom BIOS replacement
for the Nintendo DS system. This code is in no way affiliated with Nintendo
and is not derived from Nintendo's BIOS implementation but has been implemented
using publically available documentation.

The source is src/bios_common.s. It is assembled four times: NTR (DS) and
TWL (DSi) builds for the ARM9 and the ARM7 (-DPLATFORM_NTR/-DPLATFORM_TWL,
-DTARGET_ARM9/-DTARGET_ARM7), as in upstream melonDS. build.sh (or `make`)
uses clang, ld.lld and llvm-objcopy by default, or a GNU ARM toolchain when
PREFIX is set, e.g. `PREFIX=arm-none-eabi- ./build.sh`. It writes
build/bios_{ntr,twl}_arm{9,7}.bin (unpadded; each must fit the BIOS size) and
regenerates ../src/FreeBIOS_Data.h, which src/FreeBIOS.cpp wraps in
FreeBIOSGetNtrArm9/7() and FreeBIOSGetTwlArm9/7(). check.sh (or `make check`)
rebuilds into a temporary directory and fails if src/FreeBIOS_Data.h does not
match the source. Never hand-edit the hex.

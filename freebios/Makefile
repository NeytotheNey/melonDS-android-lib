# Builds drastic_bios_arm7.bin / drastic_bios_arm9.bin and regenerates the
# arrays in ../src/FreeBIOS.cpp. See build.sh for toolchain selection
# (clang/lld by default, PREFIX=arm-none-eabi- for a GNU toolchain).

all:
	./build.sh

check:
	./check.sh

clean:
	rm -f *.o *.elf

.PHONY: all check clean

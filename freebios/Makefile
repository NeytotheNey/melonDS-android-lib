# Builds the NTR/TWL ARM9/ARM7 images into build/ and regenerates
# ../src/FreeBIOS_Data.h. See build.sh for toolchain selection
# (clang/lld by default, PREFIX=arm-none-eabi- for a GNU toolchain).

all:
	./build.sh

check:
	./check.sh

clean:
	rm -rf build

.PHONY: all check clean

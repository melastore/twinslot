HOSTCC ?= cc
PYTHON ?= python3
ANDROID_HOME ?= $(HOME)/Android/Sdk
NDK ?= $(lastword $(sort $(wildcard $(ANDROID_HOME)/ndk/*)))
DEVICE_CC ?= $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang

# The one version of TwinSlot: the engine prints it and the app (which carries the engine) shows it.
VERSION := $(shell cat VERSION)

SRCS := $(wildcard src/*.c)
THIRD_PARTY := \
	third_party/lz4/lz4.c third_party/lz4/lz4hc.c \
	third_party/zstd/zstddeclib.c \
	third_party/xz/xz_crc32.c third_party/xz/xz_crc64.c \
	third_party/xz/xz_dec_lzma2.c third_party/xz/xz_dec_stream.c \
	$(addprefix third_party/bzip2/,blocksort.c bzlib.c compress.c crctable.c decompress.c huffman.c randtable.c)

INCLUDES := -Isrc -Ithird_party/lz4 -Ithird_party/zstd -Ithird_party/bzip2 -Ithird_party/xz
DEFINES := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DXZ_USE_CRC64 -DXZ_DEC_ANY_CHECK \
	-DZSTD_DISABLE_ASM -DZSTD_NO_TRACE -DTWINSLOT_VERSION='"$(VERSION)"'
CFLAGS_OWN := -std=gnu11 -O2 -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror=implicit-function-declaration
CFLAGS_3P := -O2 -w

# One static binary for the phone: runs in Android, any recovery and adb shell.
# zlib (gzip ramdisks) comes from the system on the PC and from the NDK's static libz.a on the phone.
DEVICE_LDFLAGS := -static -Wl,--gc-sections -s
DEVICE_EXTRA := -ffunction-sections -fdata-sections

all: device host

device: out/twinslot
host: out/twinslot-host

define build
out/$(1)/%.o: %.c $(wildcard src/*.h) | out
	@mkdir -p $$(dir $$@)
	@echo "  CC  $$<  ($(1))"
	@$(2) $(DEFINES) $(INCLUDES) $$(if $$(filter third_party/%,$$<),$$(CFLAGS_3P),$$(CFLAGS_OWN)) $(3) -c -o $$@ $$<
endef
$(eval $(call build,host,$(HOSTCC),))
$(eval $(call build,device,$(DEVICE_CC),$(DEVICE_EXTRA)))
out/host/src/main.o out/device/src/main.o: VERSION

out:
	@mkdir -p out

out/twinslot-host: $(addprefix out/host/,$(SRCS:.c=.o) $(THIRD_PARTY:.c=.o))
	@echo "  LD  $@"
	@$(HOSTCC) -o $@ $^ -lz

out/twinslot: $(addprefix out/device/,$(SRCS:.c=.o) $(THIRD_PARTY:.c=.o))
	@test -x "$(DEVICE_CC)" || { echo "no NDK clang at $(DEVICE_CC) (set NDK=...)"; exit 1; }
	@echo "  LD  $@"
	@$(DEVICE_CC) $(DEVICE_LDFLAGS) -o $@ $^ -lz

test: host
	$(PYTHON) -I tests/run.py

zips: device
	$(PYTHON) -I tools/mkzip.py

app: device
	cd app && ./gradlew -q assembleRelease

clean:
	rm -rf out

.PHONY: all device host test zips app clean

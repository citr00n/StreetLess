ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM toolchain directory)
endif
include $(DEVKITARM)/3ds_rules

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
PORT := $(DEVKITPRO)/portlibs/3ds
CFLAGS := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections -mword-relocations -D__3DS__ -Iinclude -I../client/include -I$(CTRULIB)/include -I$(PORT)/include
LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,--gc-sections -L$(PORT)/lib -L$(CTRULIB)/lib
CURL_LIBS = $(shell $(PORT)/bin/curl-config --libs)
CA_FILE ?= ../client/romfs/certs.pem
TARGET := 0004013000F5A302
CLIENT_SOURCES := config http sync cecd journal wire selection
OBJECTS := build/main.o build/heap.o build/lifecycle.o build/ca.o $(addprefix build/client-,$(addsuffix .o,$(CLIENT_SOURCES)))

.PHONY: all clean force
all: $(TARGET).cxi

$(TARGET).cxi: build/background.elf background.rsf build/romfs/certs.pem
	makerom -f ncch -rsf background.rsf -o $@ -elf $<

build/background.elf: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(CURL_LIBS) -lctru -lm

build/%.o: source/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

build/client-sync.o: CFLAGS += -DSTREETLESS_BACKGROUND

build/client-%.o: ../client/source/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@


build/ca.o: build/romfs/certs.pem
	$(OBJCOPY) -I binary -O elf32-littlearm -B arm $< $@
build/romfs/certs.pem: $(CA_FILE) force
	@mkdir -p build/romfs
	@cmp -s "$<" "$@" || cp "$<" "$@"

force:
clean:
	rm -rf build $(TARGET).cxi

-include $(OBJECTS:.o=.d)

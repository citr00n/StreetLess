ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM toolchain directory)
endif

include $(DEVKITARM)/3ds_rules

APP_TITLE := Streetless
APP_DESCRIPTION := StreetPass over the Internet
APP_AUTHOR := Streetless
export APP_TITLE APP_DESCRIPTION APP_AUTHOR
export APP_ICON := $(CURDIR)/build/artwork/icon.png

CA_FILE ?= romfs/certs.pem
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
PORT := $(DEVKITPRO)/portlibs/3ds
CFLAGS := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections -mword-relocations -D__3DS__ -Iinclude -I$(CTRULIB)/include -I$(PORT)/include
LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,--gc-sections -L$(PORT)/lib -L$(CTRULIB)/lib
CURL_LIBS = $(shell $(PORT)/bin/curl-config --libs)
TARGET := streetless
BUILD := build
OBJECTS := $(patsubst source/%.c,$(BUILD)/%.o,$(wildcard source/*.c))
CFLAGS += -Iquirc
OBJECTS += $(patsubst quirc/%.c,$(BUILD)/quirc/%.o,$(wildcard quirc/*.c))

.PHONY: all clean force
all: $(TARGET).3dsx $(TARGET).cia

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh $(BUILD)/romfs/certs.pem
	3dsxtool $< $@ --smdh=$(TARGET).smdh --romfs=$(BUILD)/romfs

$(TARGET).smdh: $(BUILD)/artwork/.stamp
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" "$(APP_ICON)" $@

$(BUILD)/artwork/.stamp: tools/artwork.py
	python3 tools/artwork.py
	@touch $@

$(BUILD)/banner.bnr: $(BUILD)/artwork/.stamp
	bannertool makebanner -i $(BUILD)/artwork/banner.png -a $(BUILD)/artwork/banner.wav -o $@

$(TARGET).cia: $(TARGET).elf $(TARGET).smdh $(BUILD)/banner.bnr $(BUILD)/romfs/certs.pem streetless.rsf
	makerom -f cia -o $@ -rsf streetless.rsf -target t -exefslogo -elf $(TARGET).elf -icon $(TARGET).smdh -banner $(BUILD)/banner.bnr

$(TARGET).elf: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(CURL_LIBS) -ljansson -lctru -lm

$(BUILD)/%.o: source/%.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/quirc/%.o: quirc/%.c
	@mkdir -p $(BUILD)/quirc
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/romfs/certs.pem: $(CA_FILE) force
	@mkdir -p $(BUILD)/romfs
	@cmp -s "$<" "$@" || cp "$<" "$@"

force:
clean:
	rm -rf build streetless.elf streetless.smdh streetless.3dsx streetless.cia

-include $(OBJECTS:.o=.d)

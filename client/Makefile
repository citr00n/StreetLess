ifeq ($(strip $(DEVKITARM)),)
$(error Set DEVKITARM to the devkitARM toolchain directory)
endif

include $(DEVKITARM)/3ds_rules

APP_TITLE := Streetless
APP_DESCRIPTION := StreetPass over the Internet
APP_AUTHOR := Streetless
export APP_TITLE APP_DESCRIPTION APP_AUTHOR
export APP_ICON := $(CTRULIB)/default_icon.png

CA_FILE ?= romfs/certs.pem
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
PORT := $(DEVKITPRO)/portlibs/3ds
CFLAGS := $(ARCH) -O2 -g -std=gnu11 -Wall -Wextra -ffunction-sections -fdata-sections -mword-relocations -D__3DS__ -Iinclude -I$(CTRULIB)/include -I$(PORT)/include
LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,--gc-sections -L$(PORT)/lib -L$(CTRULIB)/lib
CURL_LIBS = $(shell $(PORT)/bin/curl-config --libs)
TARGET := streetless
BUILD := build
ifeq ($(TEST_SLOT_EXPORT),1)
TARGET := streetless-slot-export
BUILD := build/export
APP_TITLE := Streetless Slot Export
CFLAGS += -DSTREETLESS_TEST_SLOT_EXPORT
endif
OBJECTS := $(patsubst source/%.c,$(BUILD)/%.o,$(wildcard source/*.c))
ifneq ($(TEST_SLOT_EXPORT),1)
CFLAGS += -Iquirc
OBJECTS += $(patsubst quirc/%.c,$(BUILD)/quirc/%.o,$(wildcard quirc/*.c))
endif

.PHONY: all clean force
all: $(TARGET).3dsx

$(TARGET).3dsx: $(TARGET).elf $(TARGET).smdh $(BUILD)/romfs/certs.pem
	3dsxtool $< $@ --smdh=$(TARGET).smdh --romfs=$(BUILD)/romfs

$(TARGET).smdh:
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" "$(APP_ICON)" $@

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
	rm -rf build streetless.elf streetless.smdh streetless.3dsx streetless-slot-export.elf streetless-slot-export.smdh streetless-slot-export.3dsx

-include $(OBJECTS:.o=.d)

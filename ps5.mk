# BlueBird: PS5 payload build (ps5-payload-sdk)

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

VERSION   := $(shell sed -E -n 's/.*define BB_VERSION[[:space:]]+"([^"]+)".*/\1/p' src/version.h | tr -d '\r')
BUILD_NUM := $(shell sed -E -n 's/.*define BB_BUILD[[:space:]]+([0-9]+).*/\1/p' src/version.h | tr -d '\r')
ELF       := dist/BlueBird-$(VERSION)-b$(BUILD_NUM).elf
BUILD     := build/ps5
GEN       := $(BUILD)/gen

CFLAGS    := -std=c11 -Wall -Wextra -O2 -Isrc -Isrc/bt -Isrc/a2dp -I$(GEN)
CFLAGS    += -DBB_VERSION='"$(VERSION)"' -DBB_BUILD=$(BUILD_NUM)
LDLIBS    += -lSceSystemService -lSceAppInstUtil

SRCS := src/util.c src/log.c src/ui_text.c src/launcher.c src/web.c src/speaker.c src/main.c \
        src/bt/cmdq.c src/bt/bt_rtl.c src/bt/usb_hci.c \
        src/a2dp/avdtp.c src/a2dp/sdp.c src/a2dp/media.c src/a2dp/sbc_enc.c src/a2dp/store.c src/a2dp/host.c \
        src/audio/capture.c src/audio/stream.c

OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
HEADERS := $(wildcard src/*.h src/*/*.h)

$(ELF): $(OBJS)
	@mkdir -p dist
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@cp $@ dist/BlueBird.elf
	@echo "[+] Built $@ (and dist/BlueBird.elf)"

$(BUILD)/%.o: src/%.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# The dashboard page, embedded as a C array
$(GEN)/web_index.h: web/index.html tools/embed_web.py
	@mkdir -p $(GEN)
	python3 tools/embed_web.py $< $@ HTML_INDEX
$(BUILD)/web.o: $(GEN)/web_index.h

# The label and number are compiled in: a stamp rebuilds what shows them when they change
$(GEN)/version.stamp: FORCE
	@mkdir -p $(GEN)
	@echo '$(VERSION) $(BUILD_NUM)' | cmp -s - $@ || echo '$(VERSION) $(BUILD_NUM)' > $@
FORCE:
$(BUILD)/main.o $(BUILD)/web.o: $(GEN)/version.stamp

# Realtek firmware from linux-firmware (tools/get-bt-firmware.sh), embedded when present
RTL_DIR := firmware/rtl_bt
$(GEN)/rtl8761bu_fw.h: $(wildcard $(RTL_DIR)/rtl8761bu_fw.bin) tools/embed_web.py
	@mkdir -p $(GEN)
	python3 tools/embed_web.py --optional $(RTL_DIR)/rtl8761bu_fw.bin $@ RTL8761BU_FW
$(GEN)/rtl8761bu_config.h: $(wildcard $(RTL_DIR)/rtl8761bu_config.bin) tools/embed_web.py
	@mkdir -p $(GEN)
	python3 tools/embed_web.py --optional $(RTL_DIR)/rtl8761bu_config.bin $@ RTL8761BU_CONFIG
$(BUILD)/bt/usb_hci.o: $(GEN)/rtl8761bu_fw.h $(GEN)/rtl8761bu_config.h

# The Media tab tile
$(GEN)/launcher_icon.h: assets/launcher/icon0.png tools/embed_web.py
	@mkdir -p $(GEN)
	python3 tools/embed_web.py $< $@ LAUNCHER_ICON
$(BUILD)/launcher.o: $(GEN)/launcher_icon.h

clean:
	rm -rf $(BUILD)

.PHONY: clean FORCE

# BlueBird
#   make test   host tests (clang)
#   make        the PS5 payload (needs PS5_PAYLOAD_SDK)

BUILD := build/host
CC_HOST := clang
HFLAGS := -std=c11 -Wall -Wextra -Werror -Wframe-larger-than=8192 -O1 -g -Isrc -Isrc/bt -Isrc/a2dp -D_DEFAULT_SOURCE

.PHONY: all ps5 test clean

all: ps5

ps5:
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK is undefined: export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
endif
	$(MAKE) -f ps5.mk

A2DP := src/a2dp/avdtp.c src/a2dp/sdp.c src/a2dp/media.c src/a2dp/sbc_enc.c src/a2dp/store.c src/a2dp/host.c
BASE := src/util.c src/log.c

test:
	@mkdir -p $(BUILD)
	$(CC_HOST) $(HFLAGS) src/bt/bt_rtl.c tests/test_bt_rtl.c -o $(BUILD)/test_bt_rtl
	./$(BUILD)/test_bt_rtl
	$(CC_HOST) $(HFLAGS) src/bt/cmdq.c $(BASE) tests/test_cmdq.c -lpthread -o $(BUILD)/test_cmdq
	./$(BUILD)/test_cmdq
	$(CC_HOST) $(HFLAGS) src/a2dp/avdtp.c tests/test_avdtp.c -o $(BUILD)/test_avdtp
	./$(BUILD)/test_avdtp
	$(CC_HOST) $(HFLAGS) src/a2dp/sdp.c tests/test_sdp.c -o $(BUILD)/test_sdp
	./$(BUILD)/test_sdp
	$(CC_HOST) $(HFLAGS) src/a2dp/avdtp.c src/a2dp/media.c src/a2dp/sbc_enc.c tests/test_media.c -lm -o $(BUILD)/test_media
	./$(BUILD)/test_media
	$(CC_HOST) $(HFLAGS) src/a2dp/store.c $(BASE) tests/test_store.c -lpthread -o $(BUILD)/test_store
	./$(BUILD)/test_store
	$(CC_HOST) $(HFLAGS) $(A2DP) src/audio/stream.c src/audio/capture.c src/ui_text.c $(BASE) tests/test_host_sim.c -lpthread -lm -o $(BUILD)/test_host_sim
	./$(BUILD)/test_host_sim
	$(CC_HOST) $(HFLAGS) src/ui_text.c tests/test_ui_text.c -o $(BUILD)/test_ui_text
	./$(BUILD)/test_ui_text

clean:
	rm -rf build

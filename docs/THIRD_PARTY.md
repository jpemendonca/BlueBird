# Third-party components

BlueBird is GPL-3.0-or-later. The parts below come from elsewhere.

## OmniPad CE (GPL-3.0-or-later)

From [OmniPad CE](https://github.com/jpemendonca/OmniPad-CE), commit `f3b7261`:

- `src/bt/usb_hci.c`: the USB transport, adapter search, bring-up and Realtek loading of `src/bt_adapter.c`, without the pad handling.
- `src/bt/bt_rtl.c`, `src/bt/bt_rtl.h`, `tests/test_bt_rtl.c`: unchanged.
- `src/bt/cmdq.c`, `src/bt/cmdq.h`: the command queue of `src/bt_sched.c`, without the background search rule.
- `src/util.c`, `src/log.c`, `src/launcher.c`, `src/ui_text.c`: adapted (paths, names, texts).
- `tools/embed_web.py`, `tools/get-bt-firmware.sh`, `tools/setup-wsl-toolchain.sh`.

## HearBridge-PS5 (GPL-3.0-or-later)

From [HearBridge-PS5](https://github.com/X-F1REBALL-X/HearBridge-PS5) by X-F1REBALL-X:

- `src/a2dp/sbc_enc.c` and `src/a2dp/sbc_enc.h`: the SBC frame encoder, written by X-F1REBALL-X from the A2DP specification. BlueBird keeps the frame encoder (`hb_sbc_*`) and drops HearBridge's streaming wrapper around it.
- `src/audio/capture.c`: rewritten for BlueBird, with HearBridge's knowledge of libSceAvcap2: the function NIDs, the open parameters (the Remote Play mix), the order of rights tried, the float/int16 and channel-count checks, and the restart on a sticky invalid status.

## Realtek Bluetooth firmware

Builds embed `rtl_bt/rtl8761bu_fw.bin` and `rtl_bt/rtl8761bu_config.bin` from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git), unchanged. They are not part of this repository; `tools/get-bt-firmware.sh` downloads them with their licence, `LICENCE.rtlwifi_firmware.txt`, which releases ship next to the ELF.

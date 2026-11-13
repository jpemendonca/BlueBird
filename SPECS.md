# SPECS.md

How BlueBird works and why. Each section is the contract for its area.

## 1. Threads

| Thread | Owns | Talks to others through |
|---|---|---|
| main | start-up, the stop flag | nothing after start-up |
| Bluetooth worker (`speaker.c`) | the USB adapter, the host, the stream's packing | requests set under `g_sp.lock`, read once per loop turn |
| capture (`stream.c`) | the libSceAvcap2 session | the ring buffer, under `g_s.lock` |
| web (`web.c`) | the listening socket | `speaker_*` calls, which only set requests or read a published copy |

The host (`src/a2dp/host.c`) is not thread-safe and only the worker calls it. No lock is held during USB or HCI I/O.

Threads start through `thread_start` with a 512 KB stack and a name the log shows. The PS5's default thread stack is small: in build 1 a 115 KB structure on the worker's stack ended the payload a moment after it started. Both builds fail on a function frame over 8 KB (`-Wframe-larger-than=8192`).

## 1b. Logs and notifications

- Every log line carries the time and the thread's name. `main` logs the firmware, pid, rights and address; the worker logs every adapter it sees or cannot open, a heartbeat every 30 s, and the stream's numbers every 10 s while it plays; the capture logs its reads and peak level every 10 s; the dashboard logs every POST.
- A crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) writes the signal, the thread and the current step (`log_step`) to the log and shows a notification.
- Notifications: running (first thing at start), adapter found, adapter ready, no adapter after 20 s, adapter failed, adapter unplugged, connecting (asked from the dashboard, or the first automatic call), paired, playing, disconnected, connection failed (asked from the dashboard), capture failed, stopped.

## 2. The adapter (`src/bt/usb_hci.c`)

- Every `/dev/ugenX.Y` is opened in turn. A device of vendor `0e8d` or `1286` is closed right after its device descriptor is read: those are the console's radio.
- A device with an HCI interface (class `E0/01/01`) is an adapter. A device another app holds (open fails with `EBUSY`, OmniPad's adapter for one) is skipped. With `prefer` set to `vvvv:pppp`, that model is taken first.
- Bring-up: HCI Reset, Read Local Version. A Realtek chip (manufacturer `0x005D`, whatever its USB vendor) on ROM firmware (subversion `0x8761`) gets the embedded RTL8761B patch, as Linux `btrtl` does. The TP-Link UB500 is `2357:0604`; HearBridge checked the USB vendor `0bda` and left it on ROM firmware.
- A device with CSR's ids (`0a12:0001`) and another manufacturer is a CSR8510 clone. The host does not search while a link to it is up.
- An adapter without Bluetooth Classic (LMP feature byte 4 bit 5) is refused: A2DP needs Classic.
- An adapter that does not start is re-enumerated over USB (`USB_DEVICEENUMERATE`) up to twice, then skipped until it is unplugged.

## 3. The host (`src/a2dp/host.c`)

One link, one sink.

- Set-up: Read Buffer Size, event mask (every Classic event), Simple Pairing on, extended inquiry results, page timeout 5.12 s, default link policy role switch only (no sniff while streaming), page scan on.
- Search: General Inquiry. Devices whose class says audio (major class Audio/Video, or the Audio or Rendering service bits) are listed as speakers, others only on request. Names come from the extended inquiry result, or a Remote Name Request after the search. On a CSR clone a search that hears nothing switches to RSSI results.
- Calling a speaker: Create Connection, then Authentication Requested. Stored key first; a speaker that refuses it (`0x05`/`0x06`) is paired again once. Pairing is Secure Simple Pairing with no input and no output (Just Works), or PIN `0000` for legacy speakers. Then encryption, then AVDTP.
- A stored speaker that calls us is accepted (staying peripheral). It gets 1.5 s to pair, encrypt or open AVDTP itself; then BlueBird does what is missing. Calls from devices that are not stored are refused.
- L2CAP: basic mode only, MTU 1024 asked. Information requests are answered (fixed channels: signaling). PSMs other than AVDTP and SDP are refused (AVRCP, RFCOMM).
- AVDTP: BlueBird is initiator or acceptor, whichever side starts first. As initiator: Discover, Get Capabilities of each free audio sink, Set Configuration (SBC), Open, the media channel, Start. As acceptor it answers every signal for its one SBC source end point. A Set Configuration refused at bitpool above 35 is tried again at 35. Suspend from the sink: Start again after 2.5 s. Close or Abort: set up again after 2 s.
- SBC choice: 48 kHz before 44.1 kHz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool up to 53. A sink with 16 or 32 kHz only is refused.
- SDP: one record, an A2DP 1.3 audio source on AVDTP 1.3.
- ACL: fragments never exceed the adapter's buffer size, and never more are in flight than its buffer count. Signaling waits in a queue; a media packet without room is dropped. Three seconds without a Number Of Completed Packets event frees the buffers.

## 4. The stream (`src/audio/`)

- Capture: libSceAvcap2, the Remote Play mix, 48 kHz stereo, float32 or int16 (detected). Opened when a stream is ready, closed 30 s after the last one ends. It is a copy of the mix: the TV keeps its sound, and what goes to the controller's speaker or headset jack is not changed.
- Ring buffer: 0.5 s. Packing starts once 40 ms are in it, and after a dry ring. Above 250 ms it drops back to 60 ms.
- Pacing: one packet per packet duration (18.7 ms at bitpool 53 and MTU 895), at most 4 per turn; a stall over 200 ms restarts the clock.
- Bitpool adaptation: packets dropped in a second lower the bitpool by 6, not below 29; 10 s without drops raise it by 2, up to the configured maximum.
- Sink at 44.1 kHz: linear resampling from 48 kHz. Mono sink: L and R averaged.

## 5. Dashboard and API (`src/web.c`)

Port 8120 (8090 is HearBridge, 8095 Dump Forge, 8105 OmniPad). One request per connection. The routes are listed in `src/web.h`. The page is one file, no external requests, Portuguese and English, phone, desktop and TV (`?tv=1`). Lists are rebuilt only when they change, so the controller's focus stays put in the console's browser.

## 6. Files on the console

| Path | What |
|---|---|
| `/data/bluebird/bluebird.log` | the log, kept across runs, moved to `.old` past 512 KB |
| `/data/bluebird/speakers.txt` | paired speakers: address, link key, last use, name |
| `/data/bluebird/settings.txt` | volume, reconnect on/off, preferred adapter |
| `/data/bluebird/bluebird.pid`, `stop` | the running copy, and the flag that asks it to stop. A copy that does not stop in 3 s is ended only if its pid still carries BlueBird's process name |
| `/user/app/BLBD00001` | the Media tab tile |

## 7. Tests

`make test` runs the host tests. `tests/test_host_sim.c` plays an adapter and a speaker written from the Core, L2CAP and AVDTP specifications: fresh pairing, a stored key, a speaker that forgot its key, a speaker that calls and leads AVDTP, a speaker that is off, a suspend, and 310-byte ACL buffers like a CSR8510. A test name says `test_<function>_<scenario>_<behaviour>`.

## 8. Decisions

- **Own adapter only.** HearBridge uses the console's radio when no adapter is plugged, sharing its endpoints with the system: the system takes some of its packets, and the DualSense suffers. BlueBird has no such mode.
- **A host of its own, not AnyPad's.** OmniPad's host is built for HID pads on a shared radio. A2DP needs other channels and roles; BlueBird's host is small and covers only that, with OmniPad's lessons on credits, fragmentation and clones.
- **SBC only.** Every A2DP sink supports it. AAC, aptX and LDAC are left for later.
- **Separate from OmniPad.** One adapter cannot serve two payloads, and A2DP takes much of the radio time pads need. Running both needs two adapters.

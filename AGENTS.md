# AGENTS.md

BlueBird is a payload for jailbroken PS5 consoles, written in C. It plays the console's audio on a Bluetooth speaker or headphones through a separate USB Bluetooth adapter. The console's own Bluetooth radio stays with the DualSense, and BlueBird never touches it.

This project is developed by AI agents with one human, the maintainer, who tests on a real PS5. This file is the source of truth for agents. `CLAUDE.md` and `GEMINI.md` only point here.

BlueBird is a sibling of OmniPad CE (`../Omnipad`). Its USB adapter transport and Realtek firmware loader come from there, and so do most rules below.

## The main path

Every change is judged against this path, as the user walks it:

1. Download `BlueBird.elf` from the GitHub release.
2. Send it to the PS5 with a payload loader and **start it**. The notification "BlueBird … running/ativo" appears.
3. Plug a USB Bluetooth adapter into the PS5. The notification "Bluetooth adapter ready" appears.
4. Open the dashboard (the BlueBird tile in the Media tab, or `http://<PS5_IP>:8120`), put the speaker in pairing mode, press Search, then Connect. The notification "playing on <speaker>" appears and the game sound comes out of the speaker.
5. Next time, switching the speaker on is enough: BlueBird calls the last speaker by itself.
6. If something fails, open Diagnostics on the dashboard and copy it into a GitHub issue.

## Read before working

- `CODESTYLE.md`: always.
- `SPECS.md`: architecture, contracts, decisions. Read the section for the area you touch.
- `docs/COMECE_AQUI.md` and `README.md`: what users are told. Keep them true when behaviour changes.

## How work is done

1. Understand the request. If it touches hardware behaviour, read the PS5 log first (see "Reading the PS5").
2. Work on a branch named `feat/...` or `fix/...` off `main`. `main` only holds what was validated on the PS5.
3. Change the code following `CODESTYLE.md`.
4. Run the quality gate.
5. Build a test ELF and hand it to the maintainer. Say plainly what was checked on the PC and what still needs the PS5.
6. Commit in Conventional Commits, one logical change per commit.
7. When the maintainer confirms it works on the PS5, merge into `main` (fast-forward), push, and delete the branch.

**Code that was written is not a path that was walked.** Only the maintainer's test on the PS5 counts as walked.

## Quality gate

Run it in WSL Ubuntu, with the toolchain from `tools/setup-wsl-toolchain.sh`:

```bash
make test
```

```bash
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk && make
```

Both must pass, and the payload build must have **no warnings**. If you changed `web/index.html`, also check that its `<script>` parses with `node --check` and look at the page in the browser pane with `python tools/mock_dashboard.py`.

What the gate does not cover: it never runs on a PS5. `tests/test_host_sim.c` plays a speaker written from the specifications, not a real one. A green gate is required, but it does not prove that a real speaker pairs or that the console's audio is captured.

## Test builds and releases

- Test build: `make -f ps5.mk VERSION=<x.y.z-label> BUILD_NUM=<n>`. Rename the output to `dist/BlueBird-b<n>-test-<topic>.elf` and remove copies it supersedes.
- Release: only when the maintainer asks for one in the chat. Bump `src/version.h`, run the full gate after `make -f ps5.mk clean`, tag `vX.Y.Z` on `main`, and run `gh release create` with `dist/BlueBird.elf` and `firmware/rtl_bt/LICENCE.rtlwifi_firmware.txt`. Release notes in English, positive, listing as tested only what was tested on the PS5.

## Reading the PS5

- The agent cannot send payloads. The maintainer sends and starts them.
- The dashboard and API are at `http://<PS5_IP>:8120`. `GET /api/state` returns the adapter, the speaker link and the stream numbers. `GET /api/log` returns the last 32 KB of the log; page with `?since=<offset>` following the `X-Log-Offset` header.
- A PS5 that answers ping while port 8120 times out usually means the IP changed. Ask the maintainer for the current one.

## Hard rules

- **Never send any request to the console's internal Bluetooth radio** (MediaTek, vendor `0e8d`, or Marvell, vendor `1286`) beyond reading its device descriptor. Sharing it locks up the DualSense until the console restarts.
- **Never stop or kill another app's process.** BlueBird replaces only its own running copy, found by its pid file. HearBridge stops any `payload.elf` and anything on ports 8090-8095, which can be another payload.
- No warnings in the payload build. Fix the code, don't suppress the warning.
- Never bypass the gate, and never commit with it red.
- No new dependency or vendored code without the maintainer asking for it. Vendored code keeps its licence and is listed in `docs/THIRD_PARTY.md`.
- **Nothing goes in without a caller.** A function or route that production code never calls is not done. A test does not count as a caller.
- Never add `Co-Authored-By` or any other AI authorship line to commits, PRs or releases.
- Firmware blobs (`firmware/`) are never committed.
- Keep `README.md` and `docs/COMECE_AQUI.md` true. A user-visible change updates them in the same branch.

## Repository map

| Path | What it is |
|---|---|
| `src/main.c` | Start-up, single copy, the start notification, shutdown |
| `src/speaker.c` | The Bluetooth worker: adapter, host, reconnecting the last speaker, requests from the dashboard |
| `src/bt/usb_hci.c` | The USB adapter: finding it, Realtek firmware, chip quirks, the HCI transport over ugen |
| `src/bt/bt_rtl.c`, `src/bt/cmdq.c` | Realtek patch selection; the HCI command queue |
| `src/a2dp/host.c` | The Bluetooth Classic host for one A2DP sink: search, pairing, L2CAP, AVDTP in both roles |
| `src/a2dp/avdtp.c`, `sdp.c`, `media.c`, `store.c` | AVDTP messages and the SBC choice; the SDP record; RTP/SBC packets; paired speakers |
| `src/a2dp/sbc_enc.c` | SBC frame encoder (vendored from HearBridge-PS5) |
| `src/audio/capture.c` | The console's audio through libSceAvcap2 |
| `src/audio/stream.c` | Ring buffer, real-time pacing, volume, test tone, bitpool adaptation |
| `src/web.c`, `web/index.html` | Dashboard server and the single-file page (embedded at build time) |
| `src/launcher.c` | The Media tab tile (`/user/app/BLBD00001`) |
| `src/log.c`, `src/ui_text.c` | Log file and notifications; notification texts in seven languages |
| `tests/` | Host tests (`make test`), the speaker simulator among them |
| `tools/` | Toolchain setup, firmware download, embedder, mock dashboard, icon |

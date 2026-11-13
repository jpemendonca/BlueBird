# CODESTYLE.md

Only rules you can check by reading a diff go here.

## Language

- Everything in the project is in English: code, identifiers, comments, log lines, test names, docs, commit messages and releases. The one exception is `docs/COMECE_AQUI.md`, the Portuguese getting-started guide.
- Text the user reads goes in every language the app has: English, Portuguese, Spanish, French, German, Russian and Japanese. Notifications use the table in `src/ui_text.c`, the dashboard its `I18N` table; `make test` fails if a language lacks a text or changes its arguments or `{placeholders}` (`tools/check_i18n.py`).
- Commit messages: a Conventional Commits prefix in English (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`, `chore:`), then the description in English.

## Comments

- Only when the *why* is not obvious. Never restate the code, never leave commented-out code, never add banners or file headers.
- Short sentences, active voice, one word for one meaning. Describe the code, not the diff that produced it.
- When hardware forced a decision, say what was seen and where: `/* ... (PS5, 25 Oct) */`.

```c
/* right */
/* A speaker that called us gets this long to pair or open AVDTP itself */
#define T_PEER_FIRST 1500

/* wrong */
/* Wait 1500 ms */   // restates the code
#define T_PEER_FIRST 1500   // was 800
```

## C

- C11, built with `-Wall -Wextra`, and the payload build stays free of warnings.
- `snake_case` for functions and variables, `UPPER_CASE` for macros and constants. Public functions carry their module prefix (`usb_hci_*`, `host_*`, `stream_*`, `speaker_*`, `launcher_*`). Everything used by one file only is `static`.
- One `.h` per module, holding only what other modules call. Each declaration gets a one-line comment saying what it returns or when to call it.
- Code that needs the PS5 goes inside `#ifdef __PROSPERO__`. It gets a host stub in `#else`, so the file still builds for host tests.
- Logging: `log_line("<module>: ...")`, lower-case module prefix (`bt:`, `host:`, `stream:`, `speaker:`, `launcher:`). Log what happened and the values that explain it. Never log link keys.
- No heap allocation in the worker's loop or the capture thread. Use static or caller-owned buffers.
- No function frame over 8 KB: the build fails on it. Big buffers are static or global. Threads start with `thread_start`, never `pthread_create` directly.
- Mutex scope: never hold `g_sp.lock` or `g_s.lock` while doing USB or HCI I/O.
- Errors: functions return `0`/`1` (or `-1`/`0`) and log the cause where it happened. Don't push a failure on silently. A state that should never happen is logged too, never left to assert on the console.

```c
/* right */
if (ioctl(d->fd, USB_FS_INIT, &init) != 0) {
    log_line("bt: USB_FS_INIT failed on %s (%s)", d->info->path, strerror(errno));
    return 0;
}

/* wrong */
ioctl(d->fd, USB_FS_INIT, &init);   // result ignored
```

## Bluetooth

- An adapter quirk is set once in `bring_up` (`usb_hci.c`) as a field of `usb_hci_info_t`, and checked where it matters.
- `src/a2dp/sbc_enc.c` is vendored. Keep changes small, mark them with `/* BlueBird: ... */`, and list them in `docs/THIRD_PARTY.md`.
- Every protocol step that waits for the other side has a timeout and a log line saying what was not answered.

## Dashboard (`web/index.html`)

- One self-contained file, no external requests, no build step. The build embeds it into the payload.
- Every visible string goes in every language of `I18N`.
- The page has one theme, light, in the icon's blue; it ignores the system's dark mode. New elements must work at phone width and in TV mode (`html.tv`).
- Lists go through `setHtml`, never `innerHTML` on every poll: the console's browser loses the controller's focus on a rebuild.

## Tests

- Name: `test_<function>_<scenario>_<behaviour>`.
- Body: `/* arrange */`, `/* act */` and `/* assert */` comments, in that order. This is the one exception to the comment rule.
- Every `CHECK` carries a readable message saying what it guarantees. That message is what the report prints when the check fails.

```c
static void test_sbc_choose_16kOnly_refused(void)
{
    /* arrange */
    const uint8_t ie[4] = { 0x81, 0x15, 2, 53 };
    sbc_choice_t c;

    /* act */
    int ok = sbc_choose(ie, SBC_BITPOOL_HQ, &c);

    /* assert */
    CHECK(!ok, "a sink with only 16 kHz is refused: the capture is 48 kHz");
}
```

- A test file is plain C with `tests/check.h`, and a `main` that ends with `REPORT("<name>")`. Add it to the `test` target in `Makefile`.

## Git

- Conventional Commits, one logical change per commit. Never commit with the gate red.
- Branches are `feat/...` or `fix/...` off `main`. Merge them by fast-forward once the change is validated on the PS5, then delete the branch.
- Never add a `Co-Authored-By` or AI attribution line.

<p align="right">
  <a href="known-pitfalls.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Known Pitfalls and Verification Gaps

Failure modes hit while extracting the application framework, splitting the
repositories, and validating on a physical device. Each entry gives the
symptom, why the normal checks missed it, the rule that prevents it, and how
it is detected now. Normative rules live in
[`coding-conventions.md`](coding-conventions.md) and
[`../ai-guide.md`](../ai-guide.md); this document records why those rules
exist.

## 1. The portal fragment injection marker hijacked itself

**Symptom.** The provisioning portal served a page in which the application's
configuration card was entirely absent. No other part of the UI was affected,
the firmware built, and every host test passed.

**Cause.** `handler_index` substitutes the application fragment with a naive
first-substring match on the injection marker. A literal mention of that marker
anywhere earlier in the template — most easily inside an HTML comment that
explains where the injection point is — becomes the match target. The fragment
is written into that comment, and the real container keeps the raw marker.

**Why the gate stayed green.** A string substitution inside a served document
is invisible to the compiler, to host tests, and to a firmware build. Only
fetching the page from a running device reveals it.

**Rules.**

- The injection marker must appear exactly once in the template.
- The shared helpers must be defined before the injection point, in an earlier
  script block. Otherwise a fragment's inline `<script>` runs before the
  helpers exist, and the failure is silent.
- A fragment must not redeclare the helpers. A global `const` redeclared across
  two classic script blocks is a `SyntaxError` that disables both blocks.

**Detection.** `tests/test_portal_template.py` asserts all three rules and the
framework gate runs it; the test was verified to fail when the marker is
duplicated. `handler_index` also counts the marker and logs an `ERROR` when the
count is not exactly one, so a regression surfaces in the device log instead
of only in a browser.

## 2. A green gate does not prove a rendered page

Any change to HTML, CSS, or portal JavaScript is unverified until the page is
fetched from a running device. Report this separately from the build result:

```text
Build:        PASS
Host tests:   PASS
Device tests: NOT RUN   <-- portal markup and injection still unverified
```

Fetching the page is cheap — the portal is resident HTTP on port 80 and its
address is on the device screen. Do it. Check that the injected fragment landed
inside its container, that no raw marker survived, and that the shared helpers
appear before the fragment.

## 3. Extraction is not finished while the framework carries product vocabulary

The framework was extracted from a single application, and the extraction
looked complete in the C sources while the portal HTML template still held the
product's organization discovery UI, its private endpoints, its brand string
in the export filename, and its field names. The same feature existed twice:
once in the framework template, once in the application card. The dead copy was
unreachable — it called endpoints that no handler registered and DOM nodes that
did not exist — so nothing failed loudly.

**Rules.**

- A framework directory is finished only when it contains no product
  vocabulary, field names, or brand strings, markup included.
- Dead application code in the framework is worse than none: it documents a
  feature the application never had.
- A feature promised in a README that no handler serves is a defect, not a
  placeholder. Return a legible error from a real endpoint instead of leaving
  the page to hit an unexplained 404, and do not invent the missing backend.

**Detection.** `tests/test_portal_template.py` rejects product vocabulary in
the template. Add the same kind of assertion when a new shared asset appears.

## 4. Reading the device partition table before choosing a flash scope

A merged image at `0x0` resets stored configuration; component images at their
configured offsets preserve it. Choosing between them without evidence risks
either losing settings or writing an application where it does not belong.

Read the device's partition table and compare it byte for byte against the
build's before writing components. Identical bytes mean the layout is
unchanged and per-offset flashing is safe. Record the comparison in the
delivery report.

## 5. esptool command shapes changed in 4.x

- `write_flash` takes its flash options **after** the subcommand:
  `esptool --chip esp32c3 -p PORT write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 file.bin`.
- `read_flash` takes address, then size, then filename:
  `esptool --chip esp32c3 -p PORT read_flash 0x9000 0x6000 out.bin`.
- An argument error fails during parsing, so nothing is written. Read the error
  before retrying; a rejected command never reached the device.

## 6. Verify a backup by reading it twice

A single successful read does not prove a backup is usable. Read the
partition twice into different files, compare hashes, and only then trust it.
Report the hash so a later restore can be checked against it.

Keep a human-readable copy alongside the raw image. A raw partition dump holds
plaintext credentials and is not a readable inventory; the portal's
`/api/config/export` is the supported readable form. Redact secrets in any copy
that will be shared, and keep the raw image out of version control.

## 7. A partial configuration import clears the Wi-Fi list

`apply_import_root` rebuilds the hotspot table unconditionally. If the posted
JSON has no `networks` array, the list is saved empty and every saved hotspot is
lost, even though the request reports success. A full export round-trip is
safe because the export always includes the array; a hand-written or partial
payload is not.

Treat a missing `networks` key in an import payload as "leave the list alone"
rather than "clear it". Until that is changed, import only complete exports.

## 8. Moving files breaks more than the moved files

Splitting the framework and application apart touched file locations and broke
four things that had no obvious connection to the move:

- a checker derived its repository root from the script's own path, so an
  application repository reusing it inspected the framework instead;
- a required-file list still demanded build inputs the framework no longer
  owned;
- host tests included BSP sources by hard-coded relative path;
- documentation linked to paths that had moved or to files owned by the other
  repository.

After any relocation, grep for the old path across scripts, tests, and
documents, and run the gate on **both** repositories. A framework is also
useless if its own gate cannot run, so verify the framework repository passes
standalone, not only from inside an application.

## 9. A deprecated Kconfig key silently loses to the new key's default

`sdkconfig.defaults` set `CONFIG_LV_MEM_SIZE_KILOBYTES=24` to keep the LVGL
malloc pool small. LVGL 9 also accepts `CONFIG_LV_MEM_SIZE`, whose Kconfig
default is 65536, and it resolves the conflict in favour of `LV_MEM_SIZE`. The
project therefore ran with a 64 KB pool instead of 24 KB, spending about 40 KB
of a roughly 107 KB heap without a single build error.

The only symptom was a `#warning` buried in a long compile log, which nobody
reads. It stayed invisible until a second application needed about 40 KB for an
MP3 decoder and every decode failed with `ESP_AUDIO_ERR_MEM_LACK (-2)`.

Rules:

- When moving off a deprecated Kconfig key, set the replacement in the same
  change. Do not keep the old key "just in case".
- After changing a `sdkconfig.defaults` value, read the value back out of the
  generated `sdkconfig`. A defaults file is an input, not evidence.
- Delete `sdkconfig` (or run `idf.py fullclean`) when a defaults change appears
  to do nothing, so Kconfig is forced to resolve again.
- Budget the heap deliberately. On a part without PSRAM, record free heap both
  idle and under load, and treat a tens-of-KB gap as a defect, not a setting.

## 10. A streaming decoder is not fed one frame at a time

A socket returns byte runs that do not line up with MP3 frame boundaries. The
bare `esp_mp3_dec_decode` in `esp_audio_codec` documents that it expects
frame-boundary input; when the trailing bytes are a partial frame it returns
`ESP_AUDIO_ERR_DATA_LACK (-3)` or `ESP_AUDIO_ERR_FAIL (-1)` instead of
consuming them. Two plausible workarounds both fail the same way:

- decode once per read and drop the unconsumed tail loses half a frame per
  block, heard as continuous stutter;
- skip one byte and call `esp_mp3_dec_reset` on every error, so a complete
  frame never reaches the decoder and a healthy stream is killed.

Use the Simple Decoder instead (`esp_audio_simple_dec_*` with
`use_frame_dec = false`). It parses frame boundaries and caches a partial
frame across calls, and `OK` means "decoded, or buffered internally". Even then,
keep advancing by `raw.consumed`: one call does not necessarily consume the
whole input block.

## 11. A counter reset inside the loop that tests it never terminates

An ICY block is "read exactly `icy-metaint` audio bytes, then read the metadata
block that follows". The implementation fed the decoder after every read and
reset the buffer counter to zero each time, while the loop condition tested
that same counter against `metaint`. Because `metaint > 0`, the condition held
on every pass, so the loop never exited and the metadata block was never read.

The stream still played, which is why it survived casual testing: the decoder
was fed audio with metadata interleaved and resynchronised on frame headers.
The only visible defect was a permanently empty "now playing" title.

Keep the count of bytes consumed from the socket separate from the length
handed to the decoder, and never reset the one the loop tests.

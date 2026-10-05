<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# appfw default Chinese fonts

LVGL's built-in fonts (Montserrat) have no CJK glyphs, so Chinese UI text
needs a shipped font. This directory is the **framework-level default**:
the complete Modern Chinese Common Characters table (3500), plus the rare
characters applications actually use (place names, traditional station
names), plus ASCII and common full-width punctuation — 4333 hanzi in total.

| File | Purpose |
| --- | --- |
| `appfw_common_charset.txt` | the charset table (single source of truth) |
| `app_font_16.c` / `app_font_24.c` | generated output, **do not edit by hand** |
| `gen_fonts.py` | generator script (needs Node/npx) |

## Why the full set

The earlier approach embedded only the characters found in source strings:
every text change required regenerating the fonts, and a missed step showed
blank glyphs on screen. With the full set, UI copy changes, title changes and
server-provided Chinese text all render without touching the fonts. The cost
is flash: both sizes together are about 1.8 MB, acceptable on 16 MB devices.

## Link model

`appfw_ui.c` references `app_font_16`/`app_font_24` weakly;
`appfw/CMakeLists.txt` pulls the defaults into the link with `-u` anchors:

- An app **without** its own fonts gets the framework defaults (full common
  coverage);
- An app with **same-name strong symbols** (its own subset, before migration)
  overrides the defaults; the framework copy is reclaimed by
  `--gc-sections` and costs no flash.

## Regenerating

```bash
cd appfw/fonts
curl -L -o NotoSansSC-Regular.otf \
  "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
python3 gen_fonts.py        # the 7.9 MB source font is not committed
```

Only needed when adding characters that are outside
`appfw_common_charset.txt` (ordinary copy changes never require it).
Commit the generated files; apps then bump the submodule pointer.

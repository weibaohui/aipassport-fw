<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# appfw default Chinese fonts

LVGL's built-in fonts (Montserrat) have no CJK glyphs, so Chinese UI text
needs a shipped font. This directory is the **framework-level default**.
The 16 px body font covers every GB2312 graphic character, printable
ASCII/space, and the framework's existing extra characters — 8151 unique
Unicode code points in total. The 24 px title font keeps the smaller common
charset: the complete Modern Chinese Common Characters table (3500), plus rare
characters applications actually use (place names, traditional station names),
plus ASCII and common full-width punctuation.

| File | Purpose |
| --- | --- |
| `appfw_gb2312_charset.txt` | 16 px charset: GB2312 ∪ ASCII/space ∪ common extras |
| `appfw_common_charset.txt` | 24 px charset and the base list of framework extras |
| `app_font_16.c` / `app_font_24.c` | generated output, **do not edit by hand** |
| `gen_fonts.py` | generator script (needs Node/npx) |

`gen_fonts.py` also records explicit glyph remaps. For example, Noto Sans SC has
U+25B6 (`▶`) but not the small cursor U+25B8 (`▸`); the generator remaps the
supported glyph so the requested cursor is actually emitted instead of being
silently dropped.

## Why the GB2312 body font

The earlier approach embedded only the characters found in source strings:
every text change required regenerating the fonts, and a missed step showed
blank glyphs on screen. With the full set, UI copy changes, title changes and
server-provided Chinese text all render without touching the fonts. GB2312
also covers the characters users normally choose for runtime-created names.

16 px is used by lists and editable/user-visible dynamic content, so it gets
the wider coverage. 24 px remains scoped to framework titles, where the smaller
charset avoids another full CJK-sized copy. Together the linked 16/24 px font
data is about 2.5 MB of Flash. GB2312 is only the source inventory: firmware
strings remain UTF-8.

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
`appfw_gb2312_charset.txt` for 16 px or `appfw_common_charset.txt` for 24 px
(ordinary copy changes never require it).
Commit the generated files; apps then bump the submodule pointer.

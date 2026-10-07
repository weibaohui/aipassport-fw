#!/usr/bin/env python3
"""Check the framework's 16 px font contract: GB2312 plus retained extras."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "appfw" / "fonts"


def load_generator():
    spec = importlib.util.spec_from_file_location("appfw_gen_fonts", FONT_DIR / "gen_fonts.py")
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load gen_fonts.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    errors: list[str] = []
    gb_path = FONT_DIR / "appfw_gb2312_charset.txt"
    common_path = FONT_DIR / "appfw_common_charset.txt"
    font_path = FONT_DIR / "app_font_16.c"
    for path in (gb_path, common_path, font_path):
        if not path.is_file():
            errors.append(f"missing generated asset: {path.relative_to(ROOT)}")
    if errors:
        print("GB2312 font checks failed:")
        print("\n".join(f"  {error}" for error in errors))
        return 1

    generator = load_generator()
    expected = generator.gb2312_charset()
    actual = gb_path.read_text(encoding="utf-8")
    common = common_path.read_text(encoding="utf-8").replace("\n", "").replace("\r", "")

    if actual != expected:
        errors.append(
            f"charset file is stale: {len(set(actual))} unique code points, "
            f"expected {len(set(expected))}"
        )
    if "\n" in actual or "\r" in actual:
        errors.append("charset must be a single line without CR/LF")
    if " " not in actual:
        errors.append("charset must contain U+0020")
    if not set(common).issubset(set(actual)):
        errors.append("charset must retain every character from appfw_common_charset.txt")
    if "▸" in actual:
        header = font_path.read_text(errors="ignore")
        header = header[:header.find("******/")]
        if "0x25b6=>0x25b8" not in header.lower():
            errors.append("font generator must remap U+25B8 from supported U+25B6")

    if errors:
        print("GB2312 font checks failed:")
        print("\n".join(f"  {error}" for error in errors))
        return 1

    print(f"GB2312 font charset: PASS ({len(set(actual))} unique code points)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

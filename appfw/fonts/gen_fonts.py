#!/usr/bin/env python3
"""生成框架默认中文字库(app_font_16.c / app_font_24.c)。

字符集不是从源码扫出来的,而是固定清单 appfw_common_charset.txt:
《现代汉语常用字表》全部 3500 字 + 各应用实际用到的生僻字(地名/繁体台名等)
+ ASCII + 常用全角标点。目的:改 UI 文案、换标题、显示服务端返回的中文,
都不必重新生成字库,也不会出现方框。只有引入清单之外的新字符才需要重跑本脚本。

用法:
    cd appfw/fonts
    curl -L -o NotoSansSC-Regular.otf \
      "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
    python3 gen_fonts.py

生成物入库(编译机器不需要 Node);源字体 7.9MB 不入库。
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OTF = HERE / "NotoSansSC-Regular.otf"
CHARSET = HERE / "appfw_common_charset.txt"


def generate(size: int, out: Path) -> None:
    syms = CHARSET.read_text(encoding="utf-8").strip()
    subprocess.run(
        [
            "npx", "--yes", "lv_font_conv@1.5.3",
            "--font", str(OTF), "--size", str(size), "--format", "lvgl", "--bpp", "4",
            "--lv-include", "lvgl.h", "--no-compress", "--force-fast-kern-format",
            "--symbols", syms, "-o", str(out),
        ],
        check=True,
    )
    print(f"  {out.name}: {out.stat().st_size} 字节")


def main() -> int:
    if not OTF.is_file():
        sys.exit(f"缺少源字体 {OTF.name},见脚本头部的下载命令")
    print("生成字体(需要 npx,首次会下载 lv_font_conv):")
    generate(16, HERE / "app_font_16.c")
    generate(24, HERE / "app_font_24.c")
    print("完成。提交生成物。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

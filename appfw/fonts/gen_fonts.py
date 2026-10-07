#!/usr/bin/env python3
"""生成框架默认中文字库(app_font_16.c / app_font_24.c)。

字符集不是从源码扫出来的,而是固定清单:
- 16px:appfw_gb2312_charset.txt = GB2312 全部 7445 个图形字符 + ASCII/空格
  + 现有常用字清单中的额外字符,覆盖用户/服务端提供的常见中文内容。
- 24px:appfw_common_charset.txt = 常用字 + 各应用实际用到的生僻字 +
  ASCII + 常用全角标点。24px 只用于少量标题,保持较小的 Flash 占用。

GB2312 只用于确定“要包含哪些 Unicode 字形”的字符表;固件文本仍使用 UTF-8。

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
GB_CHARSET = HERE / "appfw_gb2312_charset.txt"


def gb2312_charset() -> str:
    """重建 GB2312 全量清单,并保留现有清单里的框架专用字符。

    GB2312 的双字节区按 A1A1-F7FE 展开;Python 运行时负责映射到 Unicode。
    显式加入 ASCII 可读区间和空格,避免不同实现把空格当成控制字符忽略。
    """
    decoded: set[str] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                decoded.add(bytes((high, low)).decode("gb2312"))
            except UnicodeDecodeError:
                # 区码表中存在少量保留空位;GB2312 定义的有效图形字符是 7445 个。
                pass
    if len(decoded) != 7445:
        raise RuntimeError(f"GB2312 图形字符数异常: {len(decoded)} != 7445")

    common = CHARSET.read_text(encoding="utf-8").replace("\n", "").replace("\r", "")
    ascii_printable = "".join(chr(cp) for cp in range(0x20, 0x7F))
    return "".join(dict.fromkeys(ascii_printable + "".join(sorted(decoded)) + common))


def generate(size: int, out: Path, charset_path: Path) -> None:
    # 不能 strip:空格(0x20)排序后在清单首位,strip 会把它吞掉,
    # 字体就没有空格字形,屏上每个空格都变方框(2026-10-06 真机踩坑)。
    syms = charset_path.read_text(encoding="utf-8").replace("\n", "").replace("\r", "")
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
    GB_CHARSET.write_text(gb2312_charset(), encoding="utf-8")
    print("生成字体(需要 npx,首次会下载 lv_font_conv):")
    generate(16, HERE / "app_font_16.c", GB_CHARSET)
    generate(24, HERE / "app_font_24.c", CHARSET)
    print("完成。提交生成物。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

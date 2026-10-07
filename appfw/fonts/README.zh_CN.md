<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# appfw 默认中文字库

LVGL 自带字体(Montserrat)没有汉字,中文界面必须自带字库。本目录是**框架级
默认字库**。16px 正文字库覆盖 GB2312 全部图形字符、可打印 ASCII/空格以及框架
既有额外字符,共 8151 个唯一 Unicode 码点。24px 标题字库保留较小常用清单:
《现代汉语常用字表》全部 3500 字 + 各应用实际用到的生僻字(地名、繁体台名等)
+ ASCII + 常用全角标点。

| 文件 | 说明 |
| --- | --- |
| `appfw_gb2312_charset.txt` | 16px 字符清单:GB2312 ∪ ASCII/空格 ∪ 常用额外字符 |
| `appfw_common_charset.txt` | 24px 字符清单,也是框架额外字符的基础清单 |
| `app_font_16.c` / `app_font_24.c` | 生成物,**不要手改** |
| `gen_fonts.py` | 生成脚本(需要 Node/npx) |

`gen_fonts.py` 还维护显式字形映射。例如 Noto Sans SC 有 U+25B6(`▶`)但没有
小光标 U+25B8(`▸`);生成器会把可用字形映射到请求码点,避免 `lv_font_conv`
静默丢字。

## 为什么 16px 用 GB2312 全量

早期做法是"从源码扫子集":改一句文案就得重新生成字库,漏一步屏幕上就是
一片空白。全量之后,改文案、换标题、显示服务端返回的中文都不再依赖字库。
GB2312 还能覆盖用户在运行时自建名称常用的汉字。

16px 用于列表和动态内容,所以使用更宽的覆盖;24px 只用于少量框架标题,保留
较小清单,避免再复制一份全量 CJK。链接后的 16px+24px 字体数据约 2.5 MB
Flash。GB2312 只用于确定字符清单;固件文本仍然使用 UTF-8。

## 链接模型

`appfw_ui.c` 通过弱引用使用 `app_font_16`/`app_font_24`;
`appfw/CMakeLists.txt` 用 `-u` 把本目录的默认字库强制拉进链接:

- 应用**没有**自带字库 → 直接用框架默认(常见字全覆盖);
- 应用自带**同名强符号**(自有子集字库,老应用迁移前)→ 应用那份生效,
  框架这份被 `--gc-sections` 回收,不占额外 flash。

## 重新生成

```bash
cd appfw/fonts
curl -L -o NotoSansSC-Regular.otf \
  "https://github.com/googlefonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansSC-Regular.otf"
python3 gen_fonts.py        # 7.9MB 源字体不入库,下载一次即可
```

只有向 16px 的 `appfw_gb2312_charset.txt` 或 24px 的
`appfw_common_charset.txt` 添加清单外的新字符时才需要重跑(改普通文案不需要)。
生成后提交生成物,应用仓更新子模块指针。

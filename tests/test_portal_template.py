#!/usr/bin/env python3
"""门户 HTML 模板结构契约。

handler_index 用朴素的首个子串匹配把注入标记替换成应用片段。这意味着:

1. 标记字面量在整个模板里必须【恰好出现一次】。多一次(最典型的是写在了
   HTML 注释里)会让应用片段被塞进错误位置,真机表现为"配置卡片整个不
   显示",而编译与常规主机测试都发现不了。
2. 公共助手必须定义在注入点之前,否则应用片段里的内联 <script> 在解析
   时拿不到 $, esc, jget, jpost,状态回显会静默失效。
3. 模板属于框架层,不得出现任何应用业务词汇或品牌串。
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "appfw" / "src" / "appfw_portal_html.inc"
MARKER = "<!--APP_CONFIG_HTML-->"

# 框架层禁止出现的业务词汇。出现即意味着业务代码漏进了框架。
FORBIDDEN = (
    "bigmodel", "GLM", "glm", "智谱", "用量", "套餐", "API Key",
    "orgsel", "projsel", "glmDiscover",
)


def template_text() -> str:
    """把 C 字符串字面量还原成最终 HTML。

    .inc 里每行是 "....\\n" 形式的 C 字符串。直接当普通文本搜索会因为转义
    序列而漏配,所以先解开引号与 \\n,拿到浏览器真正看到的字节序列。
    """
    raw = TEMPLATE.read_text(encoding="utf-8")
    parts = re.findall(r'^"(.*)"$', raw, flags=re.MULTILINE | re.DOTALL)
    joined = "".join(parts)
    return (
        joined.replace('\\"', '"')
        .replace("\\n", "\n")
        .replace("\\'", "'")
    )


class PortalTemplateContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.html = template_text()

    def test_template_file_exists(self) -> None:
        self.assertTrue(TEMPLATE.is_file(), f"缺少门户模板: {TEMPLATE}")

    def test_injection_marker_appears_exactly_once(self) -> None:
        count = self.html.count(MARKER)
        self.assertEqual(
            count, 1,
            f"注入标记应恰好出现 1 次,实际 {count} 次。多出的出现会让应用片段"
            f"被替换到错误位置(例如写在 HTML 注释里),真机页面上的应用配置"
            f"卡片会整个不显示。",
        )

    def test_helpers_precede_injection_point(self) -> None:
        for helper in ("const $=id=>", "const esc=", "async function jget",
                       "async function jpost"):
            self.assertIn(helper, self.html, f"缺少门户公共助手: {helper}")
        for helper in ("const $=id=>", "const esc=", "async function jget",
                       "async function jpost"):
            self.assertLess(
                self.html.index(helper), self.html.index(MARKER),
                f"{helper} 必须定义在注入点之前,应用片段才能在解析时用到它",
            )

    def test_helpers_are_not_redeclared_after_injection(self) -> None:
        tail = self.html[self.html.index(MARKER):]
        self.assertNotIn(
            "const $=id=>", tail,
            "注入点之后重复声明 const $ 会触发 "
            "SyntaxError: Identifier '$' has already been declared",
        )

    def test_template_carries_no_application_vocabulary(self) -> None:
        for word in FORBIDDEN:
            self.assertNotIn(
                word, self.html,
                f"门户模板属于框架层,不应出现应用业务词汇 {word!r};"
                f"该内容应由应用经注入点提供",
            )

    def test_card_numbering_is_contiguous(self) -> None:
        # 阶段二的框架卡片依次为:1 刷新与熄屏 / 2 WiFi 设置 / 3 其他。
        # 应用卡片由应用自行编号(通常是 0),不参与连续性检查。
        for number, title in ((1, "刷新与熄屏"), (2, "WiFi 设置"), (3, "其他")):
            self.assertIn(f"{number} · {title}", self.html,
                          f"阶段二缺少 {number} 号框架卡片「{title}」")
        self.assertNotRegex(
            self.html, r"<h2>\s*[45] · ",
            "框架卡片编号出现断档(4/5),说明历史遗留了已迁走的应用卡片",
        )


if __name__ == "__main__":
    sys.exit(unittest.main())

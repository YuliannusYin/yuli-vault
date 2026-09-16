// coding: utf-8
// =============================================================================
// MarkdownUtil.h
//
// 轻量级 Markdown → HTML 转换器。覆盖Notes字段常用语法：
//   - 标题：# / ## / ###
//   - 粗体：**text**
//   - 斜体：*text*
//   - 行内代码：`code`
//   - 链接：[text](url)
//   - 无序列表：- / * 开头
//   - 有序列表：1. 开头
//   - 代码块：```fenced```
//   - 段落：其余文本
//
// 输出 HTML 供 QTextBrowser 渲染。不依赖第三方库，符合 AGENTS.md「不引入新依赖」原则。
// =============================================================================
#pragma once

#include <QString>
#include <string>

namespace yuli::vault::ui {

/// 将 markdown 源码转换为 HTML 字符串。
/// \param md markdown 源码（UTF-8）
/// \return HTML 文本，可直接传给 QTextBrowser::setHtml
QString markdown_to_html(const std::string& md);

/// 重载：QString 输入。
QString markdown_to_html(const QString& md);

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// test_markdown_util.cpp
//
// MarkdownUtil 单元测试。覆盖：
//   - escape_html 转义引号（" '）与基本实体（& < >）
//   - 链接 url 协议白名单：http/https/mailto 放行，javascript:/file:// 拒绝
//   - 含引号的 url 不会破坏 href 属性（防 XSS 注入）
//
// 测试框架：GoogleTest。
// =============================================================================
#include <gtest/gtest.h>

#include "MarkdownUtil.h"

#include <QString>

using yuli::vault::ui::markdown_to_html;

// =============================================================================
// escape_html 行为
// =============================================================================

TEST(MarkdownUtilTest, EscapesDoubleQuote) {
    const QString html = markdown_to_html(QStringLiteral("text\"name"));
    EXPECT_TRUE(html.contains(QStringLiteral("&quot;")));
}

TEST(MarkdownUtilTest, EscapesSingleQuote) {
    const QString html = markdown_to_html(QStringLiteral("text'name"));
    EXPECT_TRUE(html.contains(QStringLiteral("&#39;")));
}

TEST(MarkdownUtilTest, EscapesQuotes) {
    const QString html = markdown_to_html(QStringLiteral("text\"name'val"));
    EXPECT_TRUE(html.contains(QStringLiteral("&quot;")));
    EXPECT_TRUE(html.contains(QStringLiteral("&#39;")));
}

TEST(MarkdownUtilTest, PreservesBasicEntities) {
    const QString html = markdown_to_html(QStringLiteral("a < b > c & d"));
    EXPECT_TRUE(html.contains(QStringLiteral("&lt;")));
    EXPECT_TRUE(html.contains(QStringLiteral("&gt;")));
    EXPECT_TRUE(html.contains(QStringLiteral("&amp;")));
}

// =============================================================================
// 链接协议白名单
// =============================================================================

TEST(MarkdownUtilTest, HttpLinkAllowed) {
    const QString html = markdown_to_html(QStringLiteral("[click](http://example.com)"));
    EXPECT_TRUE(html.contains(QStringLiteral("<a href=\"http://example.com\">click</a>")));
}

TEST(MarkdownUtilTest, HttpsLinkAllowed) {
    const QString html = markdown_to_html(QStringLiteral("[click](https://example.com)"));
    EXPECT_TRUE(html.contains(QStringLiteral("<a href=\"https://example.com\">click</a>")));
}

TEST(MarkdownUtilTest, MailtoLinkAllowed) {
    const QString html = markdown_to_html(QStringLiteral("[mail](mailto:a@b.com)"));
    EXPECT_TRUE(html.contains(QStringLiteral("<a href=\"mailto:a@b.com\">mail</a>")));
}

TEST(MarkdownUtilTest, JavascriptLinkRejected) {
    const QString html = markdown_to_html(QStringLiteral("[x](javascript:alert(1))"));
    EXPECT_FALSE(html.contains(QStringLiteral("<a href")));
    // 非法协议原样输出文本
    EXPECT_TRUE(html.contains(QStringLiteral("[x](javascript:alert(1))")));
}

TEST(MarkdownUtilTest, FileLinkRejected) {
    const QString html = markdown_to_html(QStringLiteral("[f](file:///etc/passwd)"));
    EXPECT_FALSE(html.contains(QStringLiteral("<a href")));
    EXPECT_TRUE(html.contains(QStringLiteral("[f](file:///etc/passwd)")));
}

TEST(MarkdownUtilTest, DataLinkRejected) {
    const QString html = markdown_to_html(QStringLiteral("[d](data:text/html,<script>)"));
    EXPECT_FALSE(html.contains(QStringLiteral("<a href")));
}

// =============================================================================
// 含引号的 url 不应破坏 href 属性
// =============================================================================

TEST(MarkdownUtilTest, QuoteInUrlDoesNotBreakHref) {
    const QString html = markdown_to_html(QStringLiteral("[x](http://example.com/?a=\"b)"));
    // url 中的 " 应被转义为 &quot;，href 属性内不应出现裸 "
    const int href_pos = html.indexOf(QStringLiteral("href=\""));
    ASSERT_GE(href_pos, 0);
    const int href_end = html.indexOf(QStringLiteral("\">"), href_pos);
    ASSERT_GT(href_end, href_pos);
    const QString href_content = html.mid(href_pos + 6, href_end - (href_pos + 6));
    EXPECT_FALSE(href_content.contains('"'));
    // 转义后的 &quot; 应在 href 内出现
    EXPECT_TRUE(href_content.contains(QStringLiteral("&quot;")));
}

TEST(MarkdownUtilTest, SingleQuoteInUrlDoesNotBreakHref) {
    const QString html = markdown_to_html(QStringLiteral("[x](http://example.com/?a='b)"));
    const int href_pos = html.indexOf(QStringLiteral("href=\""));
    ASSERT_GE(href_pos, 0);
    const int href_end = html.indexOf(QStringLiteral("\">"), href_pos);
    ASSERT_GT(href_end, href_pos);
    const QString href_content = html.mid(href_pos + 6, href_end - (href_pos + 6));
    // ' 被转义为 &#39;，href 内不应出现裸 '
    EXPECT_FALSE(href_content.contains('\''));
    EXPECT_TRUE(href_content.contains(QStringLiteral("&#39;")));
}

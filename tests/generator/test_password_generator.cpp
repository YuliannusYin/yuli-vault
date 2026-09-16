// coding: utf-8
// =============================================================================
// test_password_generator.cpp
//
// PasswordGenerator 单元测试（GoogleTest）。
// 覆盖：
//   - 长度正确性
//   - 字符集隔离（仅大写、仅自定义）
//   - exclude_ambiguous 生效
//   - 错误路径（空池、length=0、length=1025）
//   - 两次生成不同（随机性）
//   - estimate_strength 熵值估算
// =============================================================================
#include "PasswordGenerator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using yuli::vault::core::ErrorCode;
using yuli::vault::core::PasswordGeneratorOptions;
using yuli::vault::generator::PasswordGenerator;

namespace {

/// 检查 \p s 中所有字符是否都落在 [lo, hi] 范围内
bool all_in_range(const std::string& s, char lo, char hi) {
    return std::all_of(s.begin(), s.end(),
                       [&](char c) { return c >= lo && c <= hi; });
}

/// 检查 \p s 中所有字符是否都属于 \p set
bool all_in_set(const std::string& s, const std::string& set) {
    return std::all_of(s.begin(), s.end(),
                       [&](char c) { return set.find(c) != std::string::npos; });
}

}  // namespace

// ---------------------------------------------------------------------------
// 长度与基本正确性
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, DefaultOptionsProduceRequestedLength) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;  // 默认 length=16，四个 use_* 全 true
    auto r = gen.generate(opts);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_EQ(r.value().size(), opts.length);
}

TEST(PasswordGeneratorTest, OnlyUppercaseProducesOnlyAtoZ) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.use_uppercase = true;
    opts.use_lowercase = false;
    opts.use_digits = false;
    opts.use_symbols = false;
    opts.custom_chars.clear();
    opts.length = 64;
    auto r = gen.generate(opts);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(all_in_range(r.value(), 'A', 'Z'))
        << "password contained non-uppercase: " << r.value();
}

// ---------------------------------------------------------------------------
// exclude_ambiguous
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, ExcludeAmbiguousRemovesIl1Lo0O) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.exclude_ambiguous = true;
    opts.length = 128;  // 较长以提高置信度
    auto r = gen.generate(opts);
    ASSERT_TRUE(r.ok()) << r.error().what();
    const std::string ambiguous = "il1Lo0O";
    for (char c : r.value()) {
        EXPECT_EQ(ambiguous.find(c), std::string::npos)
            << "found ambiguous char '" << c << "' in password: " << r.value();
    }
}

// ---------------------------------------------------------------------------
// 自定义字符集
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, CustomCharsOnlyWhenNoStandardSets) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.use_uppercase = false;
    opts.use_lowercase = false;
    opts.use_digits = false;
    opts.use_symbols = false;
    opts.custom_chars = "abc";
    opts.length = 32;
    auto r = gen.generate(opts);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(all_in_set(r.value(), "abc"))
        << "password contained chars outside 'abc': " << r.value();
}

// ---------------------------------------------------------------------------
// 错误路径
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, EmptyPoolReturnsError) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.use_uppercase = false;
    opts.use_lowercase = false;
    opts.use_digits = false;
    opts.use_symbols = false;
    opts.custom_chars.clear();
    auto r = gen.generate(opts);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(PasswordGeneratorTest, ZeroLengthReturnsError) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.length = 0;
    auto r = gen.generate(opts);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(PasswordGeneratorTest, TooLargeLengthReturnsError) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.length = 1025;  // 上限 1024
    auto r = gen.generate(opts);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// 随机性
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, TwoGenerationsDiffer) {
    PasswordGenerator gen;
    PasswordGeneratorOptions opts;
    opts.length = 32;
    auto r1 = gen.generate(opts);
    auto r2 = gen.generate(opts);
    ASSERT_TRUE(r1.ok()) << r1.error().what();
    ASSERT_TRUE(r2.ok()) << r2.error().what();
    // 32 位长度下碰撞概率约 1/94^32，可视为不可能
    EXPECT_NE(r1.value(), r2.value());
}

// ---------------------------------------------------------------------------
// estimate_strength
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, EstimateStrengthLowercase8) {
    PasswordGenerator gen;
    // 8 位随机小写（避开顺序/键盘序列），8 * log2(26) ≈ 37.6 bit（Weak 区间）
    auto est = gen.estimate_strength("kxqmzbvr");
    EXPECT_NEAR(est.bits, 37, 2);
    EXPECT_EQ(est.level, yuli::vault::core::StrengthLevel::Weak);
    EXPECT_EQ(est.score, static_cast<int>(yuli::vault::core::StrengthLevel::Weak));
    EXPECT_TRUE(est.warnings.empty()) << "随机小写密码无模式警告";
}

TEST(PasswordGeneratorTest, EstimateStrengthAllClasses16) {
    PasswordGenerator gen;
    // 16 * log2(94) ≈ 104.87 bits（落在 VeryStrong 区间 >= 100）
    auto est = gen.estimate_strength("Ab1!Cd2!Ef3!Gh4!");
    EXPECT_NEAR(est.bits, 105, 3);
    EXPECT_EQ(est.level, yuli::vault::core::StrengthLevel::VeryStrong);
    EXPECT_EQ(est.score, static_cast<int>(yuli::vault::core::StrengthLevel::VeryStrong));
}

TEST(PasswordGeneratorTest, EstimateStrengthEmptyPassword) {
    PasswordGenerator gen;
    auto est = gen.estimate_strength("");
    EXPECT_EQ(est.bits, 0);
    EXPECT_EQ(est.level, yuli::vault::core::StrengthLevel::VeryWeak);
    EXPECT_EQ(est.score, 0);
    EXPECT_TRUE(est.warnings.empty());
}

// ---------------------------------------------------------------------------
// 模式检测
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, EstimateStrengthDetectsRepeatedChars) {
    PasswordGenerator gen;
    // "aaaaaa" 含 6 连续重复字符
    auto est = gen.estimate_strength("aaaaaa");
    // 应触发"连续重复字符"警告
    bool has_repeat_warning = false;
    for (const auto& w : est.warnings) {
        if (w.find("repeat:") != std::string::npos) has_repeat_warning = true;
    }
    EXPECT_TRUE(has_repeat_warning) << "应检测到连续重复字符";
}

TEST(PasswordGeneratorTest, EstimateStrengthDetectsSequentialAbc) {
    PasswordGenerator gen;
    // "abcdef" 含 6 位升序序列，纯熵 6*log2(26)≈28.2 bit，模式惩罚后落入 VeryWeak
    auto est = gen.estimate_strength("abcdef");
    bool has_seq_warning = false;
    for (const auto& w : est.warnings) {
        if (w.find("sequential:") != std::string::npos) has_seq_warning = true;
    }
    EXPECT_TRUE(has_seq_warning) << "应检测到顺序字符序列";
}

TEST(PasswordGeneratorTest, EstimateStrengthDetectsDescendingSequence) {
    PasswordGenerator gen;
    // "4321" 含 4 位降序序列
    auto est = gen.estimate_strength("4321");
    bool has_seq_warning = false;
    for (const auto& w : est.warnings) {
        if (w.find("sequential:") != std::string::npos) has_seq_warning = true;
    }
    EXPECT_TRUE(has_seq_warning);
}

TEST(PasswordGeneratorTest, EstimateStrengthDetectsKeyboardSequence) {
    PasswordGenerator gen;
    // "qwerty12" 含键盘序列 "qwerty"（6 位）
    auto est = gen.estimate_strength("qwerty12");
    bool has_kb_warning = false;
    for (const auto& w : est.warnings) {
        if (w.find("keyboard:") != std::string::npos) has_kb_warning = true;
    }
    EXPECT_TRUE(has_kb_warning);
}

TEST(PasswordGeneratorTest, EstimateStrengthDetectsReversedKeyboardSequence) {
    PasswordGenerator gen;
    // "ytrewq" 是 "qwerty" 的反向，也应被检测
    auto est = gen.estimate_strength("ytrewq");
    bool has_kb_warning = false;
    for (const auto& w : est.warnings) {
        if (w.find("keyboard:") != std::string::npos) has_kb_warning = true;
    }
    EXPECT_TRUE(has_kb_warning);
}

TEST(PasswordGeneratorTest, EstimateStrengthPenaltyReducesBits) {
    PasswordGenerator gen;
    // 对比：随机短小写密码 vs 含键盘序列的相同长度密码
    auto plain = gen.estimate_strength("kxmqzpa");
    auto with_kb = gen.estimate_strength("qwertyz");
    EXPECT_LT(with_kb.bits, plain.bits)
        << "键盘序列惩罚应使 bits 低于纯随机同长度密码";
}

TEST(PasswordGeneratorTest, EstimateStrengthScoreMatchesLevel) {
    PasswordGenerator gen;
    // 任意密码的 score 应等于 level 的整数值
    auto est1 = gen.estimate_strength("x");
    EXPECT_EQ(est1.score, static_cast<int>(est1.level));
    auto est2 = gen.estimate_strength("Xy9!kM2$pQr#7LwZ");
    EXPECT_EQ(est2.score, static_cast<int>(est2.level));
}

TEST(PasswordGeneratorTest, EstimateStrengthLevelThresholds) {
    PasswordGenerator gen;
    // 4 位随机小写 4*log2(26)≈18.8 → VeryWeak (<28)
    EXPECT_EQ(gen.estimate_strength("kxqm").level,
              yuli::vault::core::StrengthLevel::VeryWeak);
    // 8 位小写+数字 8*log2(36)≈41.4 → Weak (>=28, <50)
    EXPECT_EQ(gen.estimate_strength("kxqmzbv9").level,
              yuli::vault::core::StrengthLevel::Weak);
    // 16 位大小写+数字+符号 ≈ 105 bit → VeryStrong (>=100)
    EXPECT_EQ(gen.estimate_strength("Ab1!Cd2!Ef3!Gh4!").level,
              yuli::vault::core::StrengthLevel::VeryStrong);
}

// ---------------------------------------------------------------------------
// 字符分布不均模式检测
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, EstimateStrengthDetectsUnevenDistribution) {
    PasswordGenerator gen;
    // "XyXyX"：X 占 3/5=60% > 50%，5 字符，触发"分布不均"
    {
        auto est = gen.estimate_strength("XyXyX");
        EXPECT_FALSE(est.warnings.empty());
        bool has_uneven = false;
        for (const auto& w : est.warnings) {
            if (w.find("uneven") != std::string::npos) has_uneven = true;
        }
        EXPECT_TRUE(has_uneven) << "应检测到字符分布不均";
        // 与等长均匀分布密码对比，bits 应更低
        auto uniform = gen.estimate_strength("XkYmZ");  // 5 位大小写，无模式
        EXPECT_LT(est.bits, uniform.bits)
            << "分布不均惩罚应使 bits 低于均匀分布的等长密码";
    }
    // "abacaba"：a 占 4/7≈57% > 50%，7 字符，触发"分布不均"
    {
        auto est = gen.estimate_strength("abacaba");
        EXPECT_FALSE(est.warnings.empty());
        bool has_uneven = false;
        for (const auto& w : est.warnings) {
            if (w.find("uneven") != std::string::npos) has_uneven = true;
        }
        EXPECT_TRUE(has_uneven);
        auto uniform = gen.estimate_strength("kxqmzbp");  // 7 位小写，无模式
        EXPECT_LT(est.bits, uniform.bits);
    }
}

// ---------------------------------------------------------------------------
// Medium / Strong 等级覆盖
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, EstimateStrengthMediumLevel) {
    PasswordGenerator gen;
    // 9 位大小写+数字，pool=62，熵=9*log2(62)≈53.6 bit（Medium 区间 50~70）
    auto est = gen.estimate_strength("aB3dE7fH2");
    EXPECT_EQ(est.level, yuli::vault::core::StrengthLevel::Medium);
    EXPECT_EQ(est.score, static_cast<int>(yuli::vault::core::StrengthLevel::Medium));
    EXPECT_TRUE(est.warnings.empty()) << "该密码不应触发任何模式惩罚";
}

TEST(PasswordGeneratorTest, EstimateStrengthStrongLevel) {
    PasswordGenerator gen;
    // 12 位大小写+数字+符号，pool=94，熵=12*log2(94)≈78.5 bit（Strong 区间 70~100）
    auto est = gen.estimate_strength("aB3dE7fH2#kL");
    EXPECT_EQ(est.level, yuli::vault::core::StrengthLevel::Strong);
    EXPECT_EQ(est.score, static_cast<int>(yuli::vault::core::StrengthLevel::Strong));
    EXPECT_TRUE(est.warnings.empty()) << "该密码不应触发任何模式惩罚";
}

// ---------------------------------------------------------------------------
// 阈值边界值（50 / 70 / 100）
// ---------------------------------------------------------------------------

TEST(PasswordGeneratorTest, EstimateStrengthThresholdBoundaries) {
    PasswordGenerator gen;
    // 50 bit 边界：8 位大小写+数字 ≈ 47.6 bit（Weak）；9 位 ≈ 53.6 bit（Medium）
    EXPECT_EQ(gen.estimate_strength("aB3dE7fH").level,
              yuli::vault::core::StrengthLevel::Weak);
    EXPECT_EQ(gen.estimate_strength("aB3dE7fH2").level,
              yuli::vault::core::StrengthLevel::Medium);
    // 70 bit 边界：10 位大小写+数字+符号 ≈ 65.5 bit（Medium）；11 位 ≈ 72.1 bit（Strong）
    EXPECT_EQ(gen.estimate_strength("aB3dE7fH2#").level,
              yuli::vault::core::StrengthLevel::Medium);
    EXPECT_EQ(gen.estimate_strength("aB3dE7fH2#k").level,
              yuli::vault::core::StrengthLevel::Strong);
    // 100 bit 边界：15 位 ≈ 98.3 bit（Strong）；16 位 ≈ 104.9 bit（VeryStrong）
    EXPECT_EQ(gen.estimate_strength("aB3dE7fH2#kL9mQ").level,
              yuli::vault::core::StrengthLevel::Strong);
    EXPECT_EQ(gen.estimate_strength("aB3dE7fH2#kL9mQr").level,
              yuli::vault::core::StrengthLevel::VeryStrong);
}

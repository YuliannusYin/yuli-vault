// coding: utf-8
// =============================================================================
// PasswordGenerator.cpp
//
// 密码生成引擎实现。
//
// RNG 选型：Windows 原生 BCryptGenRandom（密码学安全，由 CNG 提供）。
//   - 调用形式：BCryptGenRandom(NULL, buf, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
//   - 无需额外依赖，仅链接系统库 bcrypt.lib
//   - 线程安全，适合服务进程并发调用
//
// modulo bias 消除：rejection sampling
//   - 取 uint32_t 随机值 r，仅当 r 落在 [0, threshold) 时接受
//   - threshold = 2^32 - (2^32 % pool_size)，保证取模后各类等概率
// =============================================================================
#include "PasswordGenerator.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace yuli::vault::generator {

namespace {

// ---------------------------------------------------------------------------
// 字符集常量
// ---------------------------------------------------------------------------
constexpr std::string_view kUppercase = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view kLowercase = "abcdefghijklmnopqrstuvwxyz";
constexpr std::string_view kDigits    = "0123456789";
constexpr std::string_view kSymbols   = "!@#$%^&*()-_=+[]{};:,.<>?/";
constexpr std::string_view kAmbiguous = "il1Lo0O";

// estimate_strength 中假设的符号池大小（ASCII 可打印标点 32 个：!..~）
constexpr size_t kEstimatedSymbolPool = 32;
constexpr size_t kEstimatedAsciiPool  = 94;

/// 使用 BCryptGenRandom 生成一个密码学安全的 32 位无符号整数。
/// \return 成功时返回随机值；失败时返回 std::nullopt
std::optional<uint32_t> secure_random_uint32() {
    uint32_t value = 0;
    NTSTATUS status = BCryptGenRandom(
        nullptr,
        reinterpret_cast<PUCHAR>(&value),
        sizeof(value),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        return std::nullopt;
    }
    return value;
}

/// 从 [0, pool_size) 中无偏采样一个索引。
///
/// 使用 rejection sampling 消除 modulo bias：
///   range     = 2^32（uint32_t 取值总数）
///   threshold = range - (range % pool_size)（最大可安全取模的边界）
///   仅当 r < threshold 时接受，否则重采样
///
/// \param pool_size 池大小（必须 > 0）
/// \return 成功时返回 [0, pool_size) 内的索引；RNG 失败时返回 std::nullopt
std::optional<size_t> unbiased_index(size_t pool_size) {
    if (pool_size == 0) {
        return std::nullopt;
    }
    // 用 64 位运算避免 2^32 溢出
    const uint64_t range = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1ULL;
    const uint64_t threshold = range - (range % static_cast<uint64_t>(pool_size));
    while (true) {
        auto r = secure_random_uint32();
        if (!r) {
            return std::nullopt;
        }
        if (static_cast<uint64_t>(*r) < threshold) {
            return static_cast<size_t>(*r) % pool_size;
        }
        // 落入 biased 区间，重采样
    }
}

/// 从字符串 \p chars 中无偏选取一个字符。
/// \return 成功时返回字符；RNG 失败或 chars 为空时返回 std::nullopt
std::optional<char> pick_char(std::string_view chars) {
    auto idx = unbiased_index(chars.size());
    if (!idx) {
        return std::nullopt;
    }
    return chars[*idx];
}

/// 从 \p source 中剔除 \p to_remove 中出现的字符。
std::string remove_chars(std::string_view source, std::string_view to_remove) {
    std::string result;
    result.reserve(source.size());
    for (char c : source) {
        if (to_remove.find(c) == std::string_view::npos) {
            result.push_back(c);
        }
    }
    return result;
}

}  // namespace

core::Result<std::string> PasswordGenerator::generate(const core::PasswordGeneratorOptions& options) {
    // 1. 构建各启用的字符集（保留独立副本用于"每集至少一个"保证）
    std::vector<std::string> enabled_sets;
    if (options.use_uppercase) {
        enabled_sets.emplace_back(kUppercase);
    }
    if (options.use_lowercase) {
        enabled_sets.emplace_back(kLowercase);
    }
    if (options.use_digits) {
        enabled_sets.emplace_back(kDigits);
    }
    if (options.use_symbols) {
        enabled_sets.emplace_back(kSymbols);
    }

    // 2. 排除易混字符（同时作用于各子集与总池，避免"保证"阶段选到已剔除字符）
    if (options.exclude_ambiguous) {
        for (auto& s : enabled_sets) {
            s = remove_chars(s, kAmbiguous);
        }
    }

    // 3. 构建总字符池 = 各启用集 ∪ 自定义字符
    std::string pool;
    for (const auto& s : enabled_sets) {
        pool += s;
    }
    pool += options.custom_chars;

    if (options.exclude_ambiguous) {
        pool = remove_chars(pool, kAmbiguous);
    }

    // 4. 校验
    if (pool.empty()) {
        return core::Result<std::string>::Err(
            {core::ErrorCode::InvalidArgument, "character pool is empty"});
    }
    if (options.length == 0) {
        return core::Result<std::string>::Err(
            {core::ErrorCode::InvalidArgument, "length must be > 0"});
    }
    if (options.length > 1024) {
        return core::Result<std::string>::Err(
            {core::ErrorCode::InvalidArgument, "length too large"});
    }

    // 5. 生成密码
    std::string password;
    password.reserve(options.length);

    // 5a. 保证每种已启用且非空的字符集至少出现一个字符（长度允许时）
    for (const auto& s : enabled_sets) {
        if (password.size() >= options.length) {
            break;
        }
        if (s.empty()) {
            continue;
        }
        auto c = pick_char(s);
        if (!c) {
            return core::Result<std::string>::Err(
                {core::ErrorCode::CryptoError, "BCryptGenRandom failed"});
        }
        password.push_back(*c);
    }

    // 5b. 用总池填充剩余长度
    while (password.size() < options.length) {
        auto c = pick_char(pool);
        if (!c) {
            return core::Result<std::string>::Err(
                {core::ErrorCode::CryptoError, "BCryptGenRandom failed"});
        }
        password.push_back(*c);
    }

    // 5c. Fisher-Yates 打乱，避免前 N 位固定为各集代表字符
    for (size_t i = password.size(); i > 1; --i) {
        auto j_opt = unbiased_index(i);
        if (!j_opt) {
            return core::Result<std::string>::Err(
                {core::ErrorCode::CryptoError, "BCryptGenRandom failed"});
        }
        size_t j = *j_opt;
        std::swap(password[i - 1], password[j]);
    }

    return core::Result<std::string>::Ok(std::move(password));
}

core::StrengthEstimate PasswordGenerator::estimate_strength(const std::string& password) {
    core::StrengthEstimate result;
    if (password.empty()) {
        return result;  // bits=0, level=VeryWeak, score=0
    }

    bool has_upper = false;
    bool has_lower = false;
    bool has_digit = false;
    bool has_symbol = false;
    bool has_other = false;

    // ASCII 可打印标点全集（33..126 之间的非字母数字字符），共 32 个
    const std::string symbol_chars = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    for (char c : password) {
        if (c >= 'A' && c <= 'Z') {
            has_upper = true;
        } else if (c >= 'a' && c <= 'z') {
            has_lower = true;
        } else if (c >= '0' && c <= '9') {
            has_digit = true;
        } else if (symbol_chars.find(static_cast<unsigned char>(c)) != std::string::npos) {
            has_symbol = true;
        } else {
            has_other = true;
        }
    }

    // 1) 纯熵估算：含未知字符时保守按完整 ASCII 池（94）
    size_t pool_size = 0;
    if (has_other) {
        pool_size = kEstimatedAsciiPool;
    } else {
        if (has_upper)  pool_size += 26;
        if (has_lower)  pool_size += 26;
        if (has_digit)  pool_size += 10;
        if (has_symbol) pool_size += kEstimatedSymbolPool;
    }

    double entropy = 0.0;
    if (pool_size > 0) {
        entropy = static_cast<double>(password.size())
                  * std::log2(static_cast<double>(pool_size));
    }

    // 2) 模式检测：每命中一条按比例扣减 bits 并追加 warning
    double penalty_factor = 1.0;
    constexpr double kPerPatternPenalty = 0.15;  // 每条扣 15%

    // 2a) 重复字符：连续相同字符 run >= 3（如 "aaa"、"111"）
    {
        size_t max_run = 1;
        size_t cur_run = 1;
        for (size_t i = 1; i < password.size(); ++i) {
            if (password[i] == password[i - 1]) {
                ++cur_run;
                max_run = std::max(max_run, cur_run);
            } else {
                cur_run = 1;
            }
        }
        if (max_run >= 3) {
            penalty_factor -= kPerPatternPenalty;
            result.warnings.push_back("repeat:" + std::to_string(max_run));
        }
    }

    // 2b) 分布不均：单一字符出现次数 > 50%
    {
        std::array<int, 256> counts{};
        for (unsigned char c : password) {
            counts[c]++;
        }
        int max_count = 0;
        for (int c : counts) {
            max_count = std::max(max_count, c);
        }
        const double ratio =
            static_cast<double>(max_count) / static_cast<double>(password.size());
        if (ratio > 0.5 && password.size() >= 4) {
            penalty_factor -= kPerPatternPenalty;
            result.warnings.push_back("uneven");
        }
    }

    // 2c) 顺序序列：连续 ASCII 升序/降序（差 1）长度 >= 3（如 "abc"、"321"）
    {
        size_t max_seq = 1;
        size_t cur_asc = 1, cur_desc = 1;
        for (size_t i = 1; i < password.size(); ++i) {
            const int diff = static_cast<int>(static_cast<unsigned char>(password[i]))
                             - static_cast<int>(static_cast<unsigned char>(password[i - 1]));
            if (diff == 1) {
                ++cur_asc;
                max_seq = std::max(max_seq, cur_asc);
                cur_desc = 1;
            } else if (diff == -1) {
                ++cur_desc;
                max_seq = std::max(max_seq, cur_desc);
                cur_asc = 1;
            } else {
                cur_asc = cur_desc = 1;
            }
        }
        if (max_seq >= 3) {
            penalty_factor -= kPerPatternPenalty;
            result.warnings.push_back("sequential:" + std::to_string(max_seq));
        }
    }

    // 2d) 键盘序列：匹配预设键盘行（长度 >= 3 的子串，正反向均检测）
    {
        // 4 行常见键盘序列（小写匹配）
        static constexpr std::string_view kKeyboardRows[] = {
            "qwertyuiop",
            "asdfghjkl",
            "zxcvbnm",
            "1234567890",
        };
        std::string lower;
        lower.reserve(password.size());
        for (char c : password) {
            lower.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }
        bool hit = false;
        size_t hit_len = 0;
        for (std::string_view row : kKeyboardRows) {
            const std::string srow(row);
            // 正向
            for (size_t i = 0; i + 3 <= lower.size(); ++i) {
                std::string sub = lower.substr(i, 3);
                if (srow.find(sub) != std::string::npos
                    || std::string(srow.rbegin(), srow.rend()).find(sub) != std::string::npos) {
                    // 扩展到最长命中
                    size_t len = 3;
                    while (i + len + 1 <= lower.size()) {
                        std::string ext = lower.substr(i, len + 1);
                        if (srow.find(ext) != std::string::npos
                            || std::string(srow.rbegin(), srow.rend()).find(ext) != std::string::npos) {
                            ++len;
                        } else {
                            break;
                        }
                    }
                    hit = true;
                    hit_len = std::max(hit_len, len);
                }
            }
        }
        if (hit) {
            penalty_factor -= kPerPatternPenalty;
            result.warnings.push_back("keyboard:" + std::to_string(hit_len));
        }
    }

    if (penalty_factor < 0.0) penalty_factor = 0.0;
    entropy *= penalty_factor;
    if (entropy < 0.0) entropy = 0.0;

    result.bits = static_cast<int>(entropy);
    result.level = core::StrengthEstimate::level_from_bits(result.bits);
    result.score = static_cast<int>(result.level);
    return result;
}

}  // namespace yuli::vault::generator

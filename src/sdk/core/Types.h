// coding: utf-8
// =============================================================================
// Types.h
//
// Yuli Vault SDK core types and public aliases. Engine interfaces
// (crypto/storage/generator) and the protocol layer are built on these types.
//
// Minimum C++ standard is C++20 (std::span).
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yuli::vault::core {

/// Owned byte vector for ciphertext, IVs, tags, and keys.
using ByteVec = std::vector<std::byte>;

/// Non-owning read-only byte view for function arguments.
using ByteSpan = std::span<const std::byte>;

/// Vault item kind. Login is implemented in this release; other values are
/// reserved so later item editors can land without another schema break.
enum class VaultItemType : uint8_t {
    Login = 1,
    SecureNote = 2,
    Card = 3,
    Identity = 4,
    Custom = 5,
};

/// Payload encoding version stored at byte 0 of an item payload.
inline constexpr uint8_t kItemPayloadVersion = 1;

inline constexpr std::string_view vault_item_type_name(VaultItemType type) noexcept {
    switch (type) {
        case VaultItemType::Login:      return "login";
        case VaultItemType::SecureNote: return "note";
        case VaultItemType::Card:       return "card";
        case VaultItemType::Identity:   return "identity";
        case VaultItemType::Custom:     return "custom";
    }
    return "unknown";
}

inline bool is_known_vault_item_type(uint8_t raw) noexcept {
    return raw >= static_cast<uint8_t>(VaultItemType::Login) &&
           raw <= static_cast<uint8_t>(VaultItemType::Custom);
}

/// Tag shared by multiple vault items.
///
/// `name` is unique in the vault (case-sensitive). `color` is `#RRGGBB` or empty.
/// `id == 0` means the tag has not been persisted yet.
struct Tag {
    int64_t id = 0;
    std::string name;
    std::string color;
    int64_t created_at = 0;
    int64_t updated_at = 0;

    bool is_new() const { return id == 0; }
};

/// Unified vault item.
///
/// Common header: `id` / `type` / `title` / `tags` / timestamps.
/// Login fields (`account` / `username` / `password` / `website` / `note`) are
/// populated in memory after decrypt. On disk they live in `payload`.
///
/// When a program password is enabled, ServiceCore encrypts the whole payload
/// (not just the password) and fills `iv` / `tag`. Title, type, and tags stay
/// plaintext for listing. `id == 0` means the item has not been persisted yet.
struct VaultItem {
    int64_t id = 0;
    VaultItemType type = VaultItemType::Login;
    std::string title;
    std::string account;
    std::string username;
    std::string password;
    std::string website;
    std::string note;
    std::vector<Tag> tags;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    ByteVec iv;
    ByteVec tag;
    ByteVec payload;

    bool is_new() const { return id == 0; }
};

/// Compatibility alias used by existing login-oriented call sites and tests.
using PasswordEntry = VaultItem;

/// Search query.
struct SearchQuery {
    std::string text;
    /// Field names: title (alias: entry_name), account, username, website, note.
    /// Empty means search all of those fields.
    std::vector<std::string> fields;
    bool case_sensitive = false;
    /// Tag filter (OR). Empty means do not filter by tag.
    std::vector<int64_t> tag_ids;
};

/// Password generator options.
struct PasswordGeneratorOptions {
    size_t length = 16;
    bool use_uppercase = true;
    bool use_lowercase = true;
    bool use_digits = true;
    bool use_symbols = true;
    std::string custom_chars;
    bool exclude_ambiguous = false;
};

/// Password strength band from estimated entropy bits.
enum class StrengthLevel : uint8_t {
    VeryWeak   = 0,  ///< < 28
    Weak       = 1,  ///< 28 .. 50
    Medium     = 2,  ///< 50 .. 70
    Strong     = 3,  ///< 70 .. 100
    VeryStrong = 4,  ///< >= 100
};

/// Strength estimate. `warnings` hold stable English keys (not localized text)
/// so a Qt UI can translate them. Keys: repeat:<n>, uneven, sequential:<n>,
/// keyboard:<n>.
struct StrengthEstimate {
    int bits = 0;
    StrengthLevel level = StrengthLevel::VeryWeak;
    int score = 0;
    std::vector<std::string> warnings;

    static StrengthLevel level_from_bits(int bits) {
        if (bits < 28)  return StrengthLevel::VeryWeak;
        if (bits < 50)  return StrengthLevel::Weak;
        if (bits < 70)  return StrengthLevel::Medium;
        if (bits < 100) return StrengthLevel::Strong;
        return StrengthLevel::VeryStrong;
    }
};

/// Generator history record. Same encrypt-at-rest rules as vault item payloads.
struct GeneratedPasswordRecord {
    int64_t id = 0;
    std::string password;
    int32_t length = 0;
    int64_t created_at = 0;
    ByteVec iv;
    ByteVec tag;
};

inline constexpr int32_t kGeneratorLimitUnlimited = 0;

}  // namespace yuli::vault::core

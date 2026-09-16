// coding: utf-8
// =============================================================================
// VaultPayload.h
//
// Length-prefixed binary encoding for vault item type-specific fields.
// Header-only so storage tests and ServiceCore share one implementation.
//
// Layout (little-endian):
//   u8  payload_version   (kItemPayloadVersion)
//   u8  item_type         (VaultItemType)
//   str account
//   str username
//   str password
//   str website
//   str note
// =============================================================================
#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "Error.h"
#include "Result.h"
#include "Types.h"

namespace yuli::vault::core {

namespace vault_payload_detail {

inline void append_u8(ByteVec& out, uint8_t v) {
    out.push_back(static_cast<std::byte>(v));
}

inline void append_u32(ByteVec& out, uint32_t v) {
    const auto* p = reinterpret_cast<const std::byte*>(&v);
    out.insert(out.end(), p, p + sizeof(v));
}

inline void append_string(ByteVec& out, const std::string& s) {
    append_u32(out, static_cast<uint32_t>(s.size()));
    if (!s.empty()) {
        const auto* p = reinterpret_cast<const std::byte*>(s.data());
        out.insert(out.end(), p, p + s.size());
    }
}

struct Cursor {
    ByteSpan data;
    size_t pos = 0;

    bool read_bytes(void* out, size_t n) {
        if (pos + n > data.size()) return false;
        std::memcpy(out, data.data() + pos, n);
        pos += n;
        return true;
    }

    bool read_u8(uint8_t& out) { return read_bytes(&out, sizeof(out)); }

    bool read_u32(uint32_t& out) { return read_bytes(&out, sizeof(out)); }

    bool read_string(std::string& out) {
        uint32_t len = 0;
        if (!read_u32(len)) return false;
        if (pos + len > data.size()) return false;
        out.assign(reinterpret_cast<const char*>(data.data() + pos), len);
        pos += len;
        return true;
    }
};

inline std::string to_lower_ascii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

inline bool contains_substring(const std::string& haystack, const std::string& needle,
                               bool case_sensitive) {
    if (needle.empty()) return true;
    if (case_sensitive) {
        return haystack.find(needle) != std::string::npos;
    }
    return to_lower_ascii(haystack).find(to_lower_ascii(needle)) != std::string::npos;
}

inline std::string canonical_search_field(const std::string& field) {
    if (field == "entry_name") return "title";
    return field;
}

}  // namespace vault_payload_detail

/// Encode type-specific fields (not title/tags/timestamps) into a payload blob.
inline ByteVec encode_item_payload(const VaultItem& item) {
    ByteVec out;
    out.reserve(16 + item.account.size() + item.username.size() + item.password.size() +
                item.website.size() + item.note.size());
    vault_payload_detail::append_u8(out, kItemPayloadVersion);
    vault_payload_detail::append_u8(out, static_cast<uint8_t>(item.type));
    vault_payload_detail::append_string(out, item.account);
    vault_payload_detail::append_string(out, item.username);
    vault_payload_detail::append_string(out, item.password);
    vault_payload_detail::append_string(out, item.website);
    vault_payload_detail::append_string(out, item.note);
    return out;
}

/// Decode a plaintext payload into login-shaped fields on `item`.
/// Title, id, tags, and timestamps are left unchanged.
inline Error decode_item_payload(VaultItem& item, ByteSpan data) {
    vault_payload_detail::Cursor c{data, 0};
    uint8_t version = 0;
    uint8_t type_raw = 0;
    if (!c.read_u8(version) || version != kItemPayloadVersion) {
        return Error(ErrorCode::InvalidArgument, "item payload version mismatch");
    }
    if (!c.read_u8(type_raw)) {
        return Error(ErrorCode::InvalidArgument, "item payload truncated");
    }
    item.type = static_cast<VaultItemType>(type_raw);
    if (!c.read_string(item.account) || !c.read_string(item.username) ||
        !c.read_string(item.password) || !c.read_string(item.website) ||
        !c.read_string(item.note)) {
        return Error(ErrorCode::InvalidArgument, "item payload truncated");
    }
    return Error{};
}

/// Apply decode if `data` looks like a v1 payload; otherwise leave fields as-is.
inline bool try_decode_item_payload(VaultItem& item, ByteSpan data) {
    if (data.empty()) return false;
    VaultItem tmp = item;
    if (!decode_item_payload(tmp, data).ok()) return false;
    item.type = tmp.type;
    item.account = std::move(tmp.account);
    item.username = std::move(tmp.username);
    item.password = std::move(tmp.password);
    item.website = std::move(tmp.website);
    item.note = std::move(tmp.note);
    return true;
}

inline bool is_searchable_item_field(const std::string& field) {
    const std::string f = vault_payload_detail::canonical_search_field(field);
    return f == "title" || f == "account" || f == "username" || f == "website" ||
           f == "note";
}

inline std::string item_search_field_value(const VaultItem& item, const std::string& field) {
    const std::string f = vault_payload_detail::canonical_search_field(field);
    if (f == "title") return item.title;
    if (f == "account") return item.account;
    if (f == "username") return item.username;
    if (f == "website") return item.website;
    if (f == "note") return item.note;
    return {};
}

/// Text match after decrypt. Tag filtering is done by storage.
inline bool item_matches_text_query(const VaultItem& item, const SearchQuery& query) {
    if (query.text.empty()) return true;
    std::vector<std::string> fields = query.fields;
    if (fields.empty()) {
        fields = {"title", "account", "username", "website", "note"};
    }
    for (const auto& raw : fields) {
        if (!is_searchable_item_field(raw)) continue;
        if (vault_payload_detail::contains_substring(
                item_search_field_value(item, raw), query.text, query.case_sensitive)) {
            return true;
        }
    }
    return false;
}

}  // namespace yuli::vault::core

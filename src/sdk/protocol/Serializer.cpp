// coding: utf-8
// =============================================================================
// Serializer.cpp
//
// PwdVault IPC 协议二进制序列化/反序列化实现。
//
// 所有字段按小端序写入（x86/Windows on ARM 均为小端，直接 memcpy）。
// 变长字段以 uint32_t 长度前缀打头；定长字段直接 memcpy。
// 复合结构按字段声明顺序逐字段写入，不写额外 tag。
// =============================================================================
#include "Serializer.h"

#include <cstring>
#include <utility>

namespace yuli::vault::protocol {

// ---------------------------------------------------------------------------
// 内部辅助：Writer / Reader 与变长字段的读写函数
// ---------------------------------------------------------------------------
namespace {

/// 字节写入器：将定长/变长字段追加到内部 ByteVec。
class Writer {
public:
    void write_bytes(const void* data, size_t size) {
        const auto* p = static_cast<const std::byte*>(data);
        buffer_.insert(buffer_.end(), p, p + size);
    }
    void write_u8(uint8_t v) { write_bytes(&v, sizeof(v)); }
    void write_u16(uint16_t v) { write_bytes(&v, sizeof(v)); }
    void write_u32(uint32_t v) { write_bytes(&v, sizeof(v)); }
    void write_u64(uint64_t v) { write_bytes(&v, sizeof(v)); }
    void write_i64(int64_t v)  { write_bytes(&v, sizeof(v)); }
    void write_bool(bool v) {
        const uint8_t b = v ? 1u : 0u;
        write_bytes(&b, sizeof(b));
    }

    void write_string(const std::string& s) {
        write_u32(static_cast<uint32_t>(s.size()));
        write_bytes(s.data(), s.size());
    }

    void write_byte_vec(const core::ByteVec& v) {
        write_u32(static_cast<uint32_t>(v.size()));
        if (!v.empty()) {
            write_bytes(v.data(), v.size());
        }
    }

    core::ByteVec take() { return std::move(buffer_); }

private:
    core::ByteVec buffer_;
};

/// 字节读取器：从 ByteSpan 按序读取字段，越界返回 false。
class Reader {
public:
    explicit Reader(core::ByteSpan data) : data_(data), pos_(0) {}

    bool read_bytes(void* out, size_t size) {
        if (pos_ + size > data_.size()) return false;
        std::memcpy(out, data_.data() + pos_, size);
        pos_ += size;
        return true;
    }
    bool read_u8(uint8_t& out) { return read_bytes(&out, sizeof(out)); }
    bool read_u16(uint16_t& out) { return read_bytes(&out, sizeof(out)); }
    bool read_u32(uint32_t& out) { return read_bytes(&out, sizeof(out)); }
    bool read_u64(uint64_t& out) { return read_bytes(&out, sizeof(out)); }
    bool read_i64(int64_t& out)  { return read_bytes(&out, sizeof(out)); }
    bool read_bool(bool& out) {
        uint8_t b = 0;
        if (!read_bytes(&b, sizeof(b))) return false;
        out = (b != 0u);
        return true;
    }

    /// 读取字符串：先读 uint32_t 长度，再读对应字节数。
    /// 长度超过剩余字节时返回 false，避免恶意长度导致 bad_alloc。
    bool read_string(std::string& out) {
        uint32_t len = 0;
        if (!read_u32(len)) return false;
        if (len > remaining()) return false;
        out.resize(len);
        if (len > 0 && !read_bytes(out.data(), len)) return false;
        return true;
    }

    /// 读取 ByteVec：与 read_string 同策略。
    bool read_byte_vec(core::ByteVec& out) {
        uint32_t len = 0;
        if (!read_u32(len)) return false;
        if (len > remaining()) return false;
        out.resize(len);
        if (len > 0 && !read_bytes(out.data(), len)) return false;
        return true;
    }

    size_t remaining() const { return data_.size() - pos_; }

private:
    core::ByteSpan data_;
    size_t pos_;
};

core::Error make_error(const char* what) {
    return core::Error(core::ErrorCode::InvalidArgument, what);
}

/// 写入 Tag：id(i64) + name(string) + color(string) + created_at(i64) + updated_at(i64)。
void write_tag(Writer& w, const core::Tag& t) {
    w.write_i64(t.id);
    w.write_string(t.name);
    w.write_string(t.color);
    w.write_i64(t.created_at);
    w.write_i64(t.updated_at);
}

bool read_tag(Reader& r, core::Tag& out) {
    if (!r.read_i64(out.id)) return false;
    if (!r.read_string(out.name)) return false;
    if (!r.read_string(out.color)) return false;
    if (!r.read_i64(out.created_at)) return false;
    if (!r.read_i64(out.updated_at)) return false;
    return true;
}

void write_tag_vector(Writer& w, const std::vector<core::Tag>& v) {
    w.write_u32(static_cast<uint32_t>(v.size()));
    for (const auto& t : v) {
        write_tag(w, t);
    }
}

bool read_tag_vector(Reader& r, std::vector<core::Tag>& out) {
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;  // 粗略上界保护
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        core::Tag t;
        if (!read_tag(r, t)) return false;
        out.push_back(std::move(t));
    }
    return true;
}

/// VaultItem / PasswordEntry (protocol v2):
///   id(i64) + type(u8) + title(string) + account + username + password +
///   website + note + tags + created_at + updated_at + iv + tag.
void write_password_entry(Writer& w, const core::PasswordEntry& e) {
    w.write_i64(e.id);
    w.write_u8(static_cast<uint8_t>(e.type));
    w.write_string(e.title);
    w.write_string(e.account);
    w.write_string(e.username);
    w.write_string(e.password);
    w.write_string(e.website);
    w.write_string(e.note);
    write_tag_vector(w, e.tags);
    w.write_i64(e.created_at);
    w.write_i64(e.updated_at);
    w.write_byte_vec(e.iv);
    w.write_byte_vec(e.tag);
}

bool read_password_entry(Reader& r, core::PasswordEntry& out) {
    if (!r.read_i64(out.id)) return false;
    uint8_t type_raw = 0;
    if (!r.read_u8(type_raw)) return false;
    out.type = static_cast<core::VaultItemType>(type_raw);
    if (!r.read_string(out.title)) return false;
    if (!r.read_string(out.account)) return false;
    if (!r.read_string(out.username)) return false;
    if (!r.read_string(out.password)) return false;
    if (!r.read_string(out.website)) return false;
    if (!r.read_string(out.note)) return false;
    if (!read_tag_vector(r, out.tags)) return false;
    if (!r.read_i64(out.created_at)) return false;
    if (!r.read_i64(out.updated_at)) return false;
    if (!r.read_byte_vec(out.iv)) return false;
    if (!r.read_byte_vec(out.tag)) return false;
    return true;
}

void write_search_query(Writer& w, const core::SearchQuery& q) {
    w.write_string(q.text);
    w.write_u32(static_cast<uint32_t>(q.fields.size()));
    for (const auto& f : q.fields) {
        w.write_string(f);
    }
    w.write_bool(q.case_sensitive);
    // tag_ids（v2 新增）
    w.write_u32(static_cast<uint32_t>(q.tag_ids.size()));
    for (auto tid : q.tag_ids) {
        w.write_i64(tid);
    }
}

bool read_search_query(Reader& r, core::SearchQuery& out) {
    if (!r.read_string(out.text)) return false;
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;  // 粗略上界保护
    out.fields.clear();
    out.fields.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string s;
        if (!r.read_string(s)) return false;
        out.fields.push_back(std::move(s));
    }
    if (!r.read_bool(out.case_sensitive)) return false;
    // tag_ids（v2 新增）
    uint32_t tag_count = 0;
    if (!r.read_u32(tag_count)) return false;
    if (tag_count > r.remaining()) return false;
    out.tag_ids.clear();
    out.tag_ids.reserve(tag_count);
    for (uint32_t i = 0; i < tag_count; ++i) {
        int64_t tid = 0;
        if (!r.read_i64(tid)) return false;
        out.tag_ids.push_back(tid);
    }
    return true;
}

void write_generator_options(Writer& w, const core::PasswordGeneratorOptions& o) {
    w.write_u64(static_cast<uint64_t>(o.length));
    w.write_bool(o.use_uppercase);
    w.write_bool(o.use_lowercase);
    w.write_bool(o.use_digits);
    w.write_bool(o.use_symbols);
    w.write_string(o.custom_chars);
    w.write_bool(o.exclude_ambiguous);
}

bool read_generator_options(Reader& r, core::PasswordGeneratorOptions& out) {
    uint64_t len = 0;
    if (!r.read_u64(len)) return false;
    out.length = static_cast<size_t>(len);
    if (!r.read_bool(out.use_uppercase)) return false;
    if (!r.read_bool(out.use_lowercase)) return false;
    if (!r.read_bool(out.use_digits)) return false;
    if (!r.read_bool(out.use_symbols)) return false;
    if (!r.read_string(out.custom_chars)) return false;
    if (!r.read_bool(out.exclude_ambiguous)) return false;
    return true;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// command_name（在 Commands.h 中声明）
// ---------------------------------------------------------------------------
std::string_view command_name(CommandId cmd) noexcept {
    switch (cmd) {
        case CommandId::Ping:             return "Ping";
        case CommandId::Shutdown:         return "Shutdown";
        case CommandId::Unlock:           return "Unlock";
        case CommandId::Lock:             return "Lock";
        case CommandId::EnableProgramPassword:  return "EnableProgramPassword";
        case CommandId::DisableProgramPassword: return "DisableProgramPassword";
        case CommandId::ChangeProgramPassword:  return "ChangeProgramPassword";
        case CommandId::GetVaultStatus:         return "GetVaultStatus";
        case CommandId::AddEntry:         return "AddEntry";
        case CommandId::UpdateEntry:      return "UpdateEntry";
        case CommandId::RemoveEntry:      return "RemoveEntry";
        case CommandId::GetEntry:         return "GetEntry";
        case CommandId::SearchEntries:    return "SearchEntries";
        case CommandId::ListEntries:      return "ListEntries";
        case CommandId::GeneratePassword: return "GeneratePassword";
        case CommandId::EstimateStrength: return "EstimateStrength";
        case CommandId::ListGeneratedRecords:  return "ListGeneratedRecords";
        case CommandId::RemoveGeneratedRecord: return "RemoveGeneratedRecord";
        case CommandId::ClearGeneratedRecords: return "ClearGeneratedRecords";
        case CommandId::GetGeneratorSettings:  return "GetGeneratorSettings";
        case CommandId::SetGeneratorLimit:     return "SetGeneratorLimit";
        case CommandId::AddTag:                return "AddTag";
        case CommandId::UpdateTag:             return "UpdateTag";
        case CommandId::RemoveTag:             return "RemoveTag";
        case CommandId::ListTags:              return "ListTags";
        case CommandId::GetTag:                return "GetTag";
        case CommandId::FindTagByName:         return "FindTagByName";
        case CommandId::GetEntryTags:           return "GetEntryTags";
        case CommandId::SetEntryTags:           return "SetEntryTags";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// 基础类型特化
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<uint16_t>(const uint16_t& v) {
    Writer w; w.write_u16(v); return w.take();
}
template <> core::Result<uint16_t> deserialize<uint16_t>(core::ByteSpan data) {
    Reader r(data); uint16_t v = 0;
    if (!r.read_u16(v)) return core::Result<uint16_t>::Err(make_error("deserialize<uint16_t>: data too short"));
    return core::Result<uint16_t>::Ok(v);
}

template <> core::ByteVec serialize<uint32_t>(const uint32_t& v) {
    Writer w; w.write_u32(v); return w.take();
}
template <> core::Result<uint32_t> deserialize<uint32_t>(core::ByteSpan data) {
    Reader r(data); uint32_t v = 0;
    if (!r.read_u32(v)) return core::Result<uint32_t>::Err(make_error("deserialize<uint32_t>: data too short"));
    return core::Result<uint32_t>::Ok(v);
}

template <> core::ByteVec serialize<uint64_t>(const uint64_t& v) {
    Writer w; w.write_u64(v); return w.take();
}
template <> core::Result<uint64_t> deserialize<uint64_t>(core::ByteSpan data) {
    Reader r(data); uint64_t v = 0;
    if (!r.read_u64(v)) return core::Result<uint64_t>::Err(make_error("deserialize<uint64_t>: data too short"));
    return core::Result<uint64_t>::Ok(v);
}

template <> core::ByteVec serialize<int64_t>(const int64_t& v) {
    Writer w; w.write_i64(v); return w.take();
}
template <> core::Result<int64_t> deserialize<int64_t>(core::ByteSpan data) {
    Reader r(data); int64_t v = 0;
    if (!r.read_i64(v)) return core::Result<int64_t>::Err(make_error("deserialize<int64_t>: data too short"));
    return core::Result<int64_t>::Ok(v);
}

template <> core::ByteVec serialize<bool>(const bool& v) {
    Writer w; w.write_bool(v); return w.take();
}
template <> core::Result<bool> deserialize<bool>(core::ByteSpan data) {
    Reader r(data); bool v = false;
    if (!r.read_bool(v)) return core::Result<bool>::Err(make_error("deserialize<bool>: data too short"));
    return core::Result<bool>::Ok(v);
}

template <> core::ByteVec serialize<std::string>(const std::string& v) {
    Writer w; w.write_string(v); return w.take();
}
template <> core::Result<std::string> deserialize<std::string>(core::ByteSpan data) {
    Reader r(data); std::string v;
    if (!r.read_string(v)) return core::Result<std::string>::Err(make_error("deserialize<std::string>: malformed"));
    return core::Result<std::string>::Ok(std::move(v));
}

template <> core::ByteVec serialize<core::ByteVec>(const core::ByteVec& v) {
    Writer w; w.write_byte_vec(v); return w.take();
}
template <> core::Result<core::ByteVec> deserialize<core::ByteVec>(core::ByteSpan data) {
    Reader r(data); core::ByteVec v;
    if (!r.read_byte_vec(v)) return core::Result<core::ByteVec>::Err(make_error("deserialize<ByteVec>: malformed"));
    return core::Result<core::ByteVec>::Ok(std::move(v));
}

// ---------------------------------------------------------------------------
// core 类型特化
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<core::ErrorCode>(const core::ErrorCode& v) {
    Writer w; w.write_u32(static_cast<uint32_t>(v)); return w.take();
}
template <> core::Result<core::ErrorCode> deserialize<core::ErrorCode>(core::ByteSpan data) {
    Reader r(data); uint32_t v = 0;
    if (!r.read_u32(v)) return core::Result<core::ErrorCode>::Err(make_error("deserialize<ErrorCode>: data too short"));
    return core::Result<core::ErrorCode>::Ok(static_cast<core::ErrorCode>(v));
}

template <> core::ByteVec serialize<core::PasswordEntry>(const core::PasswordEntry& v) {
    Writer w; write_password_entry(w, v); return w.take();
}
template <> core::Result<core::PasswordEntry> deserialize<core::PasswordEntry>(core::ByteSpan data) {
    Reader r(data); core::PasswordEntry v;
    if (!read_password_entry(r, v)) return core::Result<core::PasswordEntry>::Err(make_error("deserialize<PasswordEntry>: malformed"));
    return core::Result<core::PasswordEntry>::Ok(std::move(v));
}

template <> core::ByteVec serialize<core::Tag>(const core::Tag& v) {
    Writer w; write_tag(w, v); return w.take();
}
template <> core::Result<core::Tag> deserialize<core::Tag>(core::ByteSpan data) {
    Reader r(data); core::Tag v;
    if (!read_tag(r, v)) return core::Result<core::Tag>::Err(make_error("deserialize<Tag>: malformed"));
    return core::Result<core::Tag>::Ok(std::move(v));
}

template <> core::ByteVec serialize<core::SearchQuery>(const core::SearchQuery& v) {
    Writer w; write_search_query(w, v); return w.take();
}
template <> core::Result<core::SearchQuery> deserialize<core::SearchQuery>(core::ByteSpan data) {
    Reader r(data); core::SearchQuery v;
    if (!read_search_query(r, v)) return core::Result<core::SearchQuery>::Err(make_error("deserialize<SearchQuery>: malformed"));
    return core::Result<core::SearchQuery>::Ok(std::move(v));
}

template <> core::ByteVec serialize<core::PasswordGeneratorOptions>(const core::PasswordGeneratorOptions& v) {
    Writer w; write_generator_options(w, v); return w.take();
}
template <> core::Result<core::PasswordGeneratorOptions> deserialize<core::PasswordGeneratorOptions>(core::ByteSpan data) {
    Reader r(data); core::PasswordGeneratorOptions v;
    if (!read_generator_options(r, v)) return core::Result<core::PasswordGeneratorOptions>::Err(make_error("deserialize<PasswordGeneratorOptions>: malformed"));
    return core::Result<core::PasswordGeneratorOptions>::Ok(std::move(v));
}

// ---------------------------------------------------------------------------
// 请求消息特化
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<PingRequest>(const PingRequest&) {
    return {};
}
template <> core::Result<PingRequest> deserialize<PingRequest>(core::ByteSpan) {
    return core::Result<PingRequest>::Ok(PingRequest{});
}

template <> core::ByteVec serialize<ShutdownRequest>(const ShutdownRequest&) {
    return {};
}
template <> core::Result<ShutdownRequest> deserialize<ShutdownRequest>(core::ByteSpan) {
    return core::Result<ShutdownRequest>::Ok(ShutdownRequest{});
}

template <> core::ByteVec serialize<UnlockRequest>(const UnlockRequest& v) {
    Writer w;
    w.write_string(v.password);
    return w.take();
}
template <> core::Result<UnlockRequest> deserialize<UnlockRequest>(core::ByteSpan data) {
    Reader r(data); UnlockRequest v;
    if (!r.read_string(v.password)) return core::Result<UnlockRequest>::Err(make_error("deserialize<UnlockRequest>: malformed"));
    return core::Result<UnlockRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<LockRequest>(const LockRequest&) {
    return {};
}
template <> core::Result<LockRequest> deserialize<LockRequest>(core::ByteSpan) {
    return core::Result<LockRequest>::Ok(LockRequest{});
}

template <> core::ByteVec serialize<EnableProgramPasswordRequest>(const EnableProgramPasswordRequest& v) {
    Writer w;
    w.write_string(v.password);
    return w.take();
}
template <> core::Result<EnableProgramPasswordRequest> deserialize<EnableProgramPasswordRequest>(core::ByteSpan data) {
    Reader r(data); EnableProgramPasswordRequest v;
    if (!r.read_string(v.password)) return core::Result<EnableProgramPasswordRequest>::Err(make_error("deserialize<EnableProgramPasswordRequest>: malformed"));
    return core::Result<EnableProgramPasswordRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<DisableProgramPasswordRequest>(const DisableProgramPasswordRequest& v) {
    Writer w;
    w.write_string(v.password);
    return w.take();
}
template <> core::Result<DisableProgramPasswordRequest> deserialize<DisableProgramPasswordRequest>(core::ByteSpan data) {
    Reader r(data); DisableProgramPasswordRequest v;
    if (!r.read_string(v.password)) return core::Result<DisableProgramPasswordRequest>::Err(make_error("deserialize<DisableProgramPasswordRequest>: malformed"));
    return core::Result<DisableProgramPasswordRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<ChangeProgramPasswordRequest>(const ChangeProgramPasswordRequest& v) {
    Writer w;
    w.write_string(v.old_password);
    w.write_string(v.new_password);
    return w.take();
}
template <> core::Result<ChangeProgramPasswordRequest> deserialize<ChangeProgramPasswordRequest>(core::ByteSpan data) {
    Reader r(data); ChangeProgramPasswordRequest v;
    if (!r.read_string(v.old_password)) return core::Result<ChangeProgramPasswordRequest>::Err(make_error("deserialize<ChangeProgramPasswordRequest>: malformed"));
    if (!r.read_string(v.new_password)) return core::Result<ChangeProgramPasswordRequest>::Err(make_error("deserialize<ChangeProgramPasswordRequest>: malformed"));
    return core::Result<ChangeProgramPasswordRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<GetVaultStatusRequest>(const GetVaultStatusRequest&) {
    return {};
}
template <> core::Result<GetVaultStatusRequest> deserialize<GetVaultStatusRequest>(core::ByteSpan) {
    return core::Result<GetVaultStatusRequest>::Ok(GetVaultStatusRequest{});
}

template <> core::ByteVec serialize<AddEntryRequest>(const AddEntryRequest& v) {
    Writer w; write_password_entry(w, v.entry); return w.take();
}
template <> core::Result<AddEntryRequest> deserialize<AddEntryRequest>(core::ByteSpan data) {
    Reader r(data); AddEntryRequest v;
    if (!read_password_entry(r, v.entry)) return core::Result<AddEntryRequest>::Err(make_error("deserialize<AddEntryRequest>: malformed"));
    return core::Result<AddEntryRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<UpdateEntryRequest>(const UpdateEntryRequest& v) {
    Writer w; write_password_entry(w, v.entry); return w.take();
}
template <> core::Result<UpdateEntryRequest> deserialize<UpdateEntryRequest>(core::ByteSpan data) {
    Reader r(data); UpdateEntryRequest v;
    if (!read_password_entry(r, v.entry)) return core::Result<UpdateEntryRequest>::Err(make_error("deserialize<UpdateEntryRequest>: malformed"));
    return core::Result<UpdateEntryRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<RemoveEntryRequest>(const RemoveEntryRequest& v) {
    Writer w; w.write_i64(v.id); return w.take();
}
template <> core::Result<RemoveEntryRequest> deserialize<RemoveEntryRequest>(core::ByteSpan data) {
    Reader r(data); RemoveEntryRequest v;
    if (!r.read_i64(v.id)) return core::Result<RemoveEntryRequest>::Err(make_error("deserialize<RemoveEntryRequest>: data too short"));
    return core::Result<RemoveEntryRequest>::Ok(v);
}

template <> core::ByteVec serialize<GetEntryRequest>(const GetEntryRequest& v) {
    Writer w; w.write_i64(v.id); return w.take();
}
template <> core::Result<GetEntryRequest> deserialize<GetEntryRequest>(core::ByteSpan data) {
    Reader r(data); GetEntryRequest v;
    if (!r.read_i64(v.id)) return core::Result<GetEntryRequest>::Err(make_error("deserialize<GetEntryRequest>: data too short"));
    return core::Result<GetEntryRequest>::Ok(v);
}

template <> core::ByteVec serialize<SearchEntriesRequest>(const SearchEntriesRequest& v) {
    Writer w; write_search_query(w, v.query); return w.take();
}
template <> core::Result<SearchEntriesRequest> deserialize<SearchEntriesRequest>(core::ByteSpan data) {
    Reader r(data); SearchEntriesRequest v;
    if (!read_search_query(r, v.query)) return core::Result<SearchEntriesRequest>::Err(make_error("deserialize<SearchEntriesRequest>: malformed"));
    return core::Result<SearchEntriesRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<ListEntriesRequest>(const ListEntriesRequest&) {
    return {};
}
template <> core::Result<ListEntriesRequest> deserialize<ListEntriesRequest>(core::ByteSpan) {
    return core::Result<ListEntriesRequest>::Ok(ListEntriesRequest{});
}

template <> core::ByteVec serialize<GeneratePasswordRequest>(const GeneratePasswordRequest& v) {
    Writer w; write_generator_options(w, v.options); return w.take();
}
template <> core::Result<GeneratePasswordRequest> deserialize<GeneratePasswordRequest>(core::ByteSpan data) {
    Reader r(data); GeneratePasswordRequest v;
    if (!read_generator_options(r, v.options)) return core::Result<GeneratePasswordRequest>::Err(make_error("deserialize<GeneratePasswordRequest>: malformed"));
    return core::Result<GeneratePasswordRequest>::Ok(std::move(v));
}

template <> core::ByteVec serialize<EstimateStrengthRequest>(const EstimateStrengthRequest& v) {
    Writer w; w.write_string(v.password); return w.take();
}
template <> core::Result<EstimateStrengthRequest> deserialize<EstimateStrengthRequest>(core::ByteSpan data) {
    Reader r(data); EstimateStrengthRequest v;
    if (!r.read_string(v.password)) return core::Result<EstimateStrengthRequest>::Err(make_error("deserialize<EstimateStrengthRequest>: malformed"));
    return core::Result<EstimateStrengthRequest>::Ok(std::move(v));
}

// ---------------------------------------------------------------------------
// 响应消息特化
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<PingResponse>(const PingResponse& v) {
    Writer w; w.write_u64(v.server_timestamp); return w.take();
}
template <> core::Result<PingResponse> deserialize<PingResponse>(core::ByteSpan data) {
    Reader r(data); PingResponse v;
    if (!r.read_u64(v.server_timestamp)) return core::Result<PingResponse>::Err(make_error("deserialize<PingResponse>: data too short"));
    return core::Result<PingResponse>::Ok(v);
}

template <> core::ByteVec serialize<ShutdownResponse>(const ShutdownResponse&) {
    return {};
}
template <> core::Result<ShutdownResponse> deserialize<ShutdownResponse>(core::ByteSpan data) {
    // 空响应：data 必须为空。若非空则可能是 ErrorResponse，让调用方回退解析。
    if (!data.empty()) return core::Result<ShutdownResponse>::Err(make_error("deserialize<ShutdownResponse>: expected empty payload"));
    return core::Result<ShutdownResponse>::Ok(ShutdownResponse{});
}

template <> core::ByteVec serialize<UnlockResponse>(const UnlockResponse& v) {
    Writer w;
    w.write_bool(v.success);
    w.write_string(v.error_message);
    return w.take();
}
template <> core::Result<UnlockResponse> deserialize<UnlockResponse>(core::ByteSpan data) {
    Reader r(data); UnlockResponse v;
    if (!r.read_bool(v.success)) return core::Result<UnlockResponse>::Err(make_error("deserialize<UnlockResponse>: malformed"));
    if (!r.read_string(v.error_message)) return core::Result<UnlockResponse>::Err(make_error("deserialize<UnlockResponse>: malformed"));
    return core::Result<UnlockResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<LockResponse>(const LockResponse&) {
    return {};
}
template <> core::Result<LockResponse> deserialize<LockResponse>(core::ByteSpan data) {
    // 空响应：data 必须为空。若非空则可能是 ErrorResponse，让调用方回退解析。
    if (!data.empty()) return core::Result<LockResponse>::Err(make_error("deserialize<LockResponse>: expected empty payload"));
    return core::Result<LockResponse>::Ok(LockResponse{});
}

template <> core::ByteVec serialize<EnableProgramPasswordResponse>(const EnableProgramPasswordResponse& v) {
    Writer w;
    w.write_bool(v.success);
    w.write_string(v.error_message);
    return w.take();
}
template <> core::Result<EnableProgramPasswordResponse> deserialize<EnableProgramPasswordResponse>(core::ByteSpan data) {
    Reader r(data); EnableProgramPasswordResponse v;
    if (!r.read_bool(v.success)) return core::Result<EnableProgramPasswordResponse>::Err(make_error("deserialize<EnableProgramPasswordResponse>: malformed"));
    if (!r.read_string(v.error_message)) return core::Result<EnableProgramPasswordResponse>::Err(make_error("deserialize<EnableProgramPasswordResponse>: malformed"));
    return core::Result<EnableProgramPasswordResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<DisableProgramPasswordResponse>(const DisableProgramPasswordResponse& v) {
    Writer w;
    w.write_bool(v.success);
    w.write_string(v.error_message);
    return w.take();
}
template <> core::Result<DisableProgramPasswordResponse> deserialize<DisableProgramPasswordResponse>(core::ByteSpan data) {
    Reader r(data); DisableProgramPasswordResponse v;
    if (!r.read_bool(v.success)) return core::Result<DisableProgramPasswordResponse>::Err(make_error("deserialize<DisableProgramPasswordResponse>: malformed"));
    if (!r.read_string(v.error_message)) return core::Result<DisableProgramPasswordResponse>::Err(make_error("deserialize<DisableProgramPasswordResponse>: malformed"));
    return core::Result<DisableProgramPasswordResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<ChangeProgramPasswordResponse>(const ChangeProgramPasswordResponse& v) {
    Writer w;
    w.write_bool(v.success);
    w.write_string(v.error_message);
    return w.take();
}
template <> core::Result<ChangeProgramPasswordResponse> deserialize<ChangeProgramPasswordResponse>(core::ByteSpan data) {
    Reader r(data); ChangeProgramPasswordResponse v;
    if (!r.read_bool(v.success)) return core::Result<ChangeProgramPasswordResponse>::Err(make_error("deserialize<ChangeProgramPasswordResponse>: malformed"));
    if (!r.read_string(v.error_message)) return core::Result<ChangeProgramPasswordResponse>::Err(make_error("deserialize<ChangeProgramPasswordResponse>: malformed"));
    return core::Result<ChangeProgramPasswordResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<GetVaultStatusResponse>(const GetVaultStatusResponse& v) {
    Writer w;
    w.write_bool(v.password_enabled);
    w.write_bool(v.is_locked);
    return w.take();
}
template <> core::Result<GetVaultStatusResponse> deserialize<GetVaultStatusResponse>(core::ByteSpan data) {
    Reader r(data); GetVaultStatusResponse v;
    if (!r.read_bool(v.password_enabled)) return core::Result<GetVaultStatusResponse>::Err(make_error("deserialize<GetVaultStatusResponse>: malformed"));
    if (!r.read_bool(v.is_locked)) return core::Result<GetVaultStatusResponse>::Err(make_error("deserialize<GetVaultStatusResponse>: malformed"));
    return core::Result<GetVaultStatusResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<AddEntryResponse>(const AddEntryResponse& v) {
    Writer w; write_password_entry(w, v.entry); return w.take();
}
template <> core::Result<AddEntryResponse> deserialize<AddEntryResponse>(core::ByteSpan data) {
    Reader r(data); AddEntryResponse v;
    if (!read_password_entry(r, v.entry)) return core::Result<AddEntryResponse>::Err(make_error("deserialize<AddEntryResponse>: malformed"));
    return core::Result<AddEntryResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<UpdateEntryResponse>(const UpdateEntryResponse& v) {
    Writer w; write_password_entry(w, v.entry); return w.take();
}
template <> core::Result<UpdateEntryResponse> deserialize<UpdateEntryResponse>(core::ByteSpan data) {
    Reader r(data); UpdateEntryResponse v;
    if (!read_password_entry(r, v.entry)) return core::Result<UpdateEntryResponse>::Err(make_error("deserialize<UpdateEntryResponse>: malformed"));
    return core::Result<UpdateEntryResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<RemoveEntryResponse>(const RemoveEntryResponse&) {
    return {};
}
template <> core::Result<RemoveEntryResponse> deserialize<RemoveEntryResponse>(core::ByteSpan data) {
    // 空响应：data 必须为空。若非空则可能是 ErrorResponse，让调用方回退解析。
    if (!data.empty()) return core::Result<RemoveEntryResponse>::Err(make_error("deserialize<RemoveEntryResponse>: expected empty payload"));
    return core::Result<RemoveEntryResponse>::Ok(RemoveEntryResponse{});
}

template <> core::ByteVec serialize<GetEntryResponse>(const GetEntryResponse& v) {
    Writer w; write_password_entry(w, v.entry); return w.take();
}
template <> core::Result<GetEntryResponse> deserialize<GetEntryResponse>(core::ByteSpan data) {
    Reader r(data); GetEntryResponse v;
    if (!read_password_entry(r, v.entry)) return core::Result<GetEntryResponse>::Err(make_error("deserialize<GetEntryResponse>: malformed"));
    return core::Result<GetEntryResponse>::Ok(std::move(v));
}

namespace {

/// 写入 vector<T>：长度前缀 + 逐元素序列化。
void write_password_entry_vector(Writer& w, const std::vector<core::PasswordEntry>& v) {
    w.write_u32(static_cast<uint32_t>(v.size()));
    for (const auto& e : v) {
        write_password_entry(w, e);
    }
}

bool read_password_entry_vector(Reader& r, std::vector<core::PasswordEntry>& out) {
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;  // 粗略上界保护
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        core::PasswordEntry e;
        if (!read_password_entry(r, e)) return false;
        out.push_back(std::move(e));
    }
    return true;
}

/// 写入 GeneratedPasswordRecord：id(i64) + password(string) + length(i32) +
/// created_at(i64) + iv(byte_vec) + tag(byte_vec)。
void write_generated_record(Writer& w, const core::GeneratedPasswordRecord& r) {
    w.write_i64(r.id);
    w.write_string(r.password);
    // int32_t 在线路上以 i64 传输以保持序列化器一致风格
    w.write_i64(static_cast<int64_t>(r.length));
    w.write_i64(r.created_at);
    w.write_byte_vec(r.iv);
    w.write_byte_vec(r.tag);
}

bool read_generated_record(Reader& r, core::GeneratedPasswordRecord& out) {
    int64_t tmp = 0;
    if (!r.read_i64(out.id)) return false;
    if (!r.read_string(out.password)) return false;
    if (!r.read_i64(tmp)) return false;
    out.length = static_cast<int32_t>(tmp);
    if (!r.read_i64(out.created_at)) return false;
    if (!r.read_byte_vec(out.iv)) return false;
    if (!r.read_byte_vec(out.tag)) return false;
    return true;
}

void write_generated_record_vector(Writer& w,
                                    const std::vector<core::GeneratedPasswordRecord>& v) {
    w.write_u32(static_cast<uint32_t>(v.size()));
    for (const auto& r : v) {
        write_generated_record(w, r);
    }
}

bool read_generated_record_vector(Reader& r,
                                   std::vector<core::GeneratedPasswordRecord>& out) {
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        core::GeneratedPasswordRecord rec;
        if (!read_generated_record(r, rec)) return false;
        out.push_back(std::move(rec));
    }
    return true;
}

/// 写入 StrengthEstimate：bits(i64) + level(u8) + score(i64) + warnings(vec<string>)。
/// StrengthLevel 底层为 uint8_t，直接序列化其底层值。
void write_strength_estimate(Writer& w, const core::StrengthEstimate& s) {
    w.write_i64(s.bits);
    const uint8_t level_byte = static_cast<uint8_t>(s.level);
    w.write_bytes(&level_byte, sizeof(level_byte));
    w.write_i64(s.score);
    w.write_u32(static_cast<uint32_t>(s.warnings.size()));
    for (const auto& warn : s.warnings) {
        w.write_string(warn);
    }
}

bool read_strength_estimate(Reader& r, core::StrengthEstimate& out) {
    int64_t bits_tmp = 0;
    if (!r.read_i64(bits_tmp)) return false;
    out.bits = static_cast<int>(bits_tmp);
    uint8_t level_byte = 0;
    if (!r.read_bytes(&level_byte, sizeof(level_byte))) return false;
    out.level = static_cast<core::StrengthLevel>(level_byte);
    int64_t score_tmp = 0;
    if (!r.read_i64(score_tmp)) return false;
    out.score = static_cast<int>(score_tmp);
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;  // 粗略上界保护
    out.warnings.clear();
    out.warnings.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string s;
        if (!r.read_string(s)) return false;
        out.warnings.push_back(std::move(s));
    }
    return true;
}

}  // anonymous namespace

template <> core::ByteVec serialize<core::StrengthEstimate>(const core::StrengthEstimate& v) {
    Writer w; write_strength_estimate(w, v); return w.take();
}
template <> core::Result<core::StrengthEstimate> deserialize<core::StrengthEstimate>(core::ByteSpan data) {
    Reader r(data); core::StrengthEstimate v;
    if (!read_strength_estimate(r, v)) {
        return core::Result<core::StrengthEstimate>::Err(
            make_error("deserialize<StrengthEstimate>: malformed"));
    }
    return core::Result<core::StrengthEstimate>::Ok(std::move(v));
}

template <> core::ByteVec serialize<SearchEntriesResponse>(const SearchEntriesResponse& v) {
    Writer w; write_password_entry_vector(w, v.entries); return w.take();
}
template <> core::Result<SearchEntriesResponse> deserialize<SearchEntriesResponse>(core::ByteSpan data) {
    Reader r(data); SearchEntriesResponse v;
    if (!read_password_entry_vector(r, v.entries)) return core::Result<SearchEntriesResponse>::Err(make_error("deserialize<SearchEntriesResponse>: malformed"));
    return core::Result<SearchEntriesResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<ListEntriesResponse>(const ListEntriesResponse& v) {
    Writer w; write_password_entry_vector(w, v.entries); return w.take();
}
template <> core::Result<ListEntriesResponse> deserialize<ListEntriesResponse>(core::ByteSpan data) {
    Reader r(data); ListEntriesResponse v;
    if (!read_password_entry_vector(r, v.entries)) return core::Result<ListEntriesResponse>::Err(make_error("deserialize<ListEntriesResponse>: malformed"));
    return core::Result<ListEntriesResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<GeneratePasswordResponse>(const GeneratePasswordResponse& v) {
    Writer w; w.write_string(v.password); return w.take();
}
template <> core::Result<GeneratePasswordResponse> deserialize<GeneratePasswordResponse>(core::ByteSpan data) {
    Reader r(data); GeneratePasswordResponse v;
    if (!r.read_string(v.password)) return core::Result<GeneratePasswordResponse>::Err(make_error("deserialize<GeneratePasswordResponse>: malformed"));
    return core::Result<GeneratePasswordResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<EstimateStrengthResponse>(const EstimateStrengthResponse& v) {
    Writer w;
    write_strength_estimate(w, v.estimate);
    return w.take();
}
template <> core::Result<EstimateStrengthResponse> deserialize<EstimateStrengthResponse>(core::ByteSpan data) {
    Reader r(data); EstimateStrengthResponse v;
    if (!read_strength_estimate(r, v.estimate)) {
        return core::Result<EstimateStrengthResponse>::Err(
            make_error("deserialize<EstimateStrengthResponse>: malformed"));
    }
    return core::Result<EstimateStrengthResponse>::Ok(std::move(v));
}

// ---------------------------------------------------------------------------
// 生成器历史记录相关
// ---------------------------------------------------------------------------

template <> core::ByteVec serialize<core::GeneratedPasswordRecord>(const core::GeneratedPasswordRecord& v) {
    Writer w; write_generated_record(w, v); return w.take();
}
template <> core::Result<core::GeneratedPasswordRecord> deserialize<core::GeneratedPasswordRecord>(core::ByteSpan data) {
    Reader r(data); core::GeneratedPasswordRecord v;
    if (!read_generated_record(r, v)) {
        return core::Result<core::GeneratedPasswordRecord>::Err(
            make_error("deserialize<GeneratedPasswordRecord>: malformed"));
    }
    return core::Result<core::GeneratedPasswordRecord>::Ok(std::move(v));
}

template <> core::ByteVec serialize<ListGeneratedRecordsRequest>(const ListGeneratedRecordsRequest&) {
    return {};  // 无负载
}
template <> core::Result<ListGeneratedRecordsRequest> deserialize<ListGeneratedRecordsRequest>(core::ByteSpan) {
    return core::Result<ListGeneratedRecordsRequest>::Ok(ListGeneratedRecordsRequest{});
}

template <> core::ByteVec serialize<RemoveGeneratedRecordRequest>(const RemoveGeneratedRecordRequest& v) {
    Writer w; w.write_i64(v.id); return w.take();
}
template <> core::Result<RemoveGeneratedRecordRequest> deserialize<RemoveGeneratedRecordRequest>(core::ByteSpan data) {
    Reader r(data); RemoveGeneratedRecordRequest v;
    if (!r.read_i64(v.id)) {
        return core::Result<RemoveGeneratedRecordRequest>::Err(
            make_error("deserialize<RemoveGeneratedRecordRequest>: malformed"));
    }
    return core::Result<RemoveGeneratedRecordRequest>::Ok(v);
}

template <> core::ByteVec serialize<ClearGeneratedRecordsRequest>(const ClearGeneratedRecordsRequest&) {
    return {};
}
template <> core::Result<ClearGeneratedRecordsRequest> deserialize<ClearGeneratedRecordsRequest>(core::ByteSpan) {
    return core::Result<ClearGeneratedRecordsRequest>::Ok(ClearGeneratedRecordsRequest{});
}

template <> core::ByteVec serialize<GetGeneratorSettingsRequest>(const GetGeneratorSettingsRequest&) {
    return {};
}
template <> core::Result<GetGeneratorSettingsRequest> deserialize<GetGeneratorSettingsRequest>(core::ByteSpan) {
    return core::Result<GetGeneratorSettingsRequest>::Ok(GetGeneratorSettingsRequest{});
}

template <> core::ByteVec serialize<SetGeneratorLimitRequest>(const SetGeneratorLimitRequest& v) {
    Writer w; w.write_i64(static_cast<int64_t>(v.limit)); return w.take();
}
template <> core::Result<SetGeneratorLimitRequest> deserialize<SetGeneratorLimitRequest>(core::ByteSpan data) {
    Reader r(data); SetGeneratorLimitRequest v;
    int64_t tmp = 0;
    if (!r.read_i64(tmp)) {
        return core::Result<SetGeneratorLimitRequest>::Err(
            make_error("deserialize<SetGeneratorLimitRequest>: malformed"));
    }
    v.limit = static_cast<int32_t>(tmp);
    return core::Result<SetGeneratorLimitRequest>::Ok(v);
}

template <> core::ByteVec serialize<ListGeneratedRecordsResponse>(const ListGeneratedRecordsResponse& v) {
    Writer w; write_generated_record_vector(w, v.records); return w.take();
}
template <> core::Result<ListGeneratedRecordsResponse> deserialize<ListGeneratedRecordsResponse>(core::ByteSpan data) {
    Reader r(data); ListGeneratedRecordsResponse v;
    if (!read_generated_record_vector(r, v.records)) {
        return core::Result<ListGeneratedRecordsResponse>::Err(
            make_error("deserialize<ListGeneratedRecordsResponse>: malformed"));
    }
    return core::Result<ListGeneratedRecordsResponse>::Ok(std::move(v));
}

template <> core::ByteVec serialize<RemoveGeneratedRecordResponse>(const RemoveGeneratedRecordResponse&) {
    return {};
}
template <> core::Result<RemoveGeneratedRecordResponse> deserialize<RemoveGeneratedRecordResponse>(core::ByteSpan) {
    return core::Result<RemoveGeneratedRecordResponse>::Ok(RemoveGeneratedRecordResponse{});
}

template <> core::ByteVec serialize<ClearGeneratedRecordsResponse>(const ClearGeneratedRecordsResponse&) {
    return {};
}
template <> core::Result<ClearGeneratedRecordsResponse> deserialize<ClearGeneratedRecordsResponse>(core::ByteSpan) {
    return core::Result<ClearGeneratedRecordsResponse>::Ok(ClearGeneratedRecordsResponse{});
}

template <> core::ByteVec serialize<GetGeneratorSettingsResponse>(const GetGeneratorSettingsResponse& v) {
    Writer w; w.write_i64(static_cast<int64_t>(v.history_limit)); return w.take();
}
template <> core::Result<GetGeneratorSettingsResponse> deserialize<GetGeneratorSettingsResponse>(core::ByteSpan data) {
    Reader r(data); GetGeneratorSettingsResponse v;
    int64_t tmp = 0;
    if (!r.read_i64(tmp)) {
        return core::Result<GetGeneratorSettingsResponse>::Err(
            make_error("deserialize<GetGeneratorSettingsResponse>: malformed"));
    }
    v.history_limit = static_cast<int32_t>(tmp);
    return core::Result<GetGeneratorSettingsResponse>::Ok(v);
}

template <> core::ByteVec serialize<SetGeneratorLimitResponse>(const SetGeneratorLimitResponse& v) {
    Writer w;
    const uint8_t b = v.success ? 1 : 0;
    w.write_bytes(&b, sizeof(b));
    return w.take();
}
template <> core::Result<SetGeneratorLimitResponse> deserialize<SetGeneratorLimitResponse>(core::ByteSpan data) {
    Reader r(data); SetGeneratorLimitResponse v;
    uint8_t b = 0;
    if (!r.read_bytes(&b, sizeof(b))) {
        return core::Result<SetGeneratorLimitResponse>::Err(
            make_error("deserialize<SetGeneratorLimitResponse>: malformed"));
    }
    v.success = (b != 0);
    return core::Result<SetGeneratorLimitResponse>::Ok(v);
}

// ---------------------------------------------------------------------------
// 标签（Tag）相关消息
// ---------------------------------------------------------------------------

namespace {

void write_tag_vector_inline(Writer& w, const std::vector<core::Tag>& v) {
    w.write_u32(static_cast<uint32_t>(v.size()));
    for (const auto& t : v) {
        write_tag(w, t);
    }
}

bool read_tag_vector_inline(Reader& r, std::vector<core::Tag>& out) {
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        core::Tag t;
        if (!read_tag(r, t)) return false;
        out.push_back(std::move(t));
    }
    return true;
}

void write_i64_vector(Writer& w, const std::vector<int64_t>& v) {
    w.write_u32(static_cast<uint32_t>(v.size()));
    for (auto x : v) {
        w.write_i64(x);
    }
}

bool read_i64_vector(Reader& r, std::vector<int64_t>& out) {
    uint32_t count = 0;
    if (!r.read_u32(count)) return false;
    if (count > r.remaining()) return false;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        int64_t x = 0;
        if (!r.read_i64(x)) return false;
        out.push_back(x);
    }
    return true;
}

}  // anonymous namespace

// AddTagRequest：tag(Tag)
template <> core::ByteVec serialize<AddTagRequest>(const AddTagRequest& v) {
    Writer w; write_tag(w, v.tag); return w.take();
}
template <> core::Result<AddTagRequest> deserialize<AddTagRequest>(core::ByteSpan data) {
    Reader r(data); AddTagRequest v;
    if (!read_tag(r, v.tag)) return core::Result<AddTagRequest>::Err(make_error("deserialize<AddTagRequest>: malformed"));
    return core::Result<AddTagRequest>::Ok(std::move(v));
}

// UpdateTagRequest：tag(Tag)
template <> core::ByteVec serialize<UpdateTagRequest>(const UpdateTagRequest& v) {
    Writer w; write_tag(w, v.tag); return w.take();
}
template <> core::Result<UpdateTagRequest> deserialize<UpdateTagRequest>(core::ByteSpan data) {
    Reader r(data); UpdateTagRequest v;
    if (!read_tag(r, v.tag)) return core::Result<UpdateTagRequest>::Err(make_error("deserialize<UpdateTagRequest>: malformed"));
    return core::Result<UpdateTagRequest>::Ok(std::move(v));
}

// RemoveTagRequest：id(i64)
template <> core::ByteVec serialize<RemoveTagRequest>(const RemoveTagRequest& v) {
    Writer w; w.write_i64(v.id); return w.take();
}
template <> core::Result<RemoveTagRequest> deserialize<RemoveTagRequest>(core::ByteSpan data) {
    Reader r(data); RemoveTagRequest v;
    if (!r.read_i64(v.id)) return core::Result<RemoveTagRequest>::Err(make_error("deserialize<RemoveTagRequest>: malformed"));
    return core::Result<RemoveTagRequest>::Ok(v);
}

// ListTagsRequest：空
template <> core::ByteVec serialize<ListTagsRequest>(const ListTagsRequest&) {
    return {};
}
template <> core::Result<ListTagsRequest> deserialize<ListTagsRequest>(core::ByteSpan) {
    return core::Result<ListTagsRequest>::Ok(ListTagsRequest{});
}

// GetTagRequest：id(i64)
template <> core::ByteVec serialize<GetTagRequest>(const GetTagRequest& v) {
    Writer w; w.write_i64(v.id); return w.take();
}
template <> core::Result<GetTagRequest> deserialize<GetTagRequest>(core::ByteSpan data) {
    Reader r(data); GetTagRequest v;
    if (!r.read_i64(v.id)) return core::Result<GetTagRequest>::Err(make_error("deserialize<GetTagRequest>: malformed"));
    return core::Result<GetTagRequest>::Ok(v);
}

// FindTagByNameRequest：name(string)
template <> core::ByteVec serialize<FindTagByNameRequest>(const FindTagByNameRequest& v) {
    Writer w; w.write_string(v.name); return w.take();
}
template <> core::Result<FindTagByNameRequest> deserialize<FindTagByNameRequest>(core::ByteSpan data) {
    Reader r(data); FindTagByNameRequest v;
    if (!r.read_string(v.name)) return core::Result<FindTagByNameRequest>::Err(make_error("deserialize<FindTagByNameRequest>: malformed"));
    return core::Result<FindTagByNameRequest>::Ok(std::move(v));
}

// GetEntryTagsRequest：entry_id(i64)
template <> core::ByteVec serialize<GetEntryTagsRequest>(const GetEntryTagsRequest& v) {
    Writer w; w.write_i64(v.entry_id); return w.take();
}
template <> core::Result<GetEntryTagsRequest> deserialize<GetEntryTagsRequest>(core::ByteSpan data) {
    Reader r(data); GetEntryTagsRequest v;
    if (!r.read_i64(v.entry_id)) return core::Result<GetEntryTagsRequest>::Err(make_error("deserialize<GetEntryTagsRequest>: malformed"));
    return core::Result<GetEntryTagsRequest>::Ok(v);
}

// SetEntryTagsRequest：entry_id(i64) + tag_ids(vec<i64>)
template <> core::ByteVec serialize<SetEntryTagsRequest>(const SetEntryTagsRequest& v) {
    Writer w;
    w.write_i64(v.entry_id);
    write_i64_vector(w, v.tag_ids);
    return w.take();
}
template <> core::Result<SetEntryTagsRequest> deserialize<SetEntryTagsRequest>(core::ByteSpan data) {
    Reader r(data); SetEntryTagsRequest v;
    if (!r.read_i64(v.entry_id)) return core::Result<SetEntryTagsRequest>::Err(make_error("deserialize<SetEntryTagsRequest>: malformed"));
    if (!read_i64_vector(r, v.tag_ids)) return core::Result<SetEntryTagsRequest>::Err(make_error("deserialize<SetEntryTagsRequest>: malformed"));
    return core::Result<SetEntryTagsRequest>::Ok(std::move(v));
}

// AddTagResponse：tag(Tag)
template <> core::ByteVec serialize<AddTagResponse>(const AddTagResponse& v) {
    Writer w; write_tag(w, v.tag); return w.take();
}
template <> core::Result<AddTagResponse> deserialize<AddTagResponse>(core::ByteSpan data) {
    Reader r(data); AddTagResponse v;
    if (!read_tag(r, v.tag)) return core::Result<AddTagResponse>::Err(make_error("deserialize<AddTagResponse>: malformed"));
    return core::Result<AddTagResponse>::Ok(std::move(v));
}

// UpdateTagResponse：tag(Tag)
template <> core::ByteVec serialize<UpdateTagResponse>(const UpdateTagResponse& v) {
    Writer w; write_tag(w, v.tag); return w.take();
}
template <> core::Result<UpdateTagResponse> deserialize<UpdateTagResponse>(core::ByteSpan data) {
    Reader r(data); UpdateTagResponse v;
    if (!read_tag(r, v.tag)) return core::Result<UpdateTagResponse>::Err(make_error("deserialize<UpdateTagResponse>: malformed"));
    return core::Result<UpdateTagResponse>::Ok(std::move(v));
}

// RemoveTagResponse：空
template <> core::ByteVec serialize<RemoveTagResponse>(const RemoveTagResponse&) {
    return {};
}
template <> core::Result<RemoveTagResponse> deserialize<RemoveTagResponse>(core::ByteSpan data) {
    if (!data.empty()) return core::Result<RemoveTagResponse>::Err(make_error("deserialize<RemoveTagResponse>: expected empty payload"));
    return core::Result<RemoveTagResponse>::Ok(RemoveTagResponse{});
}

// ListTagsResponse：tags(vec<Tag>)
template <> core::ByteVec serialize<ListTagsResponse>(const ListTagsResponse& v) {
    Writer w; write_tag_vector_inline(w, v.tags); return w.take();
}
template <> core::Result<ListTagsResponse> deserialize<ListTagsResponse>(core::ByteSpan data) {
    Reader r(data); ListTagsResponse v;
    if (!read_tag_vector_inline(r, v.tags)) return core::Result<ListTagsResponse>::Err(make_error("deserialize<ListTagsResponse>: malformed"));
    return core::Result<ListTagsResponse>::Ok(std::move(v));
}

// GetTagResponse：tag(Tag)
template <> core::ByteVec serialize<GetTagResponse>(const GetTagResponse& v) {
    Writer w; write_tag(w, v.tag); return w.take();
}
template <> core::Result<GetTagResponse> deserialize<GetTagResponse>(core::ByteSpan data) {
    Reader r(data); GetTagResponse v;
    if (!read_tag(r, v.tag)) return core::Result<GetTagResponse>::Err(make_error("deserialize<GetTagResponse>: malformed"));
    return core::Result<GetTagResponse>::Ok(std::move(v));
}

// FindTagByNameResponse：tag(Tag)
template <> core::ByteVec serialize<FindTagByNameResponse>(const FindTagByNameResponse& v) {
    Writer w; write_tag(w, v.tag); return w.take();
}
template <> core::Result<FindTagByNameResponse> deserialize<FindTagByNameResponse>(core::ByteSpan data) {
    Reader r(data); FindTagByNameResponse v;
    if (!read_tag(r, v.tag)) return core::Result<FindTagByNameResponse>::Err(make_error("deserialize<FindTagByNameResponse>: malformed"));
    return core::Result<FindTagByNameResponse>::Ok(std::move(v));
}

// GetEntryTagsResponse：tags(vec<Tag>)
template <> core::ByteVec serialize<GetEntryTagsResponse>(const GetEntryTagsResponse& v) {
    Writer w; write_tag_vector_inline(w, v.tags); return w.take();
}
template <> core::Result<GetEntryTagsResponse> deserialize<GetEntryTagsResponse>(core::ByteSpan data) {
    Reader r(data); GetEntryTagsResponse v;
    if (!read_tag_vector_inline(r, v.tags)) return core::Result<GetEntryTagsResponse>::Err(make_error("deserialize<GetEntryTagsResponse>: malformed"));
    return core::Result<GetEntryTagsResponse>::Ok(std::move(v));
}

// SetEntryTagsResponse：空
template <> core::ByteVec serialize<SetEntryTagsResponse>(const SetEntryTagsResponse&) {
    return {};
}
template <> core::Result<SetEntryTagsResponse> deserialize<SetEntryTagsResponse>(core::ByteSpan data) {
    if (!data.empty()) return core::Result<SetEntryTagsResponse>::Err(make_error("deserialize<SetEntryTagsResponse>: expected empty payload"));
    return core::Result<SetEntryTagsResponse>::Ok(SetEntryTagsResponse{});
}

// ErrorResponse 在序列化时加魔数前缀，使其字节布局与任何正常响应不兼容。
// 这样客户端先尝试期望响应类型反序列化时会失败（read_string 因长度过大而越界），
// 从而回退到 ErrorResponse 解析。
constexpr uint32_t kErrorResponseMagic = 0xDEADBEEFu;

template <> core::ByteVec serialize<ErrorResponse>(const ErrorResponse& v) {
    Writer w;
    w.write_u32(kErrorResponseMagic);
    w.write_u32(static_cast<uint32_t>(v.code));
    w.write_string(v.message);
    return w.take();
}
template <> core::Result<ErrorResponse> deserialize<ErrorResponse>(core::ByteSpan data) {
    Reader r(data); ErrorResponse v;
    uint32_t magic = 0;
    if (!r.read_u32(magic)) return core::Result<ErrorResponse>::Err(make_error("deserialize<ErrorResponse>: malformed"));
    if (magic != kErrorResponseMagic) return core::Result<ErrorResponse>::Err(make_error("deserialize<ErrorResponse>: magic mismatch"));
    uint32_t code = 0;
    if (!r.read_u32(code)) return core::Result<ErrorResponse>::Err(make_error("deserialize<ErrorResponse>: malformed"));
    v.code = static_cast<core::ErrorCode>(code);
    if (!r.read_string(v.message)) return core::Result<ErrorResponse>::Err(make_error("deserialize<ErrorResponse>: malformed"));
    return core::Result<ErrorResponse>::Ok(std::move(v));
}

// ---------------------------------------------------------------------------
// 消息帧封包/解包
// ---------------------------------------------------------------------------

core::ByteVec pack_message(CommandId cmd, uint32_t request_id, core::ByteSpan payload) {
    MessageHeader h;
    h.magic = kMagic;
    h.version = kProtocolVersion;
    h.command = cmd;
    h.request_id = request_id;
    h.payload_size = static_cast<uint32_t>(payload.size());

    core::ByteVec out;
    out.reserve(sizeof(h) + payload.size());
    const auto* hp = reinterpret_cast<const std::byte*>(&h);
    out.insert(out.end(), hp, hp + sizeof(h));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

core::Result<std::pair<MessageHeader, size_t>> parse_header(core::ByteSpan data) {
    if (data.size() < sizeof(MessageHeader)) {
        return core::Result<std::pair<MessageHeader, size_t>>::Err(
            core::Error(core::ErrorCode::IpcError,
                        "parse_header: data too short for header (need 16 bytes)"));
    }
    MessageHeader h;
    std::memcpy(&h, data.data(), sizeof(h));
    if (h.magic != kMagic) {
        return core::Result<std::pair<MessageHeader, size_t>>::Err(
            core::Error(core::ErrorCode::IpcError,
                        "parse_header: magic mismatch"));
    }
    return core::Result<std::pair<MessageHeader, size_t>>::Ok(
        std::make_pair(h, sizeof(MessageHeader)));
}

}  // namespace yuli::vault::protocol

// coding: utf-8
// =============================================================================
// test_e2e_flow.cpp
//
// PwdVault 端到端集成测试。模拟 UI 进程调用服务进程的核心用户流程，
// 跳过命名管道 I/O，通过 ServiceCore::handle_request 直接调用 ServiceCore。
//
// 测试策略：
//   - 实例化真实的 CryptoEngine / InMemoryStorageEngine / PasswordGenerator
//   - 使用临时目录下的 vault.meta 文件，避免污染用户数据
//   - 通过 protocol::serialize / deserialize 在协议层进行序列化往返，
//     最大化覆盖"UI 序列化 → service 反序列化 → 业务逻辑 → 序列化 → UI 反序列化"
//     的完整链路（命名管道传输环节除外）。
//   - 失败响应统一按 ErrorResponse 反序列化，提取 ErrorCode 与 message 进行断言。
//
// 状态机说明：
//   - ServiceCore 启动时自动检测 vault.meta 是否存在：
//     - 不存在 → 明文模式（password_enabled=false，自动 unlocked=true）
//     - 存在   → 加密模式（password_enabled=true，unlocked=false，需 Unlock）
//   - EnableProgramPassword：明文 → 加密，重新加密所有现有条目
//   - DisableProgramPassword：加密 → 明文，解密所有条目并删除 vault.meta
//   - ChangeProgramPassword：仅重新包装 encryption_key，条目不变
//
// 覆盖流程：
//   1. 明文模式启动（自动解锁，可直接 CRUD）
//   2. GetVaultStatus 返回正确状态
//   3. 启用程序密码（含已有条目重新加密）
//   4. 锁定后访问被拒（list_entries 返回 Unauthorized）
//   5. 解锁（正确密码成功；错误密码失败；连续 5 次错误后进入冷却期）
//   6. 禁用程序密码（条目解密回明文）
//   7. 修改程序密码
//   8. CRUD 全流程（add / get / list / search / update / remove）
//   9. 密码生成器（generate_password / estimate_strength）
//  10. 心跳 ping（返回合理的 server_timestamp）
// =============================================================================
#include "ServiceCore.h"

#include "CryptoEngine.h"
#include "InMemoryStorageEngine.h"
#include "PasswordGenerator.h"

#include "Commands.h"
#include "Messages.h"
#include "Serializer.h"

#include "Error.h"
#include "Result.h"
#include "Types.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

/// 测试用程序密码（足够长以满足 Argon2id 推荐输入长度）。
constexpr const char* kTestProgramPassword = "ProgramPass123!";

/// 生成唯一的临时 meta 文件路径，每个测试用例独立使用，避免相互干扰。
std::filesystem::path make_unique_meta_path() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    std::random_device rd;
    const uint64_t rnd = (static_cast<uint64_t>(rd()) << 32) | rd();
    const auto name = "pwdvault_test_" + std::to_string(n) + "_" +
                      std::to_string(rnd) + ".meta";
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

namespace yuli::vault::test {

/// 端到端流程测试夹具。
///
/// 每个 TEST_F 实例在 SetUp 中构造一个全新的 ServiceCore，使用独立的临时
/// meta 文件与 InMemoryStorageEngine，确保测试间相互隔离、可重复执行。
class E2EFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        meta_path_ = make_unique_meta_path();

        // 与 service main.cpp 一致：CryptoEngine 用空 encryption_key 构造，
        // 仅用于 derive_key / generate_key_and_iv；entry 加密用的 encryption_key
        // 由 ServiceCore 在 EnableProgramPassword/Unlock 后通过 set_encryption_key
        // 内部构造独立实例。
        crypto_ = std::make_unique<crypto::CryptoEngine>(core::ByteSpan{});
        storage_ = std::make_unique<storage::InMemoryStorageEngine>();
        generator_ = std::make_unique<generator::PasswordGenerator>();

        core_ = std::make_unique<service::ServiceCore>(
            std::move(crypto_), std::move(storage_), std::move(generator_),
            meta_path_);
    }

    void TearDown() override {
        core_.reset();
        std::error_code ec;
        std::filesystem::remove(meta_path_, ec);
        // 忽略删除失败（如文件未创建）
    }

    // ------------------------------------------------------------------------
    // 协议层辅助：发送请求并解析响应
    // ------------------------------------------------------------------------

    /// 发送一个 IPC 请求，返回反序列化后的响应或错误。
    ///
    /// 协议链路完全模拟真实 UI 行为：
    ///   1. 序列化 Req 为 payload
    ///   2. 构造 MessageHeader（与 IpcClient::send_request 一致）
    ///   3. 调用 ServiceCore::handle_request(payload, header)
    ///   4. 优先按 Resp 反序列化响应
    ///   5. 若失败，按 ErrorResponse 反序列化，提取 ErrorCode/message
    template <typename Req, typename Resp>
    core::Result<Resp> send(protocol::CommandId cmd, const Req& req) {
        core::ByteVec payload = protocol::serialize(req);

        protocol::MessageHeader header;
        header.magic = protocol::kMagic;
        header.version = protocol::kProtocolVersion;
        header.command = cmd;
        header.request_id = ++next_request_id_;
        header.payload_size = static_cast<uint32_t>(payload.size());

        core::ByteVec resp_bytes = core_->handle_request(payload, header);

        // 优先按期望响应类型反序列化
        core::ByteSpan resp_span(resp_bytes.data(), resp_bytes.size());
        auto resp_result = protocol::deserialize<Resp>(resp_span);
        if (resp_result.ok()) {
            return core::Result<Resp>::Ok(std::move(resp_result).value());
        }

        // 否则尝试作为 ErrorResponse 解析（service 处理失败的统一错误响应）
        auto err_result = protocol::deserialize<protocol::ErrorResponse>(resp_span);
        if (err_result.ok()) {
            const auto& err_resp = err_result.value();
            return core::Result<Resp>::Err(
                core::Error(err_resp.code, err_resp.message));
        }

        return core::Result<Resp>::Err(core::Error(
            core::ErrorCode::IpcError, "反序列化响应失败"));
    }

    /// 针对无请求负载的命令（Ping / Lock / ListEntries / GetVaultStatus）发送空负载。
    template <typename Resp>
    core::Result<Resp> send_empty(protocol::CommandId cmd) {
        core::ByteVec payload;  // 空 payload

        protocol::MessageHeader header;
        header.magic = protocol::kMagic;
        header.version = protocol::kProtocolVersion;
        header.command = cmd;
        header.request_id = ++next_request_id_;
        header.payload_size = 0;

        core::ByteVec resp_bytes = core_->handle_request(payload, header);

        core::ByteSpan resp_span(resp_bytes.data(), resp_bytes.size());
        auto resp_result = protocol::deserialize<Resp>(resp_span);
        if (resp_result.ok()) {
            return core::Result<Resp>::Ok(std::move(resp_result).value());
        }

        auto err_result = protocol::deserialize<protocol::ErrorResponse>(resp_span);
        if (err_result.ok()) {
            const auto& err_resp = err_result.value();
            return core::Result<Resp>::Err(
                core::Error(err_resp.code, err_resp.message));
        }

        return core::Result<Resp>::Err(core::Error(
            core::ErrorCode::IpcError, "反序列化响应失败"));
    }

    /// 便捷：启用程序密码，使 vault 进入加密已解锁状态。
    /// 多个 TEST_F 用例在执行 CRUD / generate 等需已解锁的操作前调用此辅助。
    void enable_program_password(const std::string& password = kTestProgramPassword) {
        protocol::EnableProgramPasswordRequest req;
        req.password = password;
        auto r = send<protocol::EnableProgramPasswordRequest,
                      protocol::EnableProgramPasswordResponse>(
            protocol::CommandId::EnableProgramPassword, req);
        ASSERT_TRUE(r.ok()) << "enable_program_password 失败: " << r.error().what();
        ASSERT_TRUE(r.value().success)
            << "enable_program_password 返回 success=false: "
            << r.value().error_message;
    }

    /// 便捷：执行 lock 命令（仅在加密模式下有效）。
    void lock() {
        auto r = send_empty<protocol::LockResponse>(protocol::CommandId::Lock);
        ASSERT_TRUE(r.ok()) << "lock 失败: " << r.error().what();
    }

    /// 便捷：执行 unlock。
    core::Result<protocol::UnlockResponse> unlock(const std::string& password) {
        protocol::UnlockRequest req;
        req.password = password;
        return send<protocol::UnlockRequest, protocol::UnlockResponse>(
            protocol::CommandId::Unlock, req);
    }

    /// 便捷：查询 vault 状态。
    core::Result<protocol::GetVaultStatusResponse> get_vault_status() {
        return send_empty<protocol::GetVaultStatusResponse>(
            protocol::CommandId::GetVaultStatus);
    }

    std::unique_ptr<service::ServiceCore> core_;
    std::filesystem::path meta_path_;

private:
    uint32_t next_request_id_ = 0;
    std::unique_ptr<crypto::CryptoEngine> crypto_;
    std::unique_ptr<storage::InMemoryStorageEngine> storage_;
    std::unique_ptr<generator::PasswordGenerator> generator_;
};

// =============================================================================
// 1. 明文模式启动（无 vault.meta 时自动进入明文模式）
// =============================================================================

/// 启动时无 vault.meta，应自动进入明文模式（password_enabled=false, is_locked=false）。
TEST_F(E2EFlowTest, StartupWithoutMetaEntersPlaintextMode) {
    auto r = get_vault_status();
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().password_enabled);
    EXPECT_FALSE(r.value().is_locked);
}

/// 明文模式下可直接调用 list_entries（无需解锁），返回空列表。
TEST_F(E2EFlowTest, PlaintextModeAllowsCrudWithoutUnlock) {
    auto r = send_empty<protocol::ListEntriesResponse>(
        protocol::CommandId::ListEntries);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().entries.empty());
}

/// 明文模式下调用 lock 应返回错误（不支持锁定）。
TEST_F(E2EFlowTest, LockInPlaintextModeReturnsError) {
    auto r = send_empty<protocol::LockResponse>(protocol::CommandId::Lock);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, core::ErrorCode::InvalidArgument);
}

/// 明文模式下调用 unlock 应直接返回 success=true（无需密码）。
TEST_F(E2EFlowTest, UnlockInPlaintextModeSucceedsWithoutPassword) {
    auto r = unlock("");
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().success);
}

// =============================================================================
// 2. GetVaultStatus 状态查询
// =============================================================================

/// 启用程序密码后，GetVaultStatus 应返回 password_enabled=true, is_locked=false。
TEST_F(E2EFlowTest, GetVaultStatusAfterEnable) {
    enable_program_password();

    auto r = get_vault_status();
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().password_enabled);
    EXPECT_FALSE(r.value().is_locked);
}

/// 启用程序密码并锁定后，GetVaultStatus 应返回 password_enabled=true, is_locked=true。
TEST_F(E2EFlowTest, GetVaultStatusAfterLock) {
    enable_program_password();
    lock();

    auto r = get_vault_status();
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().password_enabled);
    EXPECT_TRUE(r.value().is_locked);
}

// =============================================================================
// 3. 启用程序密码
// =============================================================================

/// 首次启用程序密码应成功。
TEST_F(E2EFlowTest, EnableProgramPasswordSucceeds) {
    protocol::EnableProgramPasswordRequest req;
    req.password = kTestProgramPassword;
    auto r = send<protocol::EnableProgramPasswordRequest,
                  protocol::EnableProgramPasswordResponse>(
        protocol::CommandId::EnableProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().success);
    EXPECT_TRUE(r.value().error_message.empty());
}

/// 已启用后再次启用应返回 success=false。
TEST_F(E2EFlowTest, EnableTwiceReturnsFailure) {
    enable_program_password();

    protocol::EnableProgramPasswordRequest req;
    req.password = kTestProgramPassword;
    auto r = send<protocol::EnableProgramPasswordRequest,
                  protocol::EnableProgramPasswordResponse>(
        protocol::CommandId::EnableProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
}

/// 启用程序密码后，已有明文条目应被重新加密。
/// 验证：启用前添加明文条目 → 启用 → 锁定 → 解锁 → 读取条目，password 应恢复原文。
TEST_F(E2EFlowTest, EnableReencryptsExistingEntries) {
    // 1. 明文模式下添加条目
    protocol::AddEntryRequest add_req;
    add_req.entry.title = "GitHub";
    add_req.entry.account = "user1";
    add_req.entry.website = "github.com";
    add_req.entry.username = "user1";
    add_req.entry.password = "p@ssw0rd";
    add_req.entry.note = "明文模式添加";
    auto add_resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, add_req);
    ASSERT_TRUE(add_resp.ok()) << add_resp.error().what();
    const int64_t id = add_resp.value().entry.id;
    ASSERT_GT(id, 0);

    // 2. 启用程序密码（应触发重新加密）
    enable_program_password();

    // 3. 锁定后解锁
    lock();
    auto unlock_r = unlock(kTestProgramPassword);
    ASSERT_TRUE(unlock_r.ok() && unlock_r.value().success)
        << unlock_r.error().what();

    // 4. 读取条目，password 应恢复原文
    protocol::GetEntryRequest get_req;
    get_req.id = id;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_TRUE(get_resp.ok()) << get_resp.error().what();
    EXPECT_EQ(get_resp.value().entry.password, "p@ssw0rd");
    EXPECT_EQ(get_resp.value().entry.website, "github.com");
}

// =============================================================================
// 4. 未解锁访问被拒（加密模式）
// =============================================================================

/// 加密模式下 lock() 后再访问 list_entries 应返回 Unauthorized。
TEST_F(E2EFlowTest, ListEntriesAfterLockReturnsUnauthorized) {
    enable_program_password();
    lock();

    auto r = send_empty<protocol::ListEntriesResponse>(
        protocol::CommandId::ListEntries);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, core::ErrorCode::Unauthorized);
}

/// 加密模式下 lock 后 add_entry 也应返回 Unauthorized。
TEST_F(E2EFlowTest, AddEntryAfterLockReturnsUnauthorized) {
    enable_program_password();
    lock();

    protocol::AddEntryRequest req;
    req.entry.title = "GitHub";
    req.entry.account = "user1";
    req.entry.website = "github.com";
    req.entry.username = "user1";
    req.entry.password = "p@ssw0rd";
    req.entry.note = "工作账号";

    auto r = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, req);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, core::ErrorCode::Unauthorized);
}

// =============================================================================
// 5. 解锁流程（加密模式）
// =============================================================================

/// 加密模式下 lock 后用正确密码 unlock 应成功。
TEST_F(E2EFlowTest, UnlockWithCorrectPasswordSucceeds) {
    enable_program_password();
    lock();

    auto r = unlock(kTestProgramPassword);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().success);
    EXPECT_TRUE(r.value().error_message.empty());
}

/// 加密模式下 lock 后用错误密码 unlock 应失败（success=false）。
TEST_F(E2EFlowTest, UnlockWithWrongPasswordFails) {
    enable_program_password();
    lock();

    auto r = unlock("WrongPassword");
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
    EXPECT_FALSE(r.value().error_message.empty());
}

/// 连续 5 次错误密码后，第 6 次（即使使用正确密码）应进入冷却期并返回
/// success=false 与 "已锁定" 提示（含剩余秒数供 UI 解析）。
TEST_F(E2EFlowTest, FiveFailedUnlocksTriggerCooldown) {
    enable_program_password();
    lock();

    // 前 5 次错误密码：均返回 success=false
    for (int i = 0; i < 5; ++i) {
        auto r = unlock("WrongPassword");
        ASSERT_TRUE(r.ok()) << "第 " << (i + 1) << " 次 unlock 调用应返回响应";
        EXPECT_FALSE(r.value().success)
            << "第 " << (i + 1) << " 次 unlock 不应成功";
    }

    // 第 6 次：即使使用正确密码，也应被冷却拦截
    auto r = unlock(kTestProgramPassword);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
    EXPECT_NE(r.value().error_message.find("Locked"),
              std::string::npos)
        << "should mention lockout, got: " << r.value().error_message;
    EXPECT_NE(r.value().error_message.find("seconds"),
              std::string::npos)
        << "should include remaining seconds, got: " << r.value().error_message;
}

// =============================================================================
// 6. 禁用程序密码
// =============================================================================

/// 加密模式下禁用程序密码应成功，并切换回明文模式。
TEST_F(E2EFlowTest, DisableProgramPasswordSucceeds) {
    enable_program_password();

    protocol::DisableProgramPasswordRequest req;
    req.password = kTestProgramPassword;
    auto r = send<protocol::DisableProgramPasswordRequest,
                  protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().success);

    // 验证已切换回明文模式
    auto status = get_vault_status();
    ASSERT_TRUE(status.ok());
    EXPECT_FALSE(status.value().password_enabled);
    EXPECT_FALSE(status.value().is_locked);
}

/// 禁用程序密码时密码错误应失败。
TEST_F(E2EFlowTest, DisableWithWrongPasswordFails) {
    enable_program_password();
    lock();  // 锁定后需要验证密码

    protocol::DisableProgramPasswordRequest req;
    req.password = "WrongPassword";
    auto r = send<protocol::DisableProgramPasswordRequest,
                  protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
}

/// 明文模式下调用禁用应返回 success=false。
TEST_F(E2EFlowTest, DisableInPlaintextModeFails) {
    protocol::DisableProgramPasswordRequest req;
    req.password = "any-password";
    auto r = send<protocol::DisableProgramPasswordRequest,
                  protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
}

/// 禁用程序密码后，已加密条目应被解密回明文。
TEST_F(E2EFlowTest, DisableDecryptsEntriesBackToPlaintext) {
    // 1. 启用程序密码并添加条目
    enable_program_password();
    protocol::AddEntryRequest add_req;
    add_req.entry.title = "GitHub";
    add_req.entry.account = "user1";
    add_req.entry.website = "github.com";
    add_req.entry.username = "user1";
    add_req.entry.password = "p@ssw0rd";
    auto add_resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, add_req);
    ASSERT_TRUE(add_resp.ok());
    const int64_t id = add_resp.value().entry.id;

    // 2. 禁用程序密码
    protocol::DisableProgramPasswordRequest dis_req;
    dis_req.password = kTestProgramPassword;
    auto dis_r = send<protocol::DisableProgramPasswordRequest,
                      protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, dis_req);
    ASSERT_TRUE(dis_r.ok() && dis_r.value().success);

    // 3. 读取条目，password 应仍为原文（明文模式）
    protocol::GetEntryRequest get_req;
    get_req.id = id;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_TRUE(get_resp.ok()) << get_resp.error().what();
    EXPECT_EQ(get_resp.value().entry.password, "p@ssw0rd");
}

// =============================================================================
// 7. 修改程序密码
// =============================================================================

/// 修改程序密码应成功，且不影响条目解密。
TEST_F(E2EFlowTest, ChangeProgramPasswordSucceeds) {
    enable_program_password();

    // 修改密码
    protocol::ChangeProgramPasswordRequest req;
    req.old_password = kTestProgramPassword;
    req.new_password = "NewProgramPass456!";
    auto r = send<protocol::ChangeProgramPasswordRequest,
                  protocol::ChangeProgramPasswordResponse>(
        protocol::CommandId::ChangeProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_TRUE(r.value().success);

    // 锁定后用新密码解锁应成功
    lock();
    auto unlock_r = unlock("NewProgramPass456!");
    ASSERT_TRUE(unlock_r.ok() && unlock_r.value().success);
}

/// 修改程序密码时旧密码错误应失败。
TEST_F(E2EFlowTest, ChangeWithWrongOldPasswordFails) {
    enable_program_password();

    protocol::ChangeProgramPasswordRequest req;
    req.old_password = "WrongOldPassword";
    req.new_password = "NewProgramPass456!";
    auto r = send<protocol::ChangeProgramPasswordRequest,
                  protocol::ChangeProgramPasswordResponse>(
        protocol::CommandId::ChangeProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
}

/// 明文模式下调用修改密码应失败。
TEST_F(E2EFlowTest, ChangeInPlaintextModeFails) {
    protocol::ChangeProgramPasswordRequest req;
    req.old_password = "any";
    req.new_password = "new";
    auto r = send<protocol::ChangeProgramPasswordRequest,
                  protocol::ChangeProgramPasswordResponse>(
        protocol::CommandId::ChangeProgramPassword, req);
    ASSERT_TRUE(r.ok()) << r.error().what();
    EXPECT_FALSE(r.value().success);
}

/// 修改程序密码后，已有条目应仍可正常解密。
TEST_F(E2EFlowTest, ChangePasswordPreservesEntries) {
    enable_program_password();

    // 添加条目
    protocol::AddEntryRequest add_req;
    add_req.entry.title = "GitHub";
    add_req.entry.account = "user1";
    add_req.entry.website = "github.com";
    add_req.entry.username = "user1";
    add_req.entry.password = "p@ssw0rd";
    auto add_resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, add_req);
    ASSERT_TRUE(add_resp.ok());
    const int64_t id = add_resp.value().entry.id;

    // 修改密码
    protocol::ChangeProgramPasswordRequest chg_req;
    chg_req.old_password = kTestProgramPassword;
    chg_req.new_password = "NewProgramPass456!";
    auto chg_r = send<protocol::ChangeProgramPasswordRequest,
                      protocol::ChangeProgramPasswordResponse>(
        protocol::CommandId::ChangeProgramPassword, chg_req);
    ASSERT_TRUE(chg_r.ok() && chg_r.value().success);

    // 锁定 → 用新密码解锁 → 读取条目，password 应恢复原文
    lock();
    auto unlock_r = unlock("NewProgramPass456!");
    ASSERT_TRUE(unlock_r.ok() && unlock_r.value().success);

    protocol::GetEntryRequest get_req;
    get_req.id = id;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_TRUE(get_resp.ok()) << get_resp.error().what();
    EXPECT_EQ(get_resp.value().entry.password, "p@ssw0rd");
}

// =============================================================================
// 8. CRUD 全流程（加密模式）
// =============================================================================

/// add_entry 应返回带 id 的 entry，id > 0；get_entry 应返回字段一致的原条目。
/// 验证 password 字段经加解密往返后恢复原文。
TEST_F(E2EFlowTest, AddAndGetEntryRoundtrip) {
    enable_program_password();

    protocol::AddEntryRequest add_req;
    add_req.entry.title = "GitHub";
    add_req.entry.account = "user1";
    add_req.entry.website = "github.com";
    add_req.entry.username = "user1";
    add_req.entry.password = "p@ssw0rd";
    add_req.entry.note = "工作账号";

    auto add_resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, add_req);
    ASSERT_TRUE(add_resp.ok()) << add_resp.error().what();
    const auto& added = add_resp.value().entry;
    EXPECT_GT(added.id, 0);
    EXPECT_EQ(added.website, "github.com");
    EXPECT_EQ(added.username, "user1");
    EXPECT_EQ(added.password, "p@ssw0rd");   // 响应中应为明文
    EXPECT_EQ(added.note, "工作账号");
    EXPECT_GT(added.created_at, 0);
    EXPECT_GE(added.updated_at, added.created_at);

    // get_entry 验证字段一致（含 password 经加解密往返后恢复原文）
    protocol::GetEntryRequest get_req;
    get_req.id = added.id;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_TRUE(get_resp.ok()) << get_resp.error().what();
    const auto& got = get_resp.value().entry;
    EXPECT_EQ(got.id, added.id);
    EXPECT_EQ(got.website, added.website);
    EXPECT_EQ(got.username, added.username);
    EXPECT_EQ(got.password, "p@ssw0rd");  // 关键：解密后恢复原文
    EXPECT_EQ(got.note, added.note);
    EXPECT_EQ(got.created_at, added.created_at);
    EXPECT_EQ(got.updated_at, added.updated_at);
    // 响应中 iv/tag 应被清空（decrypt_entry 清空）
    EXPECT_TRUE(got.iv.empty());
    EXPECT_TRUE(got.tag.empty());
}

/// 添加多条不同网站的条目，list_entries 应返回全部。
TEST_F(E2EFlowTest, ListEntriesReturnsAllAdded) {
    enable_program_password();

    auto add_one = [this](const std::string& website,
                         const std::string& username) -> int64_t {
        protocol::AddEntryRequest req;
        req.entry.title = website;
        req.entry.account = username;
        req.entry.website = website;
        req.entry.username = username;
        req.entry.password = "p@ssw0rd-" + website;
        req.entry.note = "";
        auto resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
            protocol::CommandId::AddEntry, req);
        EXPECT_TRUE(resp.ok()) << resp.error().what();
        return resp.ok() ? resp.value().entry.id : 0;
    };

    const int64_t id1 = add_one("github.com", "user1");
    const int64_t id2 = add_one("gitlab.com", "user2");
    const int64_t id3 = add_one("bitbucket.org", "user3");
    ASSERT_GT(id1, 0);
    ASSERT_GT(id2, 0);
    ASSERT_GT(id3, 0);
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);

    auto list_resp = send_empty<protocol::ListEntriesResponse>(
        protocol::CommandId::ListEntries);
    ASSERT_TRUE(list_resp.ok()) << list_resp.error().what();
    EXPECT_GE(list_resp.value().entries.size(), 3u);

    // 验证每条 password 字段已解密恢复原文
    bool found_github = false;
    for (const auto& e : list_resp.value().entries) {
        if (e.id == id1) {
            EXPECT_EQ(e.website, "github.com");
            EXPECT_EQ(e.username, "user1");
            EXPECT_EQ(e.password, "p@ssw0rd-github.com");
            found_github = true;
        }
    }
    EXPECT_TRUE(found_github);
}

/// search_entries 限定字段时应只返回匹配的条目。
TEST_F(E2EFlowTest, SearchEntriesByWebsiteReturnsMatchOnly) {
    enable_program_password();

    // 添加 3 条不同网站条目
    auto add_one = [this](const std::string& website,
                         const std::string& username) -> int64_t {
        protocol::AddEntryRequest req;
        req.entry.title = website;
        req.entry.account = username;
        req.entry.website = website;
        req.entry.username = username;
        req.entry.password = "p@ssw0rd";
        auto resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
            protocol::CommandId::AddEntry, req);
        return resp.ok() ? resp.value().entry.id : 0;
    };
    ASSERT_GT(add_one("github.com", "user1"), 0);
    ASSERT_GT(add_one("gitlab.com", "user2"), 0);
    ASSERT_GT(add_one("bitbucket.org", "user3"), 0);

    // 搜索 "github" 仅匹配 website 字段
    protocol::SearchEntriesRequest search_req;
    search_req.query.text = "github";
    search_req.query.fields = { "website" };
    search_req.query.case_sensitive = false;

    auto search_resp =
        send<protocol::SearchEntriesRequest, protocol::SearchEntriesResponse>(
            protocol::CommandId::SearchEntries, search_req);
    ASSERT_TRUE(search_resp.ok()) << search_resp.error().what();
    ASSERT_EQ(search_resp.value().entries.size(), 1u);
    const auto& hit = search_resp.value().entries.front();
    EXPECT_EQ(hit.website, "github.com");
    EXPECT_EQ(hit.username, "user1");
    EXPECT_EQ(hit.password, "p@ssw0rd");  // 解密后恢复原文
}

/// update_entry 应更新字段并返回最新值；get_entry 应反映更新。
TEST_F(E2EFlowTest, UpdateEntryModifiesFields) {
    enable_program_password();

    protocol::AddEntryRequest add_req;
    add_req.entry.title = "GitHub";
    add_req.entry.account = "user1";
    add_req.entry.website = "github.com";
    add_req.entry.username = "user1";
    add_req.entry.password = "p@ssw0rd";
    add_req.entry.note = "工作账号";

    auto add_resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, add_req);
    ASSERT_TRUE(add_resp.ok()) << add_resp.error().what();
    const int64_t id = add_resp.value().entry.id;
    ASSERT_GT(id, 0);

    // 等待 1 秒以确保 updated_at 严格大于原值
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 更新 username 与 note
    protocol::UpdateEntryRequest upd_req;
    upd_req.entry = add_resp.value().entry;
    upd_req.entry.username = "user1_updated";
    upd_req.entry.note = "个人账号";
    upd_req.entry.password = "new-p@ssw0rd-2024";

    auto upd_resp =
        send<protocol::UpdateEntryRequest, protocol::UpdateEntryResponse>(
            protocol::CommandId::UpdateEntry, upd_req);
    ASSERT_TRUE(upd_resp.ok()) << upd_resp.error().what();
    const auto& updated = upd_resp.value().entry;
    EXPECT_EQ(updated.id, id);
    EXPECT_EQ(updated.username, "user1_updated");
    EXPECT_EQ(updated.note, "个人账号");
    EXPECT_EQ(updated.password, "new-p@ssw0rd-2024");  // 响应中为明文
    EXPECT_GT(updated.updated_at, updated.created_at);

    // get_entry 验证已持久化
    protocol::GetEntryRequest get_req;
    get_req.id = id;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_TRUE(get_resp.ok()) << get_resp.error().what();
    EXPECT_EQ(get_resp.value().entry.username, "user1_updated");
    EXPECT_EQ(get_resp.value().entry.note, "个人账号");
    EXPECT_EQ(get_resp.value().entry.password, "new-p@ssw0rd-2024");
}

/// remove_entry 应成功；之后 get_entry 应返回 NotFound。
TEST_F(E2EFlowTest, RemoveEntryThenGetReturnsNotFound) {
    enable_program_password();

    protocol::AddEntryRequest add_req;
    add_req.entry.title = "GitHub";
    add_req.entry.account = "user1";
    add_req.entry.website = "github.com";
    add_req.entry.username = "user1";
    add_req.entry.password = "p@ssw0rd";
    add_req.entry.note = "";

    auto add_resp = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
        protocol::CommandId::AddEntry, add_req);
    ASSERT_TRUE(add_resp.ok()) << add_resp.error().what();
    const int64_t id = add_resp.value().entry.id;
    ASSERT_GT(id, 0);

    // remove_entry
    protocol::RemoveEntryRequest rm_req;
    rm_req.id = id;
    auto rm_resp =
        send<protocol::RemoveEntryRequest, protocol::RemoveEntryResponse>(
            protocol::CommandId::RemoveEntry, rm_req);
    ASSERT_TRUE(rm_resp.ok()) << rm_resp.error().what();

    // get_entry 应返回 NotFound
    protocol::GetEntryRequest get_req;
    get_req.id = id;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_FALSE(get_resp.ok());
    EXPECT_EQ(get_resp.error().code, core::ErrorCode::NotFound);
}

/// remove 不存在的条目应返回 NotFound。
TEST_F(E2EFlowTest, RemoveNonexistentReturnsNotFound) {
    enable_program_password();

    protocol::RemoveEntryRequest rm_req;
    rm_req.id = 99999;
    auto rm_resp =
        send<protocol::RemoveEntryRequest, protocol::RemoveEntryResponse>(
            protocol::CommandId::RemoveEntry, rm_req);
    ASSERT_FALSE(rm_resp.ok());
    EXPECT_EQ(rm_resp.error().code, core::ErrorCode::NotFound);
}

/// get 不存在的条目应返回 NotFound。
TEST_F(E2EFlowTest, GetNonexistentReturnsNotFound) {
    enable_program_password();

    protocol::GetEntryRequest get_req;
    get_req.id = 88888;
    auto get_resp = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
        protocol::CommandId::GetEntry, get_req);
    ASSERT_FALSE(get_resp.ok());
    EXPECT_EQ(get_resp.error().code, core::ErrorCode::NotFound);
}

// =============================================================================
// 9. 密码生成器
// =============================================================================

/// generate_password 应返回指定长度的密码。
TEST_F(E2EFlowTest, GeneratePasswordReturnsExpectedLength) {
    enable_program_password();

    protocol::GeneratePasswordRequest req;
    req.options.length = 20;
    req.options.use_uppercase = true;
    req.options.use_lowercase = true;
    req.options.use_digits = true;
    req.options.use_symbols = true;
    req.options.exclude_ambiguous = false;

    auto resp =
        send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, req);
    ASSERT_TRUE(resp.ok()) << resp.error().what();
    EXPECT_EQ(resp.value().password.size(), 20u);
}

/// generate_password 在已锁定时应返回 Unauthorized。
TEST_F(E2EFlowTest, GeneratePasswordAfterLockReturnsUnauthorized) {
    enable_program_password();
    lock();

    protocol::GeneratePasswordRequest req;
    req.options.length = 16;
    auto resp =
        send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, req);
    ASSERT_FALSE(resp.ok());
    EXPECT_EQ(resp.error().code, core::ErrorCode::Unauthorized);
}

/// estimate_strength 对弱密码返回较低等级，对强密码返回较高等级。
TEST_F(E2EFlowTest, EstimateStrengthOrdersWeakAndStrong) {
    enable_program_password();

    protocol::EstimateStrengthRequest weak_req;
    weak_req.password = "short";
    auto weak_resp =
        send<protocol::EstimateStrengthRequest, protocol::EstimateStrengthResponse>(
            protocol::CommandId::EstimateStrength, weak_req);
    ASSERT_TRUE(weak_resp.ok()) << weak_resp.error().what();
    const auto& weak_est = weak_resp.value().estimate;
    EXPECT_GE(weak_est.bits, 0);
    EXPECT_EQ(weak_est.score, static_cast<int>(weak_est.level));
    EXPECT_GE(static_cast<int>(weak_est.level),
              static_cast<int>(core::StrengthLevel::VeryWeak));

    protocol::EstimateStrengthRequest strong_req;
    strong_req.password = "Very$tr0ng&P@ssw0rd!2024";
    auto strong_resp =
        send<protocol::EstimateStrengthRequest, protocol::EstimateStrengthResponse>(
            protocol::CommandId::EstimateStrength, strong_req);
    ASSERT_TRUE(strong_resp.ok()) << strong_resp.error().what();
    const auto& strong_est = strong_resp.value().estimate;

    EXPECT_GT(strong_est.bits, weak_est.bits)
        << "强密码 entropy=" << strong_est.bits
        << " 应大于弱密码 entropy=" << weak_est.bits;
    EXPECT_GE(static_cast<int>(strong_est.level),
              static_cast<int>(weak_est.level));
}

// =============================================================================
// 9b. 生成器历史记录
// =============================================================================

/// generate_password 应在生成密码后自动追加一条历史记录，list_generated_records 能取到。
TEST_F(E2EFlowTest, GeneratePasswordAppendsHistoryRecord) {
    enable_program_password();

    protocol::GeneratePasswordRequest gen_req;
    gen_req.options.length = 16;
    gen_req.options.use_lowercase = true;
    gen_req.options.use_digits = true;
    auto gen_resp =
        send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, gen_req);
    ASSERT_TRUE(gen_resp.ok()) << gen_resp.error().what();
    const std::string generated_pwd = gen_resp.value().password;
    ASSERT_FALSE(generated_pwd.empty());

    auto list_resp = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_resp.ok()) << list_resp.error().what();
    ASSERT_EQ(list_resp.value().records.size(), 1u);
    const auto& rec = list_resp.value().records[0];
    EXPECT_GT(rec.id, 0);
    EXPECT_GT(rec.created_at, 0);
    EXPECT_EQ(rec.password, generated_pwd);  // 解密后应恢复原文
    EXPECT_EQ(rec.length, 16);
    // 加密模式下 iv / tag 不为空（被解密后清空，故应为空）
    EXPECT_TRUE(rec.iv.empty());
    EXPECT_TRUE(rec.tag.empty());
}

/// 明文模式下生成的密码记录同样可被 list（明文存储，无 iv/tag）。
TEST_F(E2EFlowTest, GeneratePasswordAppendsHistoryRecordInPlaintextMode) {
    // 不启用程序密码 → 明文模式
    protocol::GeneratePasswordRequest gen_req;
    gen_req.options.length = 12;
    gen_req.options.use_uppercase = true;
    auto gen_resp =
        send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, gen_req);
    ASSERT_TRUE(gen_resp.ok()) << gen_resp.error().what();

    auto list_resp = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_resp.ok()) << list_resp.error().what();
    ASSERT_EQ(list_resp.value().records.size(), 1u);
    EXPECT_EQ(list_resp.value().records[0].length, 12);
    // 明文模式下 iv / tag 在响应中应为空
    EXPECT_TRUE(list_resp.value().records[0].iv.empty());
    EXPECT_TRUE(list_resp.value().records[0].tag.empty());
}

/// 启用 / 禁用程序密码触发生成记录重加密 / 解密时，created_at 必须保持不变
/// （回归测试：旧实现"先删后加"会重置 created_at 为当前时间）。
TEST_F(E2EFlowTest, EnableDisableProgramPasswordPreservesGeneratedRecordTimestamp) {
    // 1. 明文模式下生成一条记录，记录原始 created_at
    protocol::GeneratePasswordRequest gen_req;
    gen_req.options.length = 14;
    gen_req.options.use_lowercase = true;
    gen_req.options.use_digits = true;
    auto gen_resp =
        send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, gen_req);
    ASSERT_TRUE(gen_resp.ok()) << gen_resp.error().what();
    const std::string generated_pwd = gen_resp.value().password;

    auto list_before = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_before.ok()) << list_before.error().what();
    ASSERT_EQ(list_before.value().records.size(), 1u);
    const int64_t id = list_before.value().records[0].id;
    const int64_t original_ts = list_before.value().records[0].created_at;
    ASSERT_GT(original_ts, 0);

    // 2. 启用程序密码 → 触发明文 → 密文重加密
    enable_program_password();

    // 3. 验证记录仍存在且 created_at 未被重置
    auto list_after_enable = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_after_enable.ok()) << list_after_enable.error().what();
    ASSERT_EQ(list_after_enable.value().records.size(), 1u);
    const auto& rec_after_enable = list_after_enable.value().records[0];
    EXPECT_EQ(rec_after_enable.id, id);
    EXPECT_EQ(rec_after_enable.created_at, original_ts) << "enable 后 created_at 不应被重置";
    EXPECT_EQ(rec_after_enable.password, generated_pwd);  // 解密后恢复原文
    EXPECT_EQ(rec_after_enable.length, 14);

    // 4. 锁定 → 解锁，验证 created_at 在持久化后仍然不变
    lock();
    auto unlock_r = unlock(kTestProgramPassword);
    ASSERT_TRUE(unlock_r.ok() && unlock_r.value().success) << unlock_r.error().what();

    auto list_after_unlock = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_after_unlock.ok()) << list_after_unlock.error().what();
    ASSERT_EQ(list_after_unlock.value().records.size(), 1u);
    EXPECT_EQ(list_after_unlock.value().records[0].id, id);
    EXPECT_EQ(list_after_unlock.value().records[0].created_at, original_ts)
        << "unlock 后 created_at 不应被重置";
    EXPECT_EQ(list_after_unlock.value().records[0].password, generated_pwd);

    // 5. 禁用程序密码 → 触发密文 → 明文重解密
    protocol::DisableProgramPasswordRequest dis_req;
    dis_req.password = kTestProgramPassword;
    auto dis_resp = send<protocol::DisableProgramPasswordRequest,
                         protocol::DisableProgramPasswordResponse>(
        protocol::CommandId::DisableProgramPassword, dis_req);
    ASSERT_TRUE(dis_resp.ok()) << dis_resp.error().what();
    ASSERT_TRUE(dis_resp.value().success) << dis_resp.value().error_message;

    // 6. 最终验证记录与时间戳仍然完好
    auto list_after_disable = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_after_disable.ok()) << list_after_disable.error().what();
    ASSERT_EQ(list_after_disable.value().records.size(), 1u);
    const auto& rec_after_disable = list_after_disable.value().records[0];
    EXPECT_EQ(rec_after_disable.id, id);
    EXPECT_EQ(rec_after_disable.created_at, original_ts)
        << "disable 后 created_at 不应被重置";
    EXPECT_EQ(rec_after_disable.password, generated_pwd);
    EXPECT_TRUE(rec_after_disable.iv.empty());  // 明文模式 iv/tag 为空
    EXPECT_TRUE(rec_after_disable.tag.empty());
}

/// list_generated_records 在未解锁时应返回 Unauthorized。
TEST_F(E2EFlowTest, ListGeneratedRecordsAfterLockReturnsUnauthorized) {
    enable_program_password();
    lock();

    auto resp = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_FALSE(resp.ok());
    EXPECT_EQ(resp.error().code, core::ErrorCode::Unauthorized);
}

/// remove_generated_record 应删除指定记录。
TEST_F(E2EFlowTest, RemoveGeneratedRecordDeletesIt) {
    enable_program_password();

    // 生成 2 条记录
    for (int i = 0; i < 2; ++i) {
        protocol::GeneratePasswordRequest req;
        req.options.length = 10 + i;
        auto r = send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, req);
        ASSERT_TRUE(r.ok()) << r.error().what();
    }

    auto list = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list.ok());
    ASSERT_EQ(list.value().records.size(), 2u);
    const int64_t id_to_delete = list.value().records[0].id;

    // 删除第一条（最新）
    protocol::RemoveGeneratedRecordRequest rm_req;
    rm_req.id = id_to_delete;
    auto rm_resp = send<protocol::RemoveGeneratedRecordRequest,
                        protocol::RemoveGeneratedRecordResponse>(
        protocol::CommandId::RemoveGeneratedRecord, rm_req);
    ASSERT_TRUE(rm_resp.ok()) << rm_resp.error().what();

    auto list2 = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list2.ok());
    ASSERT_EQ(list2.value().records.size(), 1u);
    EXPECT_NE(list2.value().records[0].id, id_to_delete);
}

/// clear_generated_records 应清空全部记录。
TEST_F(E2EFlowTest, ClearGeneratedRecordsRemovesAll) {
    enable_program_password();

    for (int i = 0; i < 3; ++i) {
        protocol::GeneratePasswordRequest req;
        req.options.length = 12;
        auto r = send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, req);
        ASSERT_TRUE(r.ok());
    }
    auto list_before = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_before.ok());
    EXPECT_EQ(list_before.value().records.size(), 3u);

    auto clr_resp = send_empty<protocol::ClearGeneratedRecordsResponse>(
        protocol::CommandId::ClearGeneratedRecords);
    ASSERT_TRUE(clr_resp.ok()) << clr_resp.error().what();

    auto list_after = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_after.ok());
    EXPECT_TRUE(list_after.value().records.empty());
}

/// get_generator_settings 默认返回无限制（limit=0）。
TEST_F(E2EFlowTest, GetGeneratorSettingsDefaultsToUnlimited) {
    enable_program_password();

    auto resp = send_empty<protocol::GetGeneratorSettingsResponse>(
        protocol::CommandId::GetGeneratorSettings);
    ASSERT_TRUE(resp.ok()) << resp.error().what();
    EXPECT_EQ(resp.value().history_limit, 0);  // 默认无限制
}

/// set_generator_limit 应持久化设置，并立即清理超出上限的旧记录。
TEST_F(E2EFlowTest, SetGeneratorLimitEnforcesHistoryCap) {
    enable_program_password();

    // 生成 5 条记录
    for (int i = 0; i < 5; ++i) {
        protocol::GeneratePasswordRequest req;
        req.options.length = 8;
        auto r = send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, req);
        ASSERT_TRUE(r.ok()) << r.error().what();
    }

    auto list_before = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_before.ok());
    ASSERT_EQ(list_before.value().records.size(), 5u);

    // 设置上限为 2
    protocol::SetGeneratorLimitRequest set_req;
    set_req.limit = 2;
    auto set_resp = send<protocol::SetGeneratorLimitRequest,
                         protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, set_req);
    ASSERT_TRUE(set_resp.ok()) << set_resp.error().what();
    EXPECT_TRUE(set_resp.value().success);

    // 重新查询应只剩 2 条
    auto list_after = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list_after.ok());
    EXPECT_EQ(list_after.value().records.size(), 2u);

    // get_generator_settings 应返回 2
    auto settings = send_empty<protocol::GetGeneratorSettingsResponse>(
        protocol::CommandId::GetGeneratorSettings);
    ASSERT_TRUE(settings.ok());
    EXPECT_EQ(settings.value().history_limit, 2);
}

/// set_generator_limit = 0 表示无限制，不应再删除已有记录。
TEST_F(E2EFlowTest, SetGeneratorLimitZeroMeansUnlimited) {
    enable_program_password();

    // 先生成 3 条
    for (int i = 0; i < 3; ++i) {
        protocol::GeneratePasswordRequest req;
        req.options.length = 8;
        send<protocol::GeneratePasswordRequest, protocol::GeneratePasswordResponse>(
            protocol::CommandId::GeneratePassword, req);
    }
    // 设置上限为 5（不会触发清理）
    protocol::SetGeneratorLimitRequest set_req1;
    set_req1.limit = 5;
    send<protocol::SetGeneratorLimitRequest, protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, set_req1);

    // 再设置回 0（无限制），不应删除现有记录
    protocol::SetGeneratorLimitRequest set_req2;
    set_req2.limit = 0;
    auto r2 = send<protocol::SetGeneratorLimitRequest, protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, set_req2);
    ASSERT_TRUE(r2.ok() && r2.value().success);

    auto list = send_empty<protocol::ListGeneratedRecordsResponse>(
        protocol::CommandId::ListGeneratedRecords);
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(list.value().records.size(), 3u);  // 0 = 无限制 → 保留全部
}

/// set_generator_limit 负数应被拒绝：service 端不会持久化负数。
///
/// 注意：SetGeneratorLimitResponse 是单 bool 字段，反序列化层无法可靠区分
/// ErrorResponse（u32 + string）与合法的 success 响应，因此这里不直接断言
/// resp.error().code，而是通过查询 settings 验证负数未被持久化。
TEST_F(E2EFlowTest, SetGeneratorLimitNegativeIsRejected) {
    enable_program_password();

    // 先设置一个合法的初始值
    protocol::SetGeneratorLimitRequest init_req;
    init_req.limit = 10;
    send<protocol::SetGeneratorLimitRequest, protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, init_req);

    // 尝试设置负数
    protocol::SetGeneratorLimitRequest bad_req;
    bad_req.limit = -1;
    send<protocol::SetGeneratorLimitRequest, protocol::SetGeneratorLimitResponse>(
        protocol::CommandId::SetGeneratorLimit, bad_req);

    // settings 不应被负数覆盖，应仍为 10
    auto settings = send_empty<protocol::GetGeneratorSettingsResponse>(
        protocol::CommandId::GetGeneratorSettings);
    ASSERT_TRUE(settings.ok());
    EXPECT_EQ(settings.value().history_limit, 10);
}

// =============================================================================
// 10. Ping
// =============================================================================

/// ping 应返回合理的 server_timestamp（接近当前 Unix 时间戳）。
TEST_F(E2EFlowTest, PingReturnsRecentTimestamp) {
    auto resp = send_empty<protocol::PingResponse>(protocol::CommandId::Ping);
    ASSERT_TRUE(resp.ok()) << resp.error().what();
    EXPECT_GT(resp.value().server_timestamp, 0u);

    // 校验时间戳接近当前时间（允许 ±60 秒误差，覆盖时钟漂移与测试延迟）
    using namespace std::chrono;
    const uint64_t now = static_cast<uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
    const int64_t diff = static_cast<int64_t>(resp.value().server_timestamp) -
                         static_cast<int64_t>(now);
    const int64_t abs_diff = diff < 0 ? -diff : diff;
    EXPECT_LE(abs_diff, 60)
        << "server_timestamp=" << resp.value().server_timestamp
        << " 距 now=" << now << " 偏差 " << diff << " 秒";
}

/// ping 在未解锁状态下也应可调用（不属于敏感操作）。
TEST_F(E2EFlowTest, PingWorksEvenWhenLocked) {
    enable_program_password();
    lock();

    auto resp = send_empty<protocol::PingResponse>(protocol::CommandId::Ping);
    ASSERT_TRUE(resp.ok()) << resp.error().what();
    EXPECT_GT(resp.value().server_timestamp, 0u);
}

// =============================================================================
// 11. 完整用户旅程：明文 CRUD → 启用密码 → 锁定/解锁 → 修改密码 → 禁用
// =============================================================================

/// 端到端完整旅程：覆盖明文 CRUD、启用程序密码、锁定/解锁、修改密码、禁用等核心步骤。
TEST_F(E2EFlowTest, FullUserJourneyPlaintextToEncryptedAndBack) {
    // 1. 明文模式下添加 2 条条目
    std::vector<int64_t> ids;
    for (int i = 0; i < 2; ++i) {
        protocol::AddEntryRequest req;
        req.entry.title = "site" + std::to_string(i) + ".com";
        req.entry.account = "user" + std::to_string(i);
        req.entry.website = "site" + std::to_string(i) + ".com";
        req.entry.username = "user" + std::to_string(i);
        req.entry.password = "pwd-" + std::to_string(i);
        req.entry.note = "note " + std::to_string(i);
        auto r = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
            protocol::CommandId::AddEntry, req);
        ASSERT_TRUE(r.ok()) << r.error().what();
        ASSERT_GT(r.value().entry.id, 0);
        ids.push_back(r.value().entry.id);
    }

    // 2. 启用程序密码（应重新加密已有条目）
    {
        protocol::EnableProgramPasswordRequest req;
        req.password = kTestProgramPassword;
        auto r = send<protocol::EnableProgramPasswordRequest,
                      protocol::EnableProgramPasswordResponse>(
            protocol::CommandId::EnableProgramPassword, req);
        ASSERT_TRUE(r.ok() && r.value().success);
    }

    // 3. 验证条目仍可正常读取（password 恢复原文）
    {
        protocol::GetEntryRequest get_req;
        get_req.id = ids[0];
        auto r = send<protocol::GetEntryRequest, protocol::GetEntryResponse>(
            protocol::CommandId::GetEntry, get_req);
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r.value().entry.password, "pwd-0");
    }

    // 4. 锁定 → 解锁（用原密码）
    lock();
    {
        auto r = unlock(kTestProgramPassword);
        ASSERT_TRUE(r.ok() && r.value().success);
    }

    // 5. 修改程序密码
    {
        protocol::ChangeProgramPasswordRequest req;
        req.old_password = kTestProgramPassword;
        req.new_password = "NewJourneyPwd789!";
        auto r = send<protocol::ChangeProgramPasswordRequest,
                      protocol::ChangeProgramPasswordResponse>(
            protocol::CommandId::ChangeProgramPassword, req);
        ASSERT_TRUE(r.ok() && r.value().success);
    }

    // 6. 锁定 → 用新密码解锁
    lock();
    {
        auto r = unlock("NewJourneyPwd789!");
        ASSERT_TRUE(r.ok() && r.value().success);
    }

    // 7. 添加第 3 条条目
    {
        protocol::AddEntryRequest req;
        req.entry.title = "site2.com";
        req.entry.account = "user2";
        req.entry.website = "site2.com";
        req.entry.username = "user2";
        req.entry.password = "pwd-2";
        auto r = send<protocol::AddEntryRequest, protocol::AddEntryResponse>(
            protocol::CommandId::AddEntry, req);
        ASSERT_TRUE(r.ok());
        ids.push_back(r.value().entry.id);
    }

    // 8. 禁用程序密码（用新密码验证）
    {
        protocol::DisableProgramPasswordRequest req;
        req.password = "NewJourneyPwd789!";
        auto r = send<protocol::DisableProgramPasswordRequest,
                      protocol::DisableProgramPasswordResponse>(
            protocol::CommandId::DisableProgramPassword, req);
        ASSERT_TRUE(r.ok() && r.value().success);
    }

    // 9. 验证已回到明文模式，所有条目可正常读取
    {
        auto status = get_vault_status();
        ASSERT_TRUE(status.ok());
        EXPECT_FALSE(status.value().password_enabled);
    }
    {
        auto r = send_empty<protocol::ListEntriesResponse>(
            protocol::CommandId::ListEntries);
        ASSERT_TRUE(r.ok());
        EXPECT_EQ(r.value().entries.size(), 3u);
        // 验证 password 字段恢复原文
        bool found0 = false, found2 = false;
        for (const auto& e : r.value().entries) {
            if (e.id == ids[0]) {
                EXPECT_EQ(e.password, "pwd-0");
                found0 = true;
            }
            if (e.id == ids[2]) {
                EXPECT_EQ(e.password, "pwd-2");
                found2 = true;
            }
        }
        EXPECT_TRUE(found0);
        EXPECT_TRUE(found2);
    }

    // 10. ping 始终可用
    {
        auto r = send_empty<protocol::PingResponse>(protocol::CommandId::Ping);
        ASSERT_TRUE(r.ok());
        EXPECT_GT(r.value().server_timestamp, 0u);
    }
}

}  // namespace yuli::vault::test

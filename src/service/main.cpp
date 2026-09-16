// coding: utf-8
// =============================================================================
// main.cpp
//
// PwdVault 服务进程入口。
//
// 职责：
//   1. 解析命令行参数：--foreground（默认）、--install（占位）、--pipe-name=<name>
//   2. 初始化引擎：加载/创建 %APPDATA%\PwdVault\ 目录与 vault.db、vault.meta
//   3. 构造 ServiceCore（注入 crypto / storage / generator 三个引擎）
//   4. 启动 IpcServer，handler lambda 拦截 Shutdown，其余转发给 ServiceCore
//   5. 主线程保活循环：30 秒内无客户端连接则自动退出
//   6. SetConsoleCtrlHandler 处理 Ctrl+C 优雅关闭
//   7. 日志输出到 stdout，格式 [timestamp] [level] message
//
// 子系统：控制台（CONSOLE），非 WIN32，便于调试时 stdout 可见。
// =============================================================================
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "CryptoEngine.h"
#include "PasswordGenerator.h"
#include "StorageEngine.h"
#include "Types.h"

#include "Commands.h"
#include "Messages.h"
#include "Serializer.h"

#include "AppDataDir.h"
#include "IpcServer.h"
#include "ServiceCore.h"

namespace {

constexpr const char* kDefaultPipeName = "\\\\.\\pipe\\YuliVaultService";
constexpr auto kKeepaliveTimeout = std::chrono::seconds(30);

/// 全局退出标志，由 Shutdown 命令或 Ctrl+C 触发。
std::atomic<bool> g_should_exit{false};

/// 全局 IpcServer 指针，供 console ctrl handler 调用 stop()。
/// 生命周期：start() 前设置，stop() 后清空；console handler 仅在此期间被调用。
yuli::vault::service::IpcServer* g_ipc_server = nullptr;

/// 输出一行日志到 stdout，格式 [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message
void log_line(std::string_view level, std::string_view message) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    std::cout << '[' << ts << '.' << std::setfill('0') << std::setw(3) << ms.count()
              << "] [" << level << "] " << message << std::endl;
}

/// Resolve %APPDATA%\YuliVault\, copying a legacy PwdVault directory if needed.
std::filesystem::path get_app_data_dir() {
    std::filesystem::path appdata_root;
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (appdata != nullptr && appdata[0] != L'\0') {
        appdata_root = appdata;
    }
    std::string migrated;
    auto dir = yuli::vault::service::resolve_vault_data_dir(appdata_root, &migrated);
    if (!migrated.empty()) {
        log_line("INFO", migrated);
    }
    return dir;
}

/// 命令行参数。
struct CliArgs {
    bool install = false;
    std::string pipe_name = kDefaultPipeName;
};

/// 解析命令行参数。
CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--foreground") {
            // 默认即前台运行，显式指定时仅做记录
        } else if (a == "--install") {
            args.install = true;
        } else if (a == "--help" || a == "-h") {
            std::cout << "Usage: yuli-vault-service [options]\n"
                      << "  --foreground          Run in foreground (default)\n"
                      << "  --install             Register as Windows service (not yet implemented)\n"
                      << "  --pipe-name=<name>    Override pipe name (default: "
                      << kDefaultPipeName << ")\n";
            std::exit(0);
        } else {
            constexpr std::string_view kPipeNamePrefix = "--pipe-name=";
            if (a.starts_with(kPipeNamePrefix)) {
                args.pipe_name = std::string(a.substr(kPipeNamePrefix.size()));
            }
        }
    }
    return args;
}

/// Console 控制信号处理：Ctrl+C / 关闭窗口等触发优雅退出。
BOOL WINAPI console_ctrl_handler(DWORD ctrl) {
    switch (ctrl) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_should_exit.store(true);
            if (g_ipc_server != nullptr) {
                g_ipc_server->stop();
            }
            return TRUE;
    }
    return FALSE;
}

}  // namespace

int main(int argc, char* argv[]) {
    CliArgs args = parse_args(argc, argv);

    if (args.install) {
        std::cout << "Windows service registration is not yet implemented.\n"
                  << "Run with --foreground (default) to start in console mode." << std::endl;
        return 0;
    }

    // 1. 数据目录与文件路径
    std::filesystem::path app_dir = get_app_data_dir();
    std::error_code mkdir_ec;
    std::filesystem::create_directories(app_dir, mkdir_ec);
    if (mkdir_ec) {
        log_line("ERROR", std::string("Failed to create data directory: ") + mkdir_ec.message());
        return 1;
    }

    auto db_path = app_dir / "vault.db";
    auto meta_path = app_dir / "vault.meta";

    log_line("INFO", "Yuli Vault service starting");
    log_line("INFO", std::string("Data directory: ") + app_dir.string());
    log_line("INFO", std::string("Pipe name: ") + args.pipe_name);

    // 2. 初始化引擎
    //    CryptoEngine 构造时 encryption_key 可为空（ByteSpan{}），仅用于 derive_key
    //    与 generate_key_and_iv；entry 加密用的 encryption_key 由 ServiceCore 在
    //    EnableProgramPassword/Unlock 后通过 set_encryption_key 构造独立的 CryptoEngine 实例。
    auto crypto = std::make_unique<yuli::vault::crypto::CryptoEngine>(yuli::vault::core::ByteSpan{});
    auto storage = std::make_unique<yuli::vault::storage::StorageEngine>(db_path);
    auto generator = std::make_unique<yuli::vault::generator::PasswordGenerator>();

    yuli::vault::service::ServiceCore core(
        std::move(crypto), std::move(storage), std::move(generator), meta_path);

    // 3. 构造 IPC handler
    //    Shutdown 命令特殊处理：设置退出标志并返回 ShutdownResponse。
    //    其余命令转发给 ServiceCore::handle_request。
    auto handler = [&core](yuli::vault::core::ByteSpan payload,
                           const yuli::vault::protocol::MessageHeader& req_header)
        -> yuli::vault::core::ByteVec {
        if (req_header.command == yuli::vault::protocol::CommandId::Shutdown) {
            g_should_exit.store(true);
            log_line("INFO", "Shutdown requested by client");
            return yuli::vault::protocol::serialize(yuli::vault::protocol::ShutdownResponse{});
        }
        return core.handle_request(payload, req_header);
    };

    yuli::vault::service::IpcServer server(args.pipe_name, std::move(handler));
    g_ipc_server = &server;

    // 4. 注册 console ctrl handler
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    // 5. 启动 IPC 服务端
    server.start();
    log_line("INFO", "IPC server started, waiting for clients...");

    // 6. 保活/超时退出循环
    //    主线程每秒检查一次：若无活跃客户端且距上次活动超过 30 秒，则退出。
    //    Shutdown 命令或 Ctrl+C 设置 g_should_exit 后也会跳出循环。
    while (!g_should_exit.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_should_exit.load()) break;

        if (server.active_client_count() == 0) {
            auto elapsed = std::chrono::steady_clock::now() - server.last_activity();
            if (elapsed > kKeepaliveTimeout) {
                log_line("INFO", "No client activity for 30 seconds, exiting");
                break;
            }
        }
    }

    // 7. 优雅关闭
    server.stop();
    g_ipc_server = nullptr;
    log_line("INFO", "Yuli Vault service stopped");
    return 0;
}

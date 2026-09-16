// coding: utf-8
// =============================================================================
// IpcServer.h
//
// PwdVault 命名管道服务端。监听 \\.\pipe\<name>，多客户端并发。
//
// 并发模型：
//   - 监听线程使用 OVERLAPPED ConnectNamedPipe，等待连接事件或停止事件。
//   - 每个客户端连接由独立工作线程处理，使用 OVERLAPPED ReadFile/WriteFile
//     并通过 WaitForMultipleObjects 同时等待 I/O 事件与停止事件。
//   - 读取使用 INFINITE 超时（客户端进程退出时管道立即断裂，无需空闲超时）；
//     写入使用 30 秒超时，防止客户端不读取响应卡死工作线程。
//   - stop() 通过 SetEvent(stop_event_) + CancelIoEx 唤醒所有阻塞的 I/O。
//
// 协议帧：
//   请求 = MessageHeader(16) + payload(payload_size 字节)
//   响应 = MessageHeader(16) + payload(payload_size 字节)
//   响应 header 中 command/request_id 与请求保持一致，便于客户端匹配。
//
// 注意：handler 签名为 (payload, req_header) → response_payload。
//   需要请求头以便 handler 按 CommandId 分发；IpcServer 自己构造响应头。
// =============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/Types.h"
#include "protocol/Messages.h"

namespace yuli::vault::service {

class IpcServer {
public:
    /// 请求处理回调。
    /// \param payload 请求负载（不含 16 字节 header）
    /// \param req_header 请求头（含 CommandId 与 request_id）
    /// \return 响应负载（不含 header；IpcServer 会自动包装响应头）
    using Handler =
        std::function<core::ByteVec(core::ByteSpan payload,
                                    const protocol::MessageHeader& req_header)>;

    /// 构造。
    /// \param pipe_name 管道全名，如 "\\.\pipe\YuliVaultService"
    /// \param handler 请求处理回调
    IpcServer(std::string_view pipe_name, Handler handler);

    /// 析构：自动 stop。
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// 启动监听线程，开始接受客户端连接。
    void start();

    /// 停止监听并断开所有客户端连接，阻塞至所有工作线程退出。
    void stop();

    /// 当前活跃客户端连接数。
    int active_client_count() const { return active_clients_.load(); }

    /// 最近一次客户端活动（连接或断开）时间点。
    /// 用于主线程的保活/超时退出判定。
    std::chrono::steady_clock::time_point last_activity() const;

private:
    /// 监听线程主循环：创建管道实例 → ConnectNamedPipe(overlapped) → 等待。
    void listener_loop();

    /// 客户端工作线程主循环：读 header + payload → 调 handler → 写响应。
    void client_loop(HANDLE client_handle);

    /// 注册/注销活跃客户端 handle，便于 stop() 时取消 I/O。
    void register_client(HANDLE h);
    void unregister_client(HANDLE h);

    /// 更新 last_activity_ 时间戳。
    void touch_activity();

    std::string pipe_name_;
    Handler handler_;

    std::atomic<bool> running_{false};
    std::thread listener_thread_;

    /// 停止事件：manual-reset，stop() 时 SetEvent 唤醒所有等待者。
    HANDLE stop_event_ = nullptr;
    /// 当前监听用管道实例（仅监听线程使用）。
    HANDLE listen_pipe_ = INVALID_HANDLE_VALUE;

    /// 活跃客户端 handle 集合，stop() 时遍历调用 CancelIoEx。
    std::mutex handles_mutex_;
    std::vector<HANDLE> active_client_handles_;

    /// 活跃客户端计数与最近活动时间，供主线程保活判定。
    std::atomic<int> active_clients_{0};
    mutable std::mutex activity_mutex_;
    std::chrono::steady_clock::time_point last_activity_{std::chrono::steady_clock::now()};

    /// 工作线程集合，stop() 时统一 join。
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
};

}  // namespace yuli::vault::service

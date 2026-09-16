// coding: utf-8
// =============================================================================
// MainWindow.h
//
// Yuli Vault 主窗口（新设计）。
//
// 布局：
//   - 左侧 232px 侧边栏：品牌 logo + 4 个导航项（Vault/New Item/Generator/Settings）
//     + 底部服务连接状态 + 锁定按钮
//   - 右侧内容区：
//   - 顶部 56px 顶栏：页面标题 + 条目数 badge + Theme切换 + 锁定按钮
//                          + Minimize / Close 按钮（自定义窗口控制）
//       * 下方 QStackedWidget：4 个功能视图
//
// 窗口装饰：
//   - 无原生 Windows 标题栏（Qt::FramelessWindowHint）
//   - 拖动窗口顶部 56px 区域（顶栏空白处 + 侧边栏品牌区）可移动窗口；
//     保留 Windows Aero Snap（拖到屏幕边缘自动排列）与任务栏右键菜单
//   - 拖动窗口边缘不调整大小（固定尺寸，但允许 Aero Snap 最大化/还原）
//   - 双击顶部标题栏区域不切换最大化
//   - 任务栏按钮：补回 WS_MINIMIZEBOX 样式，支持「点击切换Minimize/还原」
//   - 系统托盘：Close按钮 → Minimize到托盘；托盘单击切换显隐；
//     托盘右键菜单提供 显示/隐藏、锁定、Quit
//
// 启动流程：
//   - 查询 vault 状态：Program password已Enable且Locked → 显示 UnlockView（模态遮罩）
//   - 否则直接进入主界面（明文模式或已Unlock）
//   - IPC 断开 → 弹窗提示并尝试Reconnect
// =============================================================================
#pragma once

#include <QMainWindow>
#include <QString>
#include <QSystemTrayIcon>
#include <QTimer>

class QButtonGroup;
class QFrame;
class QLabel;
class QMenu;
class QPushButton;
class QStackedWidget;

namespace yuli::vault::ui {

class IpcClient;
class UnlockView;
class PasswordBookView;
class InputView;
class GeneratorView;
class SettingsView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(IpcClient* client, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;
    /// 处理 Windows 原生消息，用于无边框窗口的拖动 / Aero Snap / 任务栏右键菜单。
    bool nativeEvent(const QByteArray& eventType, void* message,
                     qintptr* result) override;
    /// 应用级事件过滤器：监听鼠标 / 键盘活动，重置 autolock_timer_ 倒计时。
    /// 安装在 qApp 上（见构造函数末尾），覆盖所有窗口与子控件的事件。
    bool eventFilter(QObject* obj, QEvent* event) override;

private slots:
    void on_nav_clicked(int row);
    void on_pin_clicked();
    void on_lock_clicked();
    void on_ipc_disconnected();
    void on_ipc_error(const QString& message);

    // Unlock流程
    void on_unlock_succeeded();
    void on_unlock_rejected();
    void on_lock_requested();

    // Program password状态变化（由 SettingsView 触发）
    void on_password_state_changed(bool enabled);

    // 视图间联动
    void on_entry_added(int64_t id);
    void on_password_generator_requested();
    void on_password_generated(const QString& password);
    void on_entry_count_changed(int count);
    void on_entry_add_requested();
    void on_generate_requested_from_history();

    // 自定义窗口控制
    void on_minimize_clicked();
    void on_close_clicked();

    // 系统托盘
    void on_tray_activated(QSystemTrayIcon::ActivationReason reason);
    void on_tray_toggle_visible();
    void on_tray_lock();
    void on_tray_quit();

private:
    void build_sidebar(QWidget* parent);
    void build_topbar(QWidget* parent);
    void update_topbar_for_view(int row);
    void update_connection_status();
    void attempt_reconnect();

    /// Settings顶栏标题（带 elide 截断，防止窗口Minimize时挤压右侧按钮）。
    void set_topbar_title(const QString& title);

    /// 构建系统托盘图标与右键菜单。
    void build_tray_icon();

    /// 显示并激活主窗口（从托盘还原 / 从Minimize还原统一入口）。
    void show_and_activate();

    /// 异步启动初始流程：查询 vault 状态，加密且Locked则显示Unlock视图，
    /// 否则Refresh各视图。IPC 失败时仍尝试Refresh。结果通过 QFutureWatcher 在
    /// 主线程接收，避免阻塞 UI 线程。
    void start_initial_flow();

    /// 显示Unlock视图（模态）。
    void show_unlock();

    /// 切换到指定侧边栏视图。
    void switch_to_view(int row);

    /// 重新着色侧边栏导航图标（按选Medium状态 + 当前Theme）。
    void refresh_nav_icons();

    /// 重新着色顶栏图标按钮（按当前Theme）。
    void refresh_topbar_icons();

    /// 应用窗口外缘边框颜色：高对比度开启时按Theme模式设为亮蓝/纯黑，
    /// Close时还原系统默认。Win11 用 DWMWA_BORDER_COLOR，Win10 静默退化。
    void apply_window_border();

    /// 配置Auto-lock：minutes > 0 时启动 / 重置倒计时；minutes == 0 时停止。
    /// 同时持久化到 QSettings（与 SettingsView::on_autolock_changed 双写）。
    /// 锁定状态（unlock_view_ 显示）下不会启动 timer，避免重复触发。
    void setup_autolock(int minutes);

    IpcClient* client_;

    // 侧边栏
    QPushButton* nav_book_btn_ = nullptr;
    QPushButton* nav_input_btn_ = nullptr;
    QPushButton* nav_generator_btn_ = nullptr;
    QPushButton* nav_settings_btn_ = nullptr;
    QButtonGroup* nav_group_ = nullptr;
    QLabel* service_status_dot_ = nullptr;
    QLabel* service_status_label_ = nullptr;
    QPushButton* sidebar_lock_btn_ = nullptr;

    // 顶栏
    QFrame* topbar_frame_ = nullptr;          ///< 顶栏容器（用于 hit-test 判断）
    QLabel* topbar_title_ = nullptr;
    QLabel* topbar_subtitle_ = nullptr;
    QLabel* topbar_count_badge_ = nullptr;
    QPushButton* pin_btn_ = nullptr;
    QPushButton* topbar_lock_btn_ = nullptr;
    QPushButton* minimize_btn_ = nullptr;     ///< 自定义Minimize按钮
    QPushButton* close_btn_ = nullptr;        ///< 自定义Close按钮（hover 红色背景）

    // 内容区
    QStackedWidget* content_stack_ = nullptr;

    // 功能视图
    PasswordBookView* book_view_ = nullptr;
    InputView* input_view_ = nullptr;
    GeneratorView* generator_view_ = nullptr;
    SettingsView* settings_view_ = nullptr;

    // Unlock视图（模态层，按需创建/销毁）
    UnlockView* unlock_view_ = nullptr;

    // 系统托盘
    QSystemTrayIcon* tray_icon_ = nullptr;
    QMenu* tray_menu_ = nullptr;

    // 标记Generator是否由 InputView 请求Open
    bool generator_from_input_ = false;

    // Always on top状态（pin 按钮切换）：true 时窗口浮于其他窗口之上
    bool is_pinned_ = false;

    // 启动流程标记：构造函数末尾 start_initial_flow 期间为 true，
    // 异步回调进入后置 false。用于防止启动期间用户Actions（保留扩展用）。
    bool starting_up_ = true;

    // Auto-lock倒计时 timer：单次触发，超时调用 client_->lock() + show_unlock()
    QTimer* autolock_timer_ = nullptr;
    // 当前Auto-lock分钟数（0 = Never；1/5/15/30 = 对应分钟）
    int autolock_minutes_ = 0;
};

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// MainWindow.cpp
//
// Yuli Vault 主窗口实现。构建 232px 侧边栏 + 56px 顶栏 + 内容区，
// 处理Unlock流程、视图间联动、IPC 断连与Theme切换。
// =============================================================================
#include "MainWindow.h"
#include "AppSettings.h"
#include "ErrorMessages.h"
#include "IconKit.h"
#include "IpcClient.h"
#include "Theme.h"
#include "Version.h"
#include "views/GeneratorView.h"
#include "views/InputView.h"
#include "views/UnlockView.h"
#include "views/PasswordBookView.h"
#include "views/SettingsView.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#if defined(Q_OS_WIN)
#  include <qt_windows.h>
#  include <windowsx.h>
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")
#endif

namespace yuli::vault::ui {

#if defined(Q_OS_WIN)
namespace {
/// Windows 11 DWM 圆角偏好常量（DWMWINDOWATTRACON.corner_preference）。
/// Win10 头文件Medium无此定义，这里手写避免编译失败。
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
#endif
/// DWM_WINDOW_CORNER_PREFERENCE 枚举值。
constexpr int DWMWCP_DEFAULT    = 0;
constexpr int DWMWCP_DONOTROUND = 1;
constexpr int DWMWCP_ROUND      = 2;
constexpr int DWMWCP_ROUNDSMALL = 3;

/// DWMWA_BORDER_COLOR：Settings DWM 窗口边框颜色（Win11 22000+）。
/// Win10 头文件无此定义，手写避免编译失败；运行时调用失败静默忽略。
#ifndef DWMWA_BORDER_COLOR
constexpr DWORD DWMWA_BORDER_COLOR = 34;
#endif
/// DWMWA_COLOR_DEFAULT：还原 DWM 属性为系统默认值。
/// 用于 HC Close时把边框色还原回系统默认（透明/Strong调色）。
/// 用宏而非 constexpr，与 SDK 风格一致，避免 COLORREF 类型识别问题。
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

/// Enable Windows 11 原生窗口圆角（系统默认半径，约 8px）。
/// Win10 上调用会失败但不报错，自动退化为直角。
void enable_win11_rounded_corners(HWND hwnd) {
    if (!hwnd) return;
    const int preference = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                            &preference, sizeof(preference));
}
}  // namespace
#endif

// 侧边栏条目索引
namespace {
constexpr int kSidebarBook = 0;
constexpr int kSidebarInput = 1;
constexpr int kSidebarGenerator = 2;
constexpr int kSidebarSettings = 3;

/// 创建带左侧高亮指示条的导航按钮。
/// 图标由 MainWindow::refresh_nav_icons() 统一着色Settings，此处不设图标。
QPushButton* make_nav_button(const QString& text, const QString& tooltip,
                             QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setCheckable(true);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setText(text);
    btn->setIconSize(QSize(18, 18));
    btn->setToolTip(tooltip);
    btn->setProperty("cssClass", QStringLiteral("navItem"));
    // 样式由 QSS Medium QPushButton[cssClass="navItem"] 提供
    return btn;
}

/// 创建 36x36 图标按钮。
/// 图标由 MainWindow::refresh_topbar_icons() 统一着色Settings，此处不设图标。
QPushButton* make_icon_button(const QString& tooltip, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setIconSize(QSize(18, 18));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tooltip);
    btn->setProperty("cssClass", QStringLiteral("icon"));
    btn->setFixedSize(36, 36);
    return btn;
}

}  // namespace

MainWindow::MainWindow(IpcClient* client, QWidget* parent)
    : QMainWindow(parent), client_(client)
{
    // 自定义无边框窗口：移除 Windows 原生标题栏/边框，但保留任务栏图标、
    // Aero Snap（通过 nativeEvent Medium的 WM_NCHITTEST 实现）和右键菜单。
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setWindowTitle(tr("Yuli Vault"));
    // 固定默认尺寸 1200x800。「固定大小」通过 WM_NCHITTEST 阻止边缘调整实现，
    // 而非 setFixedSize / setMaximumSize —— 后两者会一并Disable Aero Snap 最大化。
    // 这样：① 不能拖边缘调整大小 ② 拖顶栏到屏幕顶部仍可 Aero Snap 最大化。
    setMinimumSize(1200, 800);
    resize(1200, 800);

    // Medium央区：左侧 sidebar + 右侧 (顶栏 + 内容区)
    auto* central = new QWidget(this);
    auto* root_layout = new QHBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // ── 侧边栏 ──
    auto* sidebar_frame = new QFrame(central);
    sidebar_frame->setObjectName(QStringLiteral("sidebarFrame"));
    sidebar_frame->setProperty("cssClass", QStringLiteral("sidebar"));
    sidebar_frame->setFixedWidth(232);
    auto* sidebar_layout = new QVBoxLayout(sidebar_frame);
    sidebar_layout->setContentsMargins(0, 0, 0, 0);
    sidebar_layout->setSpacing(0);
    build_sidebar(sidebar_frame);
    root_layout->addWidget(sidebar_frame);

    // ── 右侧主区 ──
    auto* main_frame = new QFrame(central);
    main_frame->setObjectName(QStringLiteral("mainFrame"));
    auto* main_layout = new QVBoxLayout(main_frame);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    build_topbar(main_frame);

    content_stack_ = new QStackedWidget(main_frame);
    content_stack_->setContentsMargins(0, 0, 0, 0);
    main_layout->addWidget(content_stack_, 1);

    root_layout->addWidget(main_frame, 1);

    setCentralWidget(central);

    // 创建 4 个功能视图并加入 stacked widget
    book_view_ = new PasswordBookView(client_, this);
    input_view_ = new InputView(client_, this);
    generator_view_ = new GeneratorView(client_, this);
    settings_view_ = new SettingsView(client_, this);

    content_stack_->addWidget(book_view_);        // index 0
    content_stack_->addWidget(input_view_);       // index 1
    content_stack_->addWidget(generator_view_);   // index 2
    content_stack_->addWidget(settings_view_);    // index 3

    // 默认选MediumVault
    nav_book_btn_->setChecked(true);
    content_stack_->setCurrentIndex(kSidebarBook);
    update_topbar_for_view(kSidebarBook);

    // 视图间信号联动
    connect(input_view_, &InputView::entry_added,
            this, &MainWindow::on_entry_added);
    connect(input_view_, &InputView::password_generator_requested,
            this, &MainWindow::on_password_generator_requested);
    connect(generator_view_, &GeneratorView::password_generated,
            this, &MainWindow::on_password_generated);
    connect(settings_view_, &SettingsView::lock_requested,
            this, &MainWindow::on_lock_requested);
    connect(settings_view_, &SettingsView::password_state_changed,
            this, &MainWindow::on_password_state_changed);
    connect(book_view_, &PasswordBookView::entry_updated,
            this, [this](int64_t) { /* 预留 */ });
    connect(book_view_, &PasswordBookView::entry_count_changed,
            this, &MainWindow::on_entry_count_changed);
    connect(book_view_, &PasswordBookView::entry_add_requested,
            this, &MainWindow::on_entry_add_requested);
    connect(settings_view_, &SettingsView::generate_requested,
            this, &MainWindow::on_generate_requested_from_history);

    if (client_) {
        connect(client_, &IpcClient::disconnected,
                this, &MainWindow::on_ipc_disconnected);
        connect(client_, &IpcClient::error_occurred,
                this, &MainWindow::on_ipc_error);
    }

    // ── 全局快捷键 ──
    // QShortcut 作为 MainWindow 子对象，存活于主窗口生命周期。
    // 默认 Qt::WindowShortcut 上下文，仅在主窗口激活时生效，避免与对话框
    // （如 EditEntryDialog / GeneratorHistoryDialog）的输入冲突。
    // Ctrl+N → New Item新条目（切换到 InputView 并聚焦首字段）
    auto* sc_n = new QShortcut(QKeySequence(QStringLiteral("Ctrl+N")), this);
    connect(sc_n, &QShortcut::activated, this, [this] { on_entry_add_requested(); });
    // Ctrl+F → Vault + 聚焦搜索框
    auto* sc_f = new QShortcut(QKeySequence(QStringLiteral("Ctrl+F")), this);
    connect(sc_f, &QShortcut::activated, this, [this] {
        if (nav_book_btn_) nav_book_btn_->click();
        if (book_view_) book_view_->focus_search();
    });
    // Ctrl+L → Lock vault
    auto* sc_l = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(sc_l, &QShortcut::activated, this, [this] { on_lock_requested(); });
    // Ctrl+G → Generator
    auto* sc_g = new QShortcut(QKeySequence(QStringLiteral("Ctrl+G")), this);
    connect(sc_g, &QShortcut::activated, this, [this] {
        if (nav_generator_btn_) nav_generator_btn_->click();
    });
    // Ctrl+1..4 → 切换到对应侧边栏视图（click 触发 on_nav_clicked，已含按钮选Medium态切换）
    auto* sc_1 = new QShortcut(QKeySequence(QStringLiteral("Ctrl+1")), this);
    connect(sc_1, &QShortcut::activated, this, [this] {
        if (nav_book_btn_) nav_book_btn_->click();
    });
    auto* sc_2 = new QShortcut(QKeySequence(QStringLiteral("Ctrl+2")), this);
    connect(sc_2, &QShortcut::activated, this, [this] {
        if (nav_input_btn_) nav_input_btn_->click();
    });
    auto* sc_3 = new QShortcut(QKeySequence(QStringLiteral("Ctrl+3")), this);
    connect(sc_3, &QShortcut::activated, this, [this] {
        if (nav_generator_btn_) nav_generator_btn_->click();
    });
    auto* sc_4 = new QShortcut(QKeySequence(QStringLiteral("Ctrl+4")), this);
    connect(sc_4, &QShortcut::activated, this, [this] {
        if (nav_settings_btn_) nav_settings_btn_->click();
    });

#if defined(Q_OS_WIN)
    // Enable Windows 11 原生窗口圆角（Win10 自动退化为直角）。
    // 必须在窗口创建后调用，所以放在构造函数末尾。
    enable_win11_rounded_corners(reinterpret_cast<HWND>(winId()));

    // 为无边框窗口补回 WS_MINIMIZEBOX 样式，使任务栏按钮支持「点击切换
    // Minimize/还原」的 Windows 默认行为。Qt::FramelessWindowHint 会移除所有
    // WS_* 样式（包括 WS_MINIMIZEBOX），导致任务栏按钮点击只能激活窗口
    // 而无法触发系统默认的「再点一次还原/Minimize」切换。
    // 参考：Win32 SDK 文档「Window Styles」与「WM_NCHITTEST」说明。
    HWND hwnd = reinterpret_cast<HWND>(winId());
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_MINIMIZEBOX);

    // 应用窗口外缘边框颜色（HC 开启时按Theme模式着色），并监听Theme/HC 变化
    // 即时Refresh。DWMWA_BORDER_COLOR 仅 Win11 22000+ 支持，Win10 静默退化。
    apply_window_border();
    if (auto* t = Theme::instance()) {
        connect(t, &Theme::theme_changed, this, [this](Theme::Mode) { apply_window_border(); });
        connect(t, &Theme::high_contrast_changed, this, [this](bool) { apply_window_border(); });
    }
#endif

    // 构建系统托盘（Close按钮 → Minimize到托盘；托盘单击切换显隐）
    build_tray_icon();

    update_connection_status();
    // 初始着色所有图标（导航 + 顶栏），需在按钮创建后调用
    refresh_nav_icons();
    refresh_topbar_icons();

    // 启动流程：异步查询 vault 状态，若Program password已Enable且Locked则显示Unlock视图，
    // 否则Refresh各视图。结果通过 QFutureWatcher 在主线程接收，避免阻塞 UI。
    start_initial_flow();

    // ── Auto-lock ──
    // 在所有视图创建完成后安装应用级事件过滤器，监听全局鼠标 / 键盘活动。
    // autolock 的实际启动由 setup_autolock 控制，仅在 minutes > 0 且未锁定时启动。
    qApp->installEventFilter(this);
    // SettingsView 在构造时已从 QSettings 读取持久化值并Settings combo 当前项，
    // 这里直接取 combo 当前值（get_autolock_minutes）作为 timer 初始配置。
    if (settings_view_) {
        connect(settings_view_, &SettingsView::autolock_changed,
                this, &MainWindow::setup_autolock);
        const int minutes = settings_view_->get_autolock_minutes();
        setup_autolock(minutes);
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent* event) {
    // X 按钮 / Alt+F4 / 系统菜单「Close」均走这里。
    // 用户选择：Close → Minimize到托盘，而非Quit。
    // 真正Quit仅通过托盘菜单「Quit」触发（QApplication::quit）。
    if (tray_icon_ && tray_icon_->isVisible()) {
        event->ignore();
        hide();
        return;
    }
    // 托盘不可用时（如系统不支持），走原有清理路径真正Quit
    if (unlock_view_) {
        unlock_view_->deleteLater();
        unlock_view_ = nullptr;
    }
    QMainWindow::closeEvent(event);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                              qintptr* result) {
#if defined(Q_OS_WIN)
    if (eventType == "windows_generic_MSG" && message) {
        MSG* msg = static_cast<MSG*>(message);

        // 处理 WM_NCHITTEST：让窗口顶部 56 DIP 区域（顶栏空白处 + 侧边栏
        // 品牌区）返回 HTCAPTION，系统会把它当作标题栏 —— 这样拖动可移动
        // 窗口，且自动支持 Aero Snap（拖到屏幕边缘排列）和任务栏右键菜单。
        // 边缘不返回 HTLEFT/HTRIGHT/HTTOP/HTBOTTOM 等，因此不能拖边调整大小。
        if (msg->message == WM_NCHITTEST) {
            const LONG x = GET_X_LPARAM(msg->lParam);
            const LONG y = GET_Y_LPARAM(msg->lParam);

            // 使用 Win32 ScreenToClient 获取窗口客户区坐标（物理像素），比 Qt 的
            // mapFromGlobal 更可靠：后者依赖 Qt 内部缓存的窗口几何信息，在窗口
            // 状态切换（最大化/还原/Aero Snap）期间可能返回过时坐标，导致命Medium测试
            // 偶尔返回 HTCLIENT 而非 HTCAPTION，使拖拽无法启动。
            POINT pt{ x, y };
            if (msg->hwnd && ::ScreenToClient(msg->hwnd, &pt)) {
                const qreal dpr = devicePixelRatioF();
                if (dpr > 0) {
                    // 转换为设备无关像素（DIP），与 Qt widget 坐标一致
                    const qreal dip_x = pt.x / dpr;
                    const qreal dip_y = pt.y / dpr;

                    // 标题栏区域 = 窗口顶部 56 DIP（顶栏与侧边栏品牌区等高）
                    if (dip_y >= 0 && dip_y < 56) {
                        if (dip_x < 232) {
                            // 侧边栏品牌区域：无按钮，始终可拖拽
                            *result = HTCAPTION;
                            return true;
                        }
                        // 顶栏区域：检查鼠标下方是否为按钮（QPushButton），
                        // 是则交给 Qt 处理点击；否则视为标题栏。
                        if (topbar_frame_) {
                            const QPoint topbar_local(
                                static_cast<int>(dip_x - 232),
                                static_cast<int>(dip_y));
                            QWidget* hit = topbar_frame_->childAt(topbar_local);
                            bool on_button = false;
                            while (hit && hit != topbar_frame_) {
                                if (qobject_cast<QPushButton*>(hit)) {
                                    on_button = true;
                                    break;
                                }
                                hit = hit->parentWidget();
                            }
                            if (!on_button) {
                                *result = HTCAPTION;
                                return true;
                            }
                        }
                    }
                }
            }
            // 其他区域（含顶栏按钮）：交给 Qt 默认处理（视为 client area）
            return false;
        }

        // 阻止双击顶部标题栏区域自动最大化（用户选择「双击不切换最大化」）
        if (msg->message == WM_NCLBUTTONDBLCLK) {
            if (msg->wParam == HTCAPTION) {
                *result = 0;
                return true;
            }
        }
    }
#else
    Q_UNUSED(eventType); Q_UNUSED(message); Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::on_minimize_clicked() {
    showMinimized();
}

void MainWindow::on_close_clicked() {
    close();  // 走 closeEvent，由其决定「隐藏到托盘」或「真正Quit」
}

// ---------------------------------------------------------------------------
// 系统托盘
// ---------------------------------------------------------------------------

void MainWindow::build_tray_icon() {
    // 仅当系统支持托盘时创建
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    tray_icon_ = new QSystemTrayIcon(this);
    tray_icon_->setIcon(QIcon(QStringLiteral(":/logo.png")));
    tray_icon_->setToolTip(tr("Yuli Vault"));

    // 右键菜单
    tray_menu_ = new QMenu(this);
    tray_menu_->addAction(tr("Show/hide window"),
                          this, &MainWindow::on_tray_toggle_visible);
    tray_menu_->addSeparator();
    tray_menu_->addAction(tr("Lock vault"),
                          this, &MainWindow::on_tray_lock);
    tray_menu_->addSeparator();
    tray_menu_->addAction(tr("Quit"),
                          this, &MainWindow::on_tray_quit);
    tray_icon_->setContextMenu(tray_menu_);

    // 单击切换显隐
    connect(tray_icon_, &QSystemTrayIcon::activated,
            this, &MainWindow::on_tray_activated);

    tray_icon_->show();
}

void MainWindow::show_and_activate() {
    if (isMinimized()) {
        showNormal();
    } else {
        show();
    }
    raise();
    activateWindow();
}

void MainWindow::on_tray_activated(QSystemTrayIcon::ActivationReason reason) {
    // 单击（Trigger）切换显隐；双击 / Context / MiddleClick 等忽略
    // （Context 由系统自动弹出右键菜单）
    if (reason == QSystemTrayIcon::Trigger) {
        on_tray_toggle_visible();
    }
}

void MainWindow::on_tray_toggle_visible() {
    if (isVisible() && !isMinimized()) {
        hide();
    } else {
        show_and_activate();
    }
}

void MainWindow::on_tray_lock() {
    // 锁定前必须显示主窗口，否则Unlock模态对话框无可见父窗口
    show_and_activate();
    on_lock_clicked();
}

void MainWindow::on_tray_quit() {
    // 真正Quit：清理托盘图标后Quit事件循环
    if (tray_icon_) {
        tray_icon_->hide();
    }
    if (unlock_view_) {
        unlock_view_->deleteLater();
        unlock_view_ = nullptr;
    }
    QApplication::quit();
}

// ---------------------------------------------------------------------------
// 子组件构建
// ---------------------------------------------------------------------------

void MainWindow::build_sidebar(QWidget* parent) {
    auto* layout = qobject_cast<QVBoxLayout*>(parent->layout());
    if (!layout) {
        layout = new QVBoxLayout(parent);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
    }

    // 品牌块
    auto* brand_frame = new QFrame(parent);
    brand_frame->setFixedHeight(56);
    auto* brand_layout = new QHBoxLayout(brand_frame);
    // 左 26 / 上 16：logo + name 稍往右下偏移，与顶栏标题对齐更协调
    brand_layout->setContentsMargins(26, 16, 18, 0);
    brand_layout->setSpacing(10);

    auto* brand_icon = new QLabel(brand_frame);
    brand_icon->setPixmap(QIcon(QStringLiteral(":/logo.png"))
                              .pixmap(QSize(28, 28)));
    brand_layout->addWidget(brand_icon);

    auto* brand_label = new QLabel(tr("Yuli Vault"), brand_frame);
    brand_label->setProperty("cssClass", QStringLiteral("brand"));
    brand_layout->addWidget(brand_label);
    brand_layout->addStretch(1);

    layout->addWidget(brand_frame);

    // 导航
    auto* nav_frame = new QFrame(parent);
    nav_frame->setProperty("cssClass", QStringLiteral("navContainer"));
    auto* nav_layout = new QVBoxLayout(nav_frame);
    nav_layout->setContentsMargins(12, 12, 12, 12);
    nav_layout->setSpacing(4);

    nav_group_ = new QButtonGroup(this);
    nav_group_->setExclusive(true);

    nav_book_btn_ = make_nav_button(
        tr("Vault"), tr("Vault (Ctrl+1)"), nav_frame);
    nav_input_btn_ = make_nav_button(
        tr("New Item"), tr("New Item (Ctrl+2)"), nav_frame);
    nav_generator_btn_ = make_nav_button(
        tr("Generator"), tr("Generator (Ctrl+3)"), nav_frame);
    nav_settings_btn_ = make_nav_button(
        tr("Settings"), tr("Settings (Ctrl+4)"), nav_frame);

    nav_group_->addButton(nav_book_btn_, kSidebarBook);
    nav_group_->addButton(nav_input_btn_, kSidebarInput);
    nav_group_->addButton(nav_generator_btn_, kSidebarGenerator);
    nav_group_->addButton(nav_settings_btn_, kSidebarSettings);

    nav_layout->addWidget(nav_book_btn_);
    nav_layout->addWidget(nav_input_btn_);
    nav_layout->addWidget(nav_generator_btn_);
    nav_layout->addWidget(nav_settings_btn_);
    nav_layout->addStretch(1);

    layout->addWidget(nav_frame, 1);

    // 底部状态 + 锁定
    auto* bottom_frame = new QFrame(parent);
    auto* bottom_layout = new QVBoxLayout(bottom_frame);
    bottom_layout->setContentsMargins(18, 16, 18, 16);
    bottom_layout->setSpacing(12);

    auto* status_row = new QHBoxLayout();
    status_row->setSpacing(8);
    service_status_dot_ = new QLabel(bottom_frame);
    service_status_dot_->setFixedSize(8, 8);
    service_status_dot_->setProperty("cssClass", QStringLiteral("statusDotOk"));
    status_row->addWidget(service_status_dot_);

    service_status_label_ = new QLabel(
        tr("Service connected"), bottom_frame);
    service_status_label_->setProperty("cssClass", QStringLiteral("caption"));
    status_row->addWidget(service_status_label_);
    status_row->addStretch(1);
    bottom_layout->addLayout(status_row);

    sidebar_lock_btn_ = new QPushButton(bottom_frame);
    sidebar_lock_btn_->setIconSize(QSize(16, 16));
    sidebar_lock_btn_->setText(tr("Lock vault"));
    sidebar_lock_btn_->setCursor(Qt::PointingHandCursor);
    sidebar_lock_btn_->setFixedHeight(40);
    sidebar_lock_btn_->setProperty("cssClass", QStringLiteral("outline"));
    bottom_layout->addWidget(sidebar_lock_btn_);

    layout->addWidget(bottom_frame);

    // 信号槽
    connect(nav_group_, &QButtonGroup::idClicked,
            this, &MainWindow::on_nav_clicked);
    connect(sidebar_lock_btn_, &QPushButton::clicked,
            this, &MainWindow::on_lock_clicked);
}

void MainWindow::build_topbar(QWidget* parent) {
    auto* layout = qobject_cast<QVBoxLayout*>(parent->layout());
    if (!layout) {
        layout = new QVBoxLayout(parent);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
    }

    topbar_frame_ = new QFrame(parent);
    topbar_frame_->setObjectName(QStringLiteral("topbarFrame"));
    topbar_frame_->setProperty("cssClass", QStringLiteral("topbar"));
    topbar_frame_->setFixedHeight(56);
    auto* topbar_layout = new QHBoxLayout(topbar_frame_);
    // 右侧留出窗口控制按钮位置（Minimize/Close按钮直接贴右边缘）
    topbar_layout->setContentsMargins(24, 0, 8, 0);
    topbar_layout->setSpacing(12);

    // 左侧：标题 + 副标题
    topbar_title_ = new QLabel(topbar_frame_);
    topbar_title_->setProperty("cssClass", QStringLiteral("title"));
    // 限制最大宽度并Disable换行，配合 set_topbar_title 的 elide 截断，
    // 防止窗口Minimize（1200×800）时标题挤压右侧 4 个按钮。
    topbar_title_->setMaximumWidth(200);
    topbar_title_->setWordWrap(false);
    topbar_layout->addWidget(topbar_title_);

    topbar_subtitle_ = new QLabel(topbar_frame_);
    topbar_subtitle_->setProperty("cssClass", QStringLiteral("muted"));
    topbar_layout->addWidget(topbar_subtitle_);

    // 条目数 badge
    topbar_count_badge_ = new QLabel(topbar_frame_);
    topbar_count_badge_->setProperty("cssClass", QStringLiteral("badge"));
    topbar_count_badge_->setAlignment(Qt::AlignCenter);
    topbar_count_badge_->hide();
    topbar_layout->addWidget(topbar_count_badge_);

    topbar_layout->addStretch(1);

    // 右侧：Always on top + 锁定
    pin_btn_ = make_icon_button(
        tr("Always on top"), topbar_frame_);
    pin_btn_->setCheckable(true);
    topbar_layout->addWidget(pin_btn_);

    topbar_lock_btn_ = make_icon_button(
        tr("Lock vault"), topbar_frame_);
    topbar_layout->addWidget(topbar_lock_btn_);

    // ── 窗口控制按钮（贴右边缘） ──
    // 与功能按钮之间留 4px 间隔
    topbar_layout->addSpacing(4);

    minimize_btn_ = make_icon_button(
        tr("Minimize"), topbar_frame_);
    topbar_layout->addWidget(minimize_btn_);

    close_btn_ = make_icon_button(
        tr("Close"), topbar_frame_);
    // Close按钮 hover 时使用红色背景，更接近 Windows 原生体验
    close_btn_->setProperty("cssClass", QStringLiteral("closeBtn"));
    topbar_layout->addWidget(close_btn_);

    layout->addWidget(topbar_frame_);

    connect(pin_btn_, &QPushButton::clicked,
            this, &MainWindow::on_pin_clicked);
    connect(topbar_lock_btn_, &QPushButton::clicked,
            this, &MainWindow::on_lock_clicked);
    connect(minimize_btn_, &QPushButton::clicked,
            this, &MainWindow::on_minimize_clicked);
    connect(close_btn_, &QPushButton::clicked,
            this, &MainWindow::on_close_clicked);
}

void MainWindow::update_topbar_for_view(int row) {
    switch (row) {
        case kSidebarBook:
            set_topbar_title(tr("Vault"));
            topbar_subtitle_->setText(QString());
            topbar_count_badge_->show();
            break;
        case kSidebarInput:
            set_topbar_title(tr("New Item"));
            topbar_subtitle_->setText(tr("Create a login item"));
            topbar_count_badge_->hide();
            break;
        case kSidebarGenerator:
            set_topbar_title(tr("Generator"));
            topbar_subtitle_->setText(tr("Generate a strong random password"));
            topbar_count_badge_->hide();
            break;
        case kSidebarSettings:
            set_topbar_title(tr("Settings"));
            topbar_subtitle_->setText(tr("Version ") + QStringLiteral(YULI_VAULT_VERSION));
            topbar_count_badge_->hide();
            break;
        default:
            break;
    }
}

void MainWindow::set_topbar_title(const QString& title) {
    if (!topbar_title_) return;
    // 用 QFontMetrics elide 截断标题，防止窗口Minimize时挤压右侧按钮。
    // Medium文标题较短（如"Vault"约 60-80px）通常不会触发 elide，
    // 但保留此逻辑以应对未来更长的标题或更小的窗口尺寸。
    const QFontMetrics fm(topbar_title_->font());
    topbar_title_->setText(fm.elidedText(title, Qt::ElideRight,
                                          topbar_title_->maximumWidth()));
}

// ---------------------------------------------------------------------------
// 状态更新
// ---------------------------------------------------------------------------

void MainWindow::update_connection_status() {
    const bool connected = client_ && client_->is_connected();
    if (connected) {
        service_status_dot_->setProperty("cssClass", QStringLiteral("statusDotOk"));
        service_status_label_->setText(tr("Service connected"));
    } else {
        service_status_dot_->setProperty("cssClass", QStringLiteral("statusDotErr"));
        service_status_label_->setText(tr("Service disconnected"));
    }
    // 切换 dynamic property 后必须 unpolish + polish 才能让 QSS 重新生效
    service_status_dot_->style()->unpolish(service_status_dot_);
    service_status_dot_->style()->polish(service_status_dot_);
}

void MainWindow::attempt_reconnect() {
    if (!client_) return;
    if (client_->connect_to_service()) {
        update_connection_status();
        // 异步查询 vault 状态，根据是否锁定决定后续流程
        // （原同步实现调用 should_show_unlock() → client_->get_vault_status()）
        auto* watcher = new QFutureWatcher<core::Result<protocol::GetVaultStatusResponse>>(this);
        connect(watcher, &QFutureWatcher<core::Result<protocol::GetVaultStatusResponse>>::finished,
                this, [this, watcher]() {
            auto result = watcher->result();
            if (result.ok() && result.value().password_enabled && result.value().is_locked) {
                QMessageBox::information(this, tr("Reconnect"),
                    tr("Reconnected, but the vault is locked. Enter the program password."));
                show_unlock();
            } else {
                if (book_view_) book_view_->refresh();
                if (settings_view_) settings_view_->refresh_status();
                QMessageBox::information(this, tr("Reconnect"),
                    tr("Reconnected to the service."));
            }
            watcher->deleteLater();
        });
        watcher->setFuture(client_->get_vault_status_async());
    } else {
        update_connection_status();
        QMessageBox::warning(this, tr("Reconnect failed"),
            tr("Could not connect to the service. Retry later or start it manually."));
    }
}

void MainWindow::refresh_nav_icons() {
    // 按选Medium状态着色：选Medium项用 Active（主文字色），其余用 Normal（muted）。
    struct Nav { QPushButton* btn; const char* svg; };
    const Nav navs[] = {
        { nav_book_btn_,      ":/icons/key-round.svg" },
        { nav_input_btn_,     ":/icons/file-plus-2.svg" },
        { nav_generator_btn_, ":/icons/wand-2.svg" },
        { nav_settings_btn_,  ":/icons/settings.svg" },
    };
    for (const auto& n : navs) {
        if (!n.btn) continue;
        const IconRole role = n.btn->isChecked() ? IconRole::Active : IconRole::Normal;
        n.btn->setIcon(tinted_icon(QString::fromLatin1(n.svg), role));
    }
}

void MainWindow::refresh_topbar_icons() {
    // Always on top按钮：未置顶=垂直图钉（Normal 灰），已置顶=倾斜填色图钉（Active 蓝）
    if (pin_btn_) {
        const QString pin_svg = is_pinned_
            ? QStringLiteral(":/icons/pin-off.svg")
            : QStringLiteral(":/icons/pin.svg");
        const IconRole role = is_pinned_ ? IconRole::Active : IconRole::Normal;
        pin_btn_->setIcon(tinted_icon(pin_svg, role));
    }
    if (topbar_lock_btn_)
        topbar_lock_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/lock.svg"), IconRole::Normal));
    if (minimize_btn_)
        minimize_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/minus.svg"), IconRole::Normal));
    // close 按钮：常态用 Normal 色（hover 时 QSS 切红背景，Medium灰图标在红底上仍可读）
    if (close_btn_)
        close_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/x.svg"), IconRole::Normal));
    if (sidebar_lock_btn_)
        sidebar_lock_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/lock.svg"), IconRole::Normal));
}

void MainWindow::apply_window_border() {
#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;

    // HC 开启：沿用控件边框色 —— Dark=亮蓝 #3b6bff，Light=纯黑 #000000。
    // HC Close：还原系统默认（DWMWA_COLOR_DEFAULT）。
    // COLORREF 格式为 0x00BBGGRR，用 RGB() 宏避免端序手算错误。
    COLORREF color = DWMWA_COLOR_DEFAULT;
    if (Theme::is_high_contrast()) {
        color = Theme::is_dark()
            ? RGB(0x3b, 0x6b, 0xff)   // 亮蓝 #3b6bff
            : RGB(0x00, 0x00, 0x00);  // 纯黑 #000000
    }
    // DWMWA_BORDER_COLOR 仅 Win11 22000+ 支持；Win10/早期 Win11 调用失败，
    // DwmSetWindowAttribute 返回失败 HRESULT，静默忽略，无副作用。
    ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR,
                            &color, sizeof(color));
#endif
}

// ---------------------------------------------------------------------------
// Unlock流程
// ---------------------------------------------------------------------------

void MainWindow::start_initial_flow() {
    if (!client_) {
        // 无 client，直接尝试Refresh
        if (book_view_) book_view_->refresh();
        if (settings_view_) settings_view_->refresh_status();
        starting_up_ = false;
        return;
    }

    auto* watcher = new QFutureWatcher<core::Result<protocol::GetVaultStatusResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::GetVaultStatusResponse>>::finished,
            this, [this, watcher]() {
        auto result = watcher->result();
        starting_up_ = false;
        if (result.ok()) {
            const auto& status = result.value();
            if (status.password_enabled && status.is_locked) {
                show_unlock();
            } else {
                if (book_view_) book_view_->refresh();
                if (settings_view_) settings_view_->refresh_status();
            }
        } else {
            // IPC 失败：记录日志，但仍尝试Refresh（明文模式下也能正常展示）
            qWarning() << "[Yuli Vault][MainWindow] get_vault_status failed:"
                       << QString::fromStdString(result.error().what());
            if (book_view_) book_view_->refresh();
            if (settings_view_) settings_view_->refresh_status();
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->get_vault_status_async());
}

void MainWindow::show_unlock() {
    if (unlock_view_) {
        unlock_view_->hide();
        unlock_view_->deleteLater();
        unlock_view_ = nullptr;
    }

    // 锁定状态下暂停 autolock_timer_，避免在 UnlockView 模态期间反复触发
    // （eventFilter 仍会收到事件并尝试 start，但 unlock_view_ != nullptr 时
    // 不会重置，见 eventFilter 实现）。
    if (autolock_timer_) autolock_timer_->stop();

    unlock_view_ = new UnlockView(client_, this);
    connect(unlock_view_, &UnlockView::unlock_succeeded,
            this, &MainWindow::on_unlock_succeeded);
    connect(unlock_view_, &UnlockView::rejected,
            this, &MainWindow::on_unlock_rejected);
    unlock_view_->show();
}

void MainWindow::on_unlock_succeeded() {
    if (unlock_view_) {
        unlock_view_->hide();
        unlock_view_->deleteLater();
        unlock_view_ = nullptr;
    }
    if (book_view_) book_view_->refresh();
    if (settings_view_) settings_view_->refresh_status();
    show();
    raise();
    activateWindow();
    update_connection_status();

    // UnlockSuccess后恢复 autolock_timer_：若用户配置了 minutes > 0 则重新启动。
    // 与 show_unlock() Medium stop() 配对，保证锁定 / Unlock周期内 timer 状态正确。
    if (autolock_timer_ && autolock_minutes_ > 0) {
        autolock_timer_->start(autolock_minutes_ * 60 * 1000);
    }
}

void MainWindow::on_unlock_rejected() {
    QApplication::quit();
}

void MainWindow::on_lock_requested() {
    show_unlock();
}

void MainWindow::on_password_state_changed(bool enabled) {
    // 明文模式（未EnableProgram password）下隐藏所有「锁定」按钮：
    // 无加密即无可锁，按钮可见会让用户误Actions后报错。
    if (sidebar_lock_btn_) sidebar_lock_btn_->setVisible(enabled);
    if (topbar_lock_btn_) topbar_lock_btn_->setVisible(enabled);
    if (settings_view_) settings_view_->refresh_status();
}

// ---------------------------------------------------------------------------
// 视图间联动
// ---------------------------------------------------------------------------

void MainWindow::on_entry_added(int64_t id) {
    (void)id;
    if (book_view_) book_view_->refresh();
    switch_to_view(kSidebarBook);
}

void MainWindow::on_password_generator_requested() {
    generator_from_input_ = (content_stack_->currentWidget() == input_view_);
    switch_to_view(kSidebarGenerator);
}

void MainWindow::on_password_generated(const QString& password) {
    // 仅在 InputView 请求Generate时才回填，避免独立使用Generator时覆盖 InputView 已填内容
    if (generator_from_input_) {
        generator_from_input_ = false;
        if (input_view_) input_view_->set_password(password);
        switch_to_view(kSidebarInput);
    }
}

void MainWindow::on_entry_count_changed(int count) {
    if (topbar_count_badge_) {
        topbar_count_badge_->setText(tr("%1 items").arg(count));
    }
}

void MainWindow::on_entry_add_requested() {
    // PasswordBookView 空状态「New item」→ 切到 InputView 并聚焦首字段
    content_stack_->setCurrentIndex(kSidebarInput);
    if (nav_input_btn_) nav_input_btn_->setChecked(true);
    update_topbar_for_view(kSidebarInput);
    refresh_nav_icons();
    if (input_view_) input_view_->focus_first_field();
}

void MainWindow::on_generate_requested_from_history() {
    // GeneratorHistoryDialog 空状态「Generate a password」→ 切到 GeneratorView
    content_stack_->setCurrentIndex(kSidebarGenerator);
    if (nav_generator_btn_) nav_generator_btn_->setChecked(true);
    update_topbar_for_view(kSidebarGenerator);
    refresh_nav_icons();
}

void MainWindow::switch_to_view(int row) {
    auto* btn = nav_group_->button(row);
    if (btn) btn->setChecked(true);
    content_stack_->setCurrentIndex(row);
    update_topbar_for_view(row);
    if (row == kSidebarBook && book_view_) {
        book_view_->refresh();
        // 切到Vault后将焦点置于列表，方便键盘上下方向键浏览条目。
        book_view_->focus_list();
    }
    if (row == kSidebarSettings && settings_view_) {
        settings_view_->refresh_status();
    }
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void MainWindow::on_nav_clicked(int row) {
    content_stack_->setCurrentIndex(row);
    update_topbar_for_view(row);
    refresh_nav_icons();
    if (row == kSidebarBook && book_view_) {
        book_view_->refresh();
        // 切到Vault后将焦点置于列表，方便键盘上下方向键浏览条目。
        book_view_->focus_list();
    }
    if (row == kSidebarSettings && settings_view_) {
        settings_view_->refresh_status();
    }
}

void MainWindow::on_pin_clicked() {
    is_pinned_ = !is_pinned_;
#if defined(Q_OS_WIN)
    // Windows：用 SetWindowPos 直接Change HWND_EX_TOPMOST，避免 setWindowFlag
    // 重建窗口导致的可见闪烁。flags 仅改 Z 序与 topmost，不动尺寸/位置。
    const HWND insert_after = is_pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST;
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    ::SetWindowPos(reinterpret_cast<HWND>(winId()), insert_after,
                   0, 0, 0, 0, flags);
#else
    // 非 Windows 回退：setWindowFlag 会重建窗口，可能闪一下
    setWindowFlag(Qt::WindowStaysOnTopHint, is_pinned_);
    show();
#endif
    // 同步图标（按钮 checked 态由 QAbstractButton::clicked 自动切换，无需手动 setChecked）
    refresh_topbar_icons();
}

void MainWindow::on_lock_clicked() {
    if (!client_) {
        // 无 client：仍走 UI 锁定流程（切到 UnlockView）
        on_lock_requested();
        return;
    }

    auto* watcher = new QFutureWatcher<core::Result<protocol::LockResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::LockResponse>>::finished,
            this, [this, watcher]() {
        auto result = watcher->result();
        if (result.ok()) {
            // 锁定Success：切到 UnlockView（与原同步路径一致）
            on_lock_requested();
        } else {
            // Lock failed：保留原有错误文案逻辑
            const QString msg = friendly_message(result.error());
            QMessageBox::warning(this, tr("Lock failed"),
                msg.isEmpty() ? tr("Could not lock the vault.")
                              : tr("Lock failed: %1").arg(msg));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->lock_async());
}

void MainWindow::on_ipc_disconnected() {
    update_connection_status();
    // 断连后停止 autolock_timer_，避免在 service 不可用时反复尝试 client_->lock()
    // 失败。ReconnectSuccess后由 attempt_reconnect / on_unlock_succeeded 视情况重启。
    if (autolock_timer_) autolock_timer_->stop();
    const auto answer = QMessageBox::warning(
        this,
        tr("Disconnected"),
        tr("The service connection was lost. Reconnect?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        attempt_reconnect();
    }
}

void MainWindow::on_ipc_error(const QString& message) {
    // 新设计无状态栏，错误用 tooltip 或弹窗（这里静默处理，避免打扰）
    (void)message;
}

// ---------------------------------------------------------------------------
// Auto-lock
// ---------------------------------------------------------------------------

void MainWindow::setup_autolock(int minutes) {
    autolock_minutes_ = minutes;

    // 懒创建 timer：仅首次调用时 new，后续复用
    if (!autolock_timer_) {
        autolock_timer_ = new QTimer(this);
        autolock_timer_->setSingleShot(true);
        connect(autolock_timer_, &QTimer::timeout, this, [this] {
            // 锁定状态下（unlock_view_ 已显示）不应触发：可能由边界时序
            // （如手动锁定后 timer 仍未 stop）引起，直接跳过。
            if (unlock_view_) return;
            if (client_ && client_->is_connected()) {
                // 异步锁定：让 service Clear内存Medium的 KEK。
                // 不等回调直接切 UI（on_lock_requested），service 端会很快处理；
                // 失败仅记录日志，避免在Auto-lock路径上弹窗打扰用户。
                auto* watcher = new QFutureWatcher<core::Result<protocol::LockResponse>>(this);
                connect(watcher, &QFutureWatcher<core::Result<protocol::LockResponse>>::finished,
                        this, [this, watcher]() {
                    auto result = watcher->result();
                    if (!result.ok()) {
                        qWarning() << "[Yuli Vault][Autolock] lock_async failed:"
                                   << QString::fromStdString(result.error().what());
                    }
                    watcher->deleteLater();
                });
                watcher->setFuture(client_->lock_async());
            }
            // 触发锁定 UI 流程（与 SettingsView「Lock now」按钮相同路径）：
            // 显示 UnlockView 模态层。show_unlock 内部会 stop autolock_timer_。
            on_lock_requested();
        });
    }

    // 锁定状态下不启动 timer：unlock_view_ 显示时用户处于Unlock模态，
    // 没有需要计时的活动；UnlockSuccess后 on_unlock_succeeded 会按需重启。
    if (minutes > 0 && !unlock_view_) {
        autolock_timer_->start(minutes * 60 * 1000);
    } else {
        autolock_timer_->stop();
    }

    // 持久化：与 SettingsView::on_autolock_changed 双写，键值相同无害。
    // 此处写入保证 setup_autolock 被外部调用（如未来命令行参数）时也能持久化。
    settings_set(QStringLiteral("autolock_minutes"), minutes);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event) {
    // 仅在 autolock Enable且未处于锁定状态时重置倒计时：
    // - autolock_minutes_ > 0：用户选择了某个分钟数
    // - autolock_timer_ 非空：setup_autolock 至少调用过一次
    // - !unlock_view_：当前不在Unlock模态（避免在 UnlockView Medium输入Password时
    //   重置 timer 干扰 stop 状态）
    if (autolock_timer_ && autolock_minutes_ > 0 && !unlock_view_) {
        switch (event->type()) {
            case QEvent::MouseMove:
            case QEvent::KeyPress:
            case QEvent::Wheel:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
                // 任意用户活动重置倒计时为完整 minutes 周期
                autolock_timer_->start(autolock_minutes_ * 60 * 1000);
                break;
            default:
                break;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

}  // namespace yuli::vault::ui

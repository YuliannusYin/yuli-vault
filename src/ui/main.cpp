// coding: utf-8
// =============================================================================
// main.cpp
//
// Yuli Vault UI 进程入口。负责：
//   - 创建 QApplication 与事件循环
//   - 创建 IpcClient 并连接 service
//   - 连接失败时拉起 service 进程（QProcess::startDetached）
//   - 重试连接 3 次（由 connect_to_service 内部完成）
//   - 创建 MainWindow 显示
// =============================================================================
#include "IpcClient.h"
#include "MainWindow.h"
#include "AppSettings.h"
#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTranslator>

namespace {

/// service 可执行文件名。
constexpr const char* kServiceExeName = "yuli-vault-service.exe";

/// 返回与 UI 同目录的 service 可执行文件路径。
QString service_executable_path() {
    const QString app_dir = QCoreApplication::applicationDirPath();
    return QDir(app_dir).absoluteFilePath(QString::fromLatin1(kServiceExeName));
}

/// 尝试拉起 service 进程（与 UI 同目录的 yuli-vault-service.exe）。
/// \return 是否Success startDetached
bool launch_service() {
    const QString exe = service_executable_path();
    if (!QFileInfo::exists(exe)) {
        return false;
    }
    return QProcess::startDetached(exe, QStringList{});
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("YuliVault"));
    QApplication::setApplicationDisplayName(QStringLiteral("Yuli Vault"));
    QApplication::setOrganizationName(QStringLiteral("YuliVault"));
    // 全局窗口图标：影响任务栏、标题栏、Alt+Tab 预览。
    // QIcon 会按需自动缩放，保留原始宽高比，四周透明填充。
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/logo.png")));

    // 全局字体：用 QFont::setFamilies Enable字符级回退（Qt 6+）。
    //
    // 注意：Qt 的 QSS font-family 列表对「缺失字符」的回退不可靠——
    // QSS 选Medium列表里第一个能加载的字体（如 Segoe UI）后就停止，对于该字体
    // 不含的字符（Medium文），不会自动回退到列表里下一个Medium文字体，而是走系统
    // 默认 fallback。Windows 上这个 fallback 通常是 SimSun 点阵字，导致
    // Medium文渲染发虚、字重不均。
    //
    // QFont::setFamilies 是真正的字符级回退链，与浏览器 CSS 行为一致：
    // 对每个字符按列表顺序找第一个能渲染的字体。英文走 Segoe UI（锐利），
    // Medium文走 Microsoft YaHei UI（清晰抗锯齿）。
    //
    // QSS Medium的 font-size: 13px 会覆盖此处的字号；这里只设默认 9pt 作为
    // QSS 未覆盖控件（如原生对话框）的兜底。
    QFont app_font;
    app_font.setFamilies({
        QStringLiteral("Segoe UI Variable"),   // Win11 首选
        QStringLiteral("Segoe UI"),            // Win10 首选
        QStringLiteral("Microsoft YaHei UI"),  // Medium文回退（UI 变体，更紧凑）
        QStringLiteral("Microsoft YaHei"),     // Medium文回退（无 UI 变体时的兜底）
    });
    app_font.setPointSize(9);
    QApplication::setFont(app_font);

    // 加载Medium文翻译（Task 37 会用 tr() 包裹字符串，由 lrelease Generate .qm）。
    // .qm 通过 CMake qt_add_translation Generate并嵌入资源 :/translations/。
    // 加载失败不影响启动（仅缺少翻译，UI 退回源码字面量）。
    QTranslator translator;
    yuli::vault::ui::apply_ui_translator(&app, &translator);

    // 加载持久化Theme（dark / light qss），必须在 MainWindow 创建前调用
    yuli::vault::ui::Theme::load_initial_theme(&app);

    yuli::vault::ui::IpcClient client;

    // 1) 先尝试连接已在运行的 service。
    //    connect_to_service 内部会重试 3 次（间隔 500ms）。
    if (!client.connect_to_service()) {
        // 2) 拉起 service 进程
        if (!launch_service()) {
            QMessageBox::critical(nullptr,
                QCoreApplication::translate("main", "Startup failed"),
                QCoreApplication::translate(
                    "main",
                    "Could not connect to the service and yuli-vault-service.exe was not found.\nPlace the service next to the UI executable."));
        } else {
            // 3) 再次重试连接（connect_to_service 内部 3 次重试）
            client.connect_to_service();
        }
    }

    yuli::vault::ui::MainWindow window(&client);
    window.show();

    return app.exec();
}

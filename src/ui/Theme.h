// coding: utf-8
// =============================================================================
// Theme.h
//
// Yuli Vault Theme管理器。负责加载 dark.qss / light.qss，运行时切换并持久化
// 用户偏好到 QSettings（组织名 Yuli Vault，应用名 Yuli Vault）。
//
// 用法：
//   Theme::load_initial_theme(qApp);  // main.cpp 启动时调用
//   Theme::set_mode(Theme::Mode::Light);  // 切换Theme，自动持久化
//   Theme::toggle();  // 在 dark/light 之间切换
// =============================================================================
#pragma once

#include <QObject>

class QApplication;

namespace yuli::vault::ui {

class Theme : public QObject {
    Q_OBJECT
public:
    /// Theme模式。System 暂时按 Dark 处理（占位）。
    /// 高对比度独立于Theme模式（见 is_high_contrast / set_high_contrast），
    /// 开启后在当前Theme基础上增StrongMedium性边框可见度，不改变Theme本身。
    enum class Mode {
        Dark,
        Light,
        System,
    };
    Q_ENUM(Mode)

    /// 加载持久化的Theme并应用到 \p app。应在 QApplication 构造后、显示主窗口前调用。
    static void load_initial_theme(QApplication* app);

    /// 返回单例实例（load_initial_theme 后才非空）。供需要监听 theme_changed 信号的对象使用。
    static Theme* instance();

    /// 当前模式。
    static Mode current_mode();

    /// SettingsTheme模式，自动持久化并即时Refresh样式表。
    static void set_mode(Mode mode);

    /// 在 Dark / Light 之间切换（System 视为 Dark）。
    static void toggle();

    /// 当前是否为DarkTheme。
    static bool is_dark();

    /// High contrast是否开启（独立于Theme模式，增StrongMedium性边框可见度）。
    static bool is_high_contrast();

    /// 开启/CloseHigh contrast，自动持久化并即时Refresh样式表。
    static void set_high_contrast(bool enabled);

signals:
    /// Theme变化时触发（仅对实例化对象生效，主要供 MainWindow 监听）。
    void theme_changed(Mode new_mode);
    /// 高对比度开关变化时触发（供 SettingsView 同步复选框状态）。
    void high_contrast_changed(bool enabled);

private:
    explicit Theme(QObject* parent = nullptr);

    static Theme* instance_;
    static Mode current_mode_;
    static bool high_contrast_;

    void apply_mode(Mode mode);
    void persist(Mode mode);
    Mode load_persisted() const;
    void persist_hc(bool enabled);
    bool load_persisted_hc() const;
};

}  // namespace yuli::vault::ui

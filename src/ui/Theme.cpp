// coding: utf-8
// =============================================================================
// Theme.cpp
//
// Yuli Vault Theme管理器实现。从 Qt 资源 :/dark.qss 或 :/light.qss 加载样式表，
// 应用到 QApplication。用户偏好持久化到 QSettings。
// =============================================================================
#include "Theme.h"
#include "AppSettings.h"

#include <QApplication>
#include <QFile>
#include <QString>
#include <QStringConverter>
#include <QTextStream>
#include <QVariant>

namespace yuli::vault::ui {

namespace {

/// QSettings MediumStorageTheme / 高对比度开关的键名。
constexpr const char* kSettingsKey = "ui/theme";
constexpr const char* kHcSettingsKey = "ui/high_contrast";

/// Theme对应的基础 qss 资源路径（不含 HC 增Strong）。
QString qss_resource_for_mode(Theme::Mode mode) {
    switch (mode) {
        case Theme::Mode::Light: return QStringLiteral(":/light.qss");
        case Theme::Mode::Dark:
        case Theme::Mode::System:
        default:                  return QStringLiteral(":/dark.qss");
    }
}

/// 高对比度增Strong片段资源路径。Dark模式用亮蓝边框（#3b6bff），
/// Light模式用纯黑边框（#000000），仅覆盖Medium性装饰边框，语义色边框保留原色。
QString hc_enhance_resource_for_mode(Theme::Mode mode) {
    return (mode == Theme::Mode::Light)
        ? QStringLiteral(":/hc_light_enhance.qss")
        : QStringLiteral(":/hc_dark_enhance.qss");
}

/// 加载 qss 文件全文。文件不存在时返回空字符串（不阻塞启动）。
QString load_qss(const QString& resource_path) {
    QFile f(resource_path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    QTextStream stream(&f);
    stream.setEncoding(QStringConverter::Utf8);
    return stream.readAll();
}

}  // namespace

// 静态成员定义
Theme* Theme::instance_ = nullptr;
Theme::Mode Theme::current_mode_ = Theme::Mode::Dark;
bool Theme::high_contrast_ = false;

Theme::Theme(QObject* parent) : QObject(parent) {}

void Theme::load_initial_theme(QApplication* app) {
    if (!instance_) {
        instance_ = new Theme(app);
    }
    const Mode persisted = instance_->load_persisted();
    current_mode_ = persisted;
    high_contrast_ = instance_->load_persisted_hc();
    instance_->apply_mode(persisted);
}

Theme* Theme::instance() {
    return instance_;
}

Theme::Mode Theme::current_mode() {
    return current_mode_;
}

void Theme::set_mode(Mode mode) {
    if (!instance_) return;
    if (mode == current_mode_) return;
    current_mode_ = mode;
    instance_->apply_mode(mode);
    instance_->persist(mode);
    emit instance_->theme_changed(mode);
}

void Theme::toggle() {
    set_mode(is_dark() ? Mode::Light : Mode::Dark);
}

bool Theme::is_dark() {
    return current_mode_ != Mode::Light;
}

bool Theme::is_high_contrast() {
    return high_contrast_;
}

void Theme::set_high_contrast(bool enabled) {
    if (!instance_) return;
    if (enabled == high_contrast_) return;
    high_contrast_ = enabled;
    instance_->apply_mode(current_mode_);
    instance_->persist_hc(enabled);
    emit instance_->high_contrast_changed(enabled);
}

// ---------------------------------------------------------------------------
// 内部实现
// ---------------------------------------------------------------------------

void Theme::apply_mode(Mode mode) {
    auto* app = qobject_cast<QApplication*>(parent());
    if (!app) return;
    // 基础 qss + 高对比度增Strong片段（HC 开启时追加，覆盖Medium性边框颜色）。
    QString qss = load_qss(qss_resource_for_mode(mode));
    if (high_contrast_) {
        qss += QStringLiteral("\n") + load_qss(hc_enhance_resource_for_mode(mode));
    }
    app->setStyleSheet(qss);
}

void Theme::persist(Mode mode) {
    settings_set(QString::fromLatin1(kSettingsKey), static_cast<int>(mode));
}

Theme::Mode Theme::load_persisted() const {
    const QVariant v = settings_value(QString::fromLatin1(kSettingsKey));
    if (!v.isValid()) return Mode::Dark;
    bool ok = false;
    const int iv = v.toInt(&ok);
    if (!ok) return Mode::Dark;
    switch (iv) {
        case static_cast<int>(Mode::Light):  return Mode::Light;
        case static_cast<int>(Mode::System): return Mode::System;
        case static_cast<int>(Mode::Dark):
        default:                              return Mode::Dark;
    }
}

void Theme::persist_hc(bool enabled) {
    settings_set(QString::fromLatin1(kHcSettingsKey), enabled);
}

bool Theme::load_persisted_hc() const {
    return settings_value(QString::fromLatin1(kHcSettingsKey), false).toBool();
}

}  // namespace yuli::vault::ui

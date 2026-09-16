// coding: utf-8
// =============================================================================
// IconKit.h
//
// SVG 图标着色工具集。
//
// 背景：项目 SVG 图标用 Lucide 风格 stroke="currentColor"，但 Qt 的
// QSvgRenderer 不支持 CSS currentColor 关键字，会 fallback 到黑色，导致
// 图标无法随Theme/状态变色。
//
// 方案：自定义 QIconEngine 按需着色渲染。engine 在 paint()/pixmap() 时
// 实时用 QSvgRenderer 渲染 SVG 到请求尺寸，再用 SourceIn 着色。这样：
//   ① 保留矢量特性，任意尺寸清晰，不预缩放 pixmap
//   ② 尺寸精确匹配调用方 setIconSize（engine 用 rect.size() 渲染）
//   ③ dpr 正确处理，高 DPI 屏幕清晰
//
// 用法：
//   btn->setIcon(tinted_icon(":/icons/lock.svg", IconRole::Normal));
//   btn->setIconSize(QSize(18, 18));
//   lbl->setPixmap(tinted_pixmap(":/icons/lock.svg", IconRole::Normal, QSize(18,18)));
//   // Theme切换 / 状态变化后重新调用以Refresh颜色
//
// 颜色由 IconRole 语义角色决定，具体色值随当前Theme（dark/light）推导。
// =============================================================================
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <QString>

class QPushButton;

namespace yuli::vault::ui {

/// 图标语义角色：决定图标渲染颜色。
/// 颜色值由当前Theme（dark/light）推导，Theme切换后需重新着色。
enum class IconRole {
    Normal,     ///< 常态：次要色（导航未选Medium、普通图标按钮、输入框内联图标）
    Active,     ///< 激活/选Medium：主文字色（导航选Medium项）
    OnPrimary,  ///< 位于 primary 按钮上：白色
    Danger,     ///< 危险动作：红色
    Success,    ///< Success：绿色（Security区 section 图标等）
    Info,       ///< 信息：蓝色（Generator section 图标等）
};

/// 获取当前Theme下指定角色的颜色（依赖 Theme::is_dark()）。
QColor icon_color(IconRole role);

/// 按需着色渲染 pixmap（用于 QLabel::setPixmap 等需要 QPixmap 的场景）。
/// 按设备像素比渲染保证高 DPI 清晰。
QPixmap tinted_pixmap(const QString& svg_path, const QColor& color, const QSize& size);

/// 便捷：按角色着色 pixmap。
QPixmap tinted_pixmap(const QString& svg_path, IconRole role, const QSize& size);

/// 着色 QIcon（基于自定义 QIconEngine，按需渲染）。
/// 显示尺寸由调用方 setIconSize 决定，无需在此指定。
QIcon tinted_icon(const QString& svg_path, const QColor& color);

/// 便捷：按角色着色 QIcon。
QIcon tinted_icon(const QString& svg_path, IconRole role);

/// 将文本Copy到剪贴板，并在 30 秒后自动清空。
/// 用于Password等敏感数据，避免明文长期留存剪贴板被其他程序读取。
/// 多次调用会重置计时器，不会提前清空后续Copy的内容。
void copy_secure_to_clipboard(const QString& text);

}  // namespace yuli::vault::ui

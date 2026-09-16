// coding: utf-8
// =============================================================================
// Toast.h
//
// 顶部居Medium浮层 Toast 组件，用于CopySuccess、保存Success等非阻塞反馈。
//
// 设计要点：
//   - 显示在 parent（通常为 MainWindow）顶部居Medium、纵向下方 80px 处
//   - 不抢焦点（Qt::WA_ShowWithoutActivating + Qt::ToolTip）
//   - 允许点击Close（WA_TransparentForMouseEvents = false + mousePressEvent）
//   - 默认 2 秒后通过 QTimer::singleShot + deleteLater 自动消失
//   - 视觉随Theme（dark/light）切换：圆角 6px、半透明背景、1px 边框
//   - 用内联 setStyleSheet（Toast 是临时浮层，不属于 QSS 体系管理的常规控件）
//
// 用法：
//   Toast::show(this, QStringLiteral("Copied. Clipboard clears in 30 seconds."));
//   Toast::show(this, QStringLiteral("Login saved"), 2000);
// =============================================================================
#pragma once

#include <QString>
#include <QWidget>

class QLabel;

namespace yuli::vault::ui {

/// 顶部居Medium浮层 Toast。显示一条短文本反馈，2 秒后自动消失。
class Toast : public QWidget {
    Q_OBJECT
public:
    /// 在 \p parent 顶部居Medium显示一条 toast。
    /// \param parent       锚定父窗口（通常为 MainWindow），用于定位。
    /// \param text         要显示的文本。
    /// \param duration_ms  自动消失时长（毫秒），默认 2000ms。
    static void show(QWidget* parent, const QString& text, int duration_ms = 2000);

protected:
    /// 点击 toast 任意位置立即Close（deleteLater）。
    void mousePressEvent(QMouseEvent* event) override;

private:
    explicit Toast(QWidget* parent, const QString& text, int duration_ms);

    QLabel* label_;
};

}  // namespace yuli::vault::ui

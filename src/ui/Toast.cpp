// coding: utf-8
// =============================================================================
// Toast.cpp
//
// Toast 组件实现。详见 Toast.h 的设计说明。
// =============================================================================
#include "Toast.h"
#include "Theme.h"

#include <QLabel>
#include <QMouseEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace yuli::vault::ui {

void Toast::show(QWidget* parent, const QString& text, int duration_ms) {
    // 自我管理生命周期：构造时即启动自动消失计时器，无需调用方 delete。
    auto* toast = new Toast(parent, text, duration_ms);
    // 显式调用 QWidget::show：本类的静态方法 show 遮蔽了基类的 show()，
    // 直接写 toast->show() 会被解析为 Toast::show(parent, text, duration_ms)
    // 导致编译错误（参数数量不匹配）。
    toast->QWidget::show();
}

Toast::Toast(QWidget* parent, const QString& text, int duration_ms)
    : QWidget(parent), label_(nullptr) {
    // 窗口标志：ToolTip 不抢焦点、不显示任务栏图标；FramelessWindowHint 无边框。
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    // 不激活窗口（不夺走主窗口焦点）。
    setAttribute(Qt::WA_ShowWithoutActivating);
    // 允许半透明背景（圆角外侧区域透明，避免矩形灰边）。
    setAttribute(Qt::WA_TranslucentBackground);
    // 显式允许接收鼠标事件（点击Close）。
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setFocusPolicy(Qt::NoFocus);

    // 内部 QLabel 显示文本，字号 13px。
    label_ = new QLabel(text, this);
    label_->setAlignment(Qt::AlignCenter);
    QFont label_font = label_->font();
    label_font.setPixelSize(13);
    label_->setFont(label_font);

    // padding 10px 18px 通过布局 margins 实现。
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 10, 18, 10);
    layout->setSpacing(0);
    layout->addWidget(label_);

    // 视觉：圆角 6px、半透明背景、1px 边框，按Theme取色。
    // 用内联 setStyleSheet（Toast 是临时浮层，不属于 QSS 体系管理的常规控件）。
    const bool dark = Theme::is_dark();
    const QString bg = dark ? QStringLiteral("rgba(20,24,32,0.92)")
                            : QStringLiteral("rgba(255,255,255,0.95)");
    const QString border = dark ? QStringLiteral("#2a3344")
                                : QStringLiteral("#d6e0ff");
    const QString fg = dark ? QStringLiteral("#e8ecf4")
                            : QStringLiteral("#0f1626");
    setStyleSheet(QStringLiteral(
        "Toast {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 6px;"
        "}"
        "QLabel {"
        "  background: transparent;"
        "  border: none;"
        "  color: %3;"
        "  font-size: 13px;"
        "}"
    ).arg(bg, border, fg));

    // 尺寸：根据内容自适应（layout + label sizeHint）。
    adjustSize();

    // 位置：parent 顶部居Medium，纵向下方 80px。
    // 用 mapToGlobal 兼容 parent 为顶层窗口或嵌套 widget 两种情况。
    if (parent) {
        const QPoint top_left = parent->mapToGlobal(QPoint(0, 0));
        const int x = top_left.x() + (parent->width() - width()) / 2;
        const int y = top_left.y() + 80;
        move(x, y);
    }

    // 自动消失：duration_ms 后 deleteLater。
    // 用 this 作为 context，若 toast 已被点击销毁，回调不会再触发（Security）。
    QTimer::singleShot(duration_ms, this, [this] { this->deleteLater(); });
}

void Toast::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        deleteLater();
        return;
    }
    QWidget::mousePressEvent(event);
}

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// FlowLayout.cpp
//
// 流式布局实现。子控件按行排列，行宽超出容器宽度时换行。
// =============================================================================
#include "FlowLayout.h"

#include <QWidget>

namespace yuli::vault::ui {

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), h_space_(hSpacing), v_space_(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::FlowLayout(int margin, int hSpacing, int vSpacing)
    : h_space_(hSpacing), v_space_(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
    while (!items_.isEmpty()) {
        delete takeAt(0);
    }
}

void FlowLayout::addItem(QLayoutItem* item) {
    items_.append(item);
}

int FlowLayout::horizontalSpacing() const {
    return h_space_ >= 0 ? h_space_ : smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const {
    return v_space_ >= 0 ? v_space_ : smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
    QObject* p = parent();
    if (!p) return -1;
    if (p->isWidgetType()) {
        auto* w = static_cast<QWidget*>(p);
        return w->style()->pixelMetric(pm, nullptr, w);
    }
    return static_cast<QLayout*>(p)->spacing();
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    const int height = doLayout(QRect(0, 0, width, 0), true);
    return height;
}

int FlowLayout::count() const {
    return static_cast<int>(items_.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const {
    return items_.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index) {
    return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const {
    return {0};
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (QLayoutItem* item : items_) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins m = contentsMargins();
    size += QSize(m.left() + m.right(), m.top() + m.bottom());
    return size;
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const {
    return minimumSize();
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    const int left = rect.x();
    const int top = rect.y();
    const int right = rect.right();

    QMargins m = contentsMargins();
    int x = left + m.left();
    int y = top + m.top();
    int lineHeight = 0;

    for (QLayoutItem* item : items_) {
        const int spaceX = horizontalSpacing();
        const int spaceY = verticalSpacing();
        const int nextX = x + item->sizeHint().width() + spaceX;
        if (nextX - spaceX > right - m.right() && lineHeight > 0) {
            x = left + m.left();
            y = y + lineHeight + spaceY;
            lineHeight = 0;
        }
        if (!testOnly) {
            item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
        }
        x = nextX;
        lineHeight = qMax(lineHeight, item->sizeHint().height());
    }
    return y + lineHeight - top + m.bottom();
}

}  // namespace yuli::vault::ui

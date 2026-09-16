// coding: utf-8
// =============================================================================
// FlowLayout.h
//
// 流式布局：子控件按从左到右排列，超出宽度自动换行。用于 TagInputWidget 的
// Tags芯片排列。基于 Qt 官方 FlowLayout 示例简化实现。
// =============================================================================
#pragma once

#include <QLayout>
#include <QList>
#include <QRect>
#include <QStyle>

namespace yuli::vault::ui {

class FlowLayout : public QLayout {
    Q_OBJECT
public:
    explicit FlowLayout(QWidget* parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
    explicit FlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QLayoutItem* takeAt(int index) override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem*> items_;
    int h_space_;
    int v_space_;
};

}  // namespace yuli::vault::ui

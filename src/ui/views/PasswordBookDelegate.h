// coding: utf-8
// =============================================================================
// PasswordBookDelegate.h
//
// Vault左侧列表项自绘 delegate。把一个 core::PasswordEntry 渲染到列表项
// 矩形内：左侧 40x40 圆形头像（首字母）+ Title（elide）+ Tags chips（最多
// 3 个 + "+N"）+ 右侧 chevron-right。
//
// 替换原 QListWidget + setItemWidget（每项 new 一个 QWidget）方案，性能更好
// 且内存占用更低。选Medium态/悬停态背景由 delegate 自绘，不依赖 QSS ::item 规则。
//
// 与 PasswordBookModel 配合使用：从 index.data(Qt::UserRole) 取
// core::PasswordEntry（Q_DECLARE_METATYPE 注册）。
// =============================================================================
#pragma once

#include <QHash>
#include <QPixmap>
#include <QStyledItemDelegate>
#include <QString>

#include "Types.h"

namespace yuli::vault::ui {

class PasswordBookDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit PasswordBookDelegate(QObject* parent = nullptr);
    ~PasswordBookDelegate() override;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override;

private:
    /// 取当前Theme下的 chevron pixmap，Theme切换时缓存自动失效。
    /// 在 const paint() Medium调用，故声明为 mutable。
    QPixmap chevron_pixmap() const;

    /// 清空 pixmap 缓存（Theme切换时调用）。
    void clear_pixmap_cache();

    mutable QHash<QString, QPixmap> pixmap_cache_;
};

}  // namespace yuli::vault::ui

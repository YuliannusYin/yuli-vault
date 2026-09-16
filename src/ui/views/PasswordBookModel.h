// coding: utf-8
// =============================================================================
// PasswordBookModel.h
//
// Vault左侧列表的 QAbstractListModel 实现。管理
// std::vector<core::PasswordEntry>，通过 Qt::UserRole 暴露完整 entry 给
// delegate 自绘，通过 Qt::ToolTipRole 暴露完整 entry_name 用于长名称 tooltip。
// =============================================================================
#pragma once

#include <QAbstractListModel>
#include <vector>

#include "Types.h"

namespace yuli::vault::ui {

class PasswordBookModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit PasswordBookModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    /// 替换全部条目（重置模型）。
    void set_entries(const std::vector<core::PasswordEntry>& entries);

    /// 取指定行的条目指针（无效索引返回 nullptr）。
    const core::PasswordEntry* entry_at(int row) const;

    /// 取指定 id 的条目指针（未找到返回 nullptr）。
    const core::PasswordEntry* entry_by_id(int64_t id) const;

private:
    std::vector<core::PasswordEntry> entries_;
};

}  // namespace yuli::vault::ui

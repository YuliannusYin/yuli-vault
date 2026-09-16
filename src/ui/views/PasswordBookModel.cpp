// coding: utf-8
// =============================================================================
// PasswordBookModel.cpp
// =============================================================================
#include "PasswordBookModel.h"

#include <QVariant>

// 在 QVariant Medium流转 core::PasswordEntry 需注册 metatype。
// std::string / std::vector 已被 Qt 内置支持，无需额外注册。
Q_DECLARE_METATYPE(yuli::vault::core::PasswordEntry)

namespace yuli::vault::ui {

PasswordBookModel::PasswordBookModel(QObject* parent)
    : QAbstractListModel(parent)
{
    // 让 QVariant::fromValue 在 queued connections / signal-slot Medium可用。
    qRegisterMetaType<core::PasswordEntry>();
}

int PasswordBookModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(entries_.size());
}

QVariant PasswordBookModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return QVariant();
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(entries_.size())) return QVariant();

    const auto& entry = entries_[static_cast<size_t>(row)];
    switch (role) {
        case Qt::DisplayRole:
            // 备用：entry_name 回退到 account / website
            if (!entry.title.empty()) return QString::fromStdString(entry.title);
            if (!entry.account.empty())    return QString::fromStdString(entry.account);
            return QString::fromStdString(entry.website);
        case Qt::ToolTipRole:
            // 长名称 tooltip：显示完整 entry_name（paint 时 elide 截断，tooltip 给完整）
            return QString::fromStdString(entry.title);
        case Qt::UserRole:
            // 完整 entry 给 delegate 自绘
            return QVariant::fromValue(entry);
        default:
            return QVariant();
    }
}

void PasswordBookModel::set_entries(const std::vector<core::PasswordEntry>& entries) {
    beginResetModel();
    entries_ = entries;
    endResetModel();
}

const core::PasswordEntry* PasswordBookModel::entry_at(int row) const {
    if (row < 0 || row >= static_cast<int>(entries_.size())) return nullptr;
    return &entries_[static_cast<size_t>(row)];
}

const core::PasswordEntry* PasswordBookModel::entry_by_id(int64_t id) const {
    if (id == 0) return nullptr;
    for (const auto& e : entries_) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

}  // namespace yuli::vault::ui

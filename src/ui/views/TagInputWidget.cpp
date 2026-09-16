// coding: utf-8
// =============================================================================
// TagInputWidget.cpp
//
// Tags输入控件实现。
// =============================================================================
#include "TagInputWidget.h"

#include <QCompleter>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringListModel>
#include <QToolButton>

#include "FlowLayout.h"
#include "Types.h"

namespace yuli::vault::ui {

TagInputWidget::TagInputWidget(QWidget* parent) : QWidget(parent) {
    chips_layout_ = new FlowLayout(this, /*margin=*/0, /*hSpacing=*/6, /*vSpacing=*/6);

    input_ = new QLineEdit(this);
    input_->setProperty("cssClass", QStringLiteral("tagInput"));
    input_->setPlaceholderText(tr("Type a tag and press Enter"));
    input_->setMinimumWidth(120);
    input_->installEventFilter(this);
    chips_layout_->addWidget(input_);

    completer_ = new QCompleter(this);
    completer_->setModel(new QStringListModel(this));
    completer_->setCaseSensitivity(Qt::CaseSensitive);
    completer_->setFilterMode(Qt::MatchContains);
    input_->setCompleter(completer_);

    connect(input_, &QLineEdit::returnPressed, this, &TagInputWidget::on_return_pressed);
}

TagInputWidget::~TagInputWidget() = default;

void TagInputWidget::set_existing_tags(const std::vector<core::Tag>& tags) {
    existing_tags_ = tags;
    QStringList names;
    names.reserve(static_cast<int>(tags.size()));
    for (const auto& t : tags) names << QString::fromStdString(t.name);
    if (auto* model = qobject_cast<QStringListModel*>(completer_->model())) {
        model->setStringList(names);
    }
}

void TagInputWidget::set_selected_tags(const std::vector<core::Tag>& tags) {
    selected_tags_ = tags;
    rebuild_chips();
}

std::vector<core::Tag> TagInputWidget::selected_tags() const {
    return selected_tags_;
}

void TagInputWidget::rebuild_chips() {
    // 先把 input_ 从布局Medium取出（保留 widget，不Delete）
    // input_ 始终是最后一个 item
    QLayoutItem* input_item = nullptr;
    while (chips_layout_->count() > 0) {
        QLayoutItem* item = chips_layout_->takeAt(chips_layout_->count() - 1);
        if (!item) break;
        QWidget* w = item->widget();
        if (w == input_) {
            input_item = item;  // 保留，稍后重新加入
        } else {
            if (w) w->deleteLater();
            delete item;
        }
    }

    // 重新添加芯片
    for (const auto& tag : selected_tags_) {
        const QString tag_name = QString::fromStdString(tag.name);

        auto* chip = new QFrame(this);
        chip->setObjectName(QStringLiteral("tagChip"));
        chip->setProperty("cssClass", QStringLiteral("tagChip"));

        auto* hl = new QHBoxLayout(chip);
        hl->setContentsMargins(8, 2, 4, 2);
        hl->setSpacing(4);

        auto* name = new QLabel(tag_name, chip);
        name->setObjectName(QStringLiteral("tagChipName"));
        hl->addWidget(name);

        auto* btn = new QToolButton(chip);
        btn->setObjectName(QStringLiteral("tagChipRemove"));
        btn->setText(QStringLiteral("×"));
        btn->setFixedSize(16, 16);
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QToolButton::clicked, this, [this, tag_name]() {
            const std::string target = tag_name.toStdString();
            for (size_t i = 0; i < selected_tags_.size(); ++i) {
                if (selected_tags_[i].name == target) {
                    remove_tag_at(static_cast<int>(i));
                    return;
                }
            }
        });
        hl->addWidget(btn);

        chips_layout_->addWidget(chip);
    }

    // 重新添加 input_（最后）
    if (input_item) {
        chips_layout_->addItem(input_item);
    } else {
        chips_layout_->addWidget(input_);
    }

    updateGeometry();
}

void TagInputWidget::on_return_pressed() {
    const QString text = input_->text().trimmed();
    if (!text.isEmpty()) {
        add_tag_by_name(text);
        input_->clear();
    }
    focus_input();
}

void TagInputWidget::add_tag_by_name(const QString& name) {
    const std::string name_std = name.toStdString();
    if (name_std.empty()) return;

    // 去重：与已选Tags同名（大小写敏感）则跳过
    for (const auto& t : selected_tags_) {
        if (t.name == name_std) {
            focus_input();
            return;
        }
    }

    // 查找已有Tags
    core::Tag tag;
    bool found = false;
    for (const auto& t : existing_tags_) {
        if (t.name == name_std) {
            tag = t;
            found = true;
            break;
        }
    }
    if (!found) {
        // 新Tags：id=0 表示尚未分配
        tag.id = 0;
        tag.name = name_std;
    }

    selected_tags_.push_back(tag);
    rebuild_chips();
    emit tags_changed();
    focus_input();
}

void TagInputWidget::remove_tag_at(int index) {
    if (index < 0 || index >= static_cast<int>(selected_tags_.size())) return;
    selected_tags_.erase(selected_tags_.begin() + index);
    rebuild_chips();
    emit tags_changed();
    focus_input();
}

void TagInputWidget::focus_input() {
    input_->setFocus();
}

bool TagInputWidget::eventFilter(QObject* obj, QEvent* event) {
    if (obj == input_ && event->type() == QEvent::KeyPress) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        if (key_event->key() == Qt::Key_Backspace && input_->text().isEmpty()) {
            remove_tag_at(static_cast<int>(selected_tags_.size()) - 1);
            return true;  // 事件已处理，不再传递
        }
    }
    return QWidget::eventFilter(obj, event);
}

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// TagInputWidget.h
//
// Tags输入控件：以可Close芯片（chip）形式展示已选Tags，下方 QLineEdit 输入新Tags，
// QCompleter 补全已有Tags名。采用 FlowLayout 实现芯片自动换行。
//
// 交互：
//   - 输入Tags名 + 回车 → 添加芯片（若已有同名Tags则复用，否则创建 id=0 新Tags）
//   - 输入框为空时按退格 → Delete最后一个芯片
//   - 点击芯片上的 × → Delete该芯片
//   - 大小写敏感的去重（与 core::Tag.name 唯一约束一致）
// =============================================================================
#pragma once

#include <QWidget>
#include <vector>

#include "Types.h"

class QLineEdit;
class QCompleter;

namespace yuli::vault::ui {

class FlowLayout;

class TagInputWidget : public QWidget {
    Q_OBJECT
public:
    explicit TagInputWidget(QWidget* parent = nullptr);
    ~TagInputWidget() override;

    /// Settings全部已知Tags（用于补全与按名查找）。
    void set_existing_tags(const std::vector<core::Tag>& tags);

    /// Settings当前已选Tags（覆盖）。
    void set_selected_tags(const std::vector<core::Tag>& tags);

    /// 获取当前已选Tags。
    [[nodiscard]] std::vector<core::Tag> selected_tags() const;

signals:
    /// 选MediumTags集合变化时触发。
    void tags_changed();

private slots:
    void on_return_pressed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void rebuild_chips();
    void add_tag_by_name(const QString& name);
    void remove_tag_at(int index);
    void focus_input();

    FlowLayout* chips_layout_;
    QLineEdit* input_;
    QCompleter* completer_;

    std::vector<core::Tag> existing_tags_;
    std::vector<core::Tag> selected_tags_;
};

}  // namespace yuli::vault::ui

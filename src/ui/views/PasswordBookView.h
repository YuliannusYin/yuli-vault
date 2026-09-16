// coding: utf-8
// =============================================================================
// PasswordBookView.h
//
// Yuli Vault Vault视图（新设计）。master-detail 布局：
//   - 顶部搜索行：搜索框（带 search 图标前缀）+ 字段下拉 + Refresh按钮
//   - 左侧 340px 列表：每条目 = 头像（首字母）+ Title + Account + chevron-right
//   - 右侧详情：头部（大头像 + Title + Account + Copy/Edit/Delete按钮）
//                + 字段网格（Title/Account/Username/Password（带可见性切换）/Website/Tags/Notes (Markdown)/Time）
//                + 外部链接按钮
//
// 进入视图时自动调用 list_entries 加载；条目数变化时 emit entry_count_changed。
// =============================================================================
#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <vector>

#include "Types.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QModelIndex;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QTimer;
class QWidget;

namespace yuli::vault::ui {

class FlowLayout;
class IpcClient;
class PasswordBookDelegate;
class PasswordBookModel;

class PasswordBookView : public QWidget {
    Q_OBJECT
public:
    explicit PasswordBookView(IpcClient* client, QWidget* parent = nullptr);
    ~PasswordBookView() override;

    /// 重新加载列表（调用 list_entries）。
    void refresh();

    /// 把焦点Settings到搜索框（供全局快捷键 Ctrl+F 调用）。
    void focus_search();

    /// 把焦点Settings到左侧列表（供 MainWindow 视图切换后键盘导航调用）。
    void focus_list();

signals:
    /// Edit条目时触发，MainWindow 可用于切换视图或Refresh。
    void entry_updated(int64_t id);

    /// Item count变化时触发，MainWindow 用于更新顶栏 badge。
    void entry_count_changed(int count);

    /// 用户在空状态点击「New item」时触发，MainWindow 切换到 InputView。
    void entry_add_requested();

private slots:
    void on_search_clicked();
    void on_refresh_clicked();
    void on_search_text_changed(const QString& text);
    void on_list_current_changed(const QModelIndex& current, const QModelIndex& previous);
    void on_copy_account_clicked();
    void on_copy_password_clicked();
    void on_toggle_password_clicked();
    void on_edit_clicked();
    void on_delete_clicked();
    void on_open_external_clicked();

private:
    void build_ui();
    void populate_list(const std::vector<core::PasswordEntry>& entries);
    void clear_detail();
    void load_detail(const core::PasswordEntry& entry);
    void set_detail_actions_enabled(bool enabled);
    const core::PasswordEntry* current_entry() const;
    void show_empty_state(const QString& message);

    // 异步 IPC 与 UI 辅助
    void begin_loading(const QString& message);
    void end_loading();
    void refresh_async();
    void search_async(const QString& text, const QString& field);
    void clear_note_cache_for(int64_t id);
    void clear_all_note_cache();

    IpcClient* client_;
    std::vector<core::PasswordEntry> entries_;
    int64_t current_id_ = 0;
    bool password_visible_ = false;

    // 增量搜索 debounce
    QTimer* search_timer_ = nullptr;
    QString pending_search_text_;

    // Markdown 渲染缓存（entry.id → html）
    QHash<int64_t, QString> note_cache_;

    // 加载Medium占位提示（复用 empty_hint_，仅作语义别名指针）
    QLabel* loading_hint_ = nullptr;

    // 顶部搜索行
    QLineEdit* search_edit_ = nullptr;
    QComboBox* field_combo_ = nullptr;
    QPushButton* refresh_button_ = nullptr;

    // 左侧列表
    QListView* list_ = nullptr;
    PasswordBookModel* model_ = nullptr;
    PasswordBookDelegate* delegate_ = nullptr;

    // 右侧详情
    QScrollArea* detail_scroll_ = nullptr;
    QWidget* detail_container_ = nullptr;
    QLabel* detail_avatar_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QLabel* detail_subtitle_ = nullptr;
    QPushButton* copy_account_btn_ = nullptr;
    QPushButton* copy_pwd_btn_ = nullptr;
    QPushButton* edit_btn_ = nullptr;
    QPushButton* delete_btn_ = nullptr;

    QLabel* field_entry_name_ = nullptr;
    QLabel* field_account_ = nullptr;
    QLabel* field_username_ = nullptr;
    QLabel* field_website_ = nullptr;
    QLabel* field_password_ = nullptr;
    QWidget* field_tags_container_ = nullptr;  ///< Tags容器（用 FlowLayout 展示只读芯片）
    FlowLayout* field_tags_layout_ = nullptr;
    QLabel* field_created_ = nullptr;
    QLabel* field_updated_ = nullptr;
    QTextBrowser* field_note_ = nullptr;  ///< Notes（markdown 渲染）
    QLabel* strength_badge_ = nullptr;
    QPushButton* toggle_pwd_btn_ = nullptr;
    QPushButton* copy_entry_name_btn_ = nullptr;
    QPushButton* copy_account_field_btn_ = nullptr;
    QPushButton* copy_user_field_btn_ = nullptr;
    QPushButton* copy_pwd_field_btn_ = nullptr;
    QPushButton* copy_website_btn_ = nullptr;
    QPushButton* open_external_btn_ = nullptr;

    // 空状态提示
    QLabel* empty_hint_ = nullptr;
    QPushButton* empty_add_button_ = nullptr;  ///< 空状态引导按钮
};

}  // namespace yuli::vault::ui

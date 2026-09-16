// coding: utf-8
// =============================================================================
// InputView.h
//
// Yuli Vault New Item视图（新设计）。640px 居Medium卡片：
//   - 标题「New login」+ 副标题「Sensitive fields are encrypted locally」
//   - 表单：*Title / *Account / Username / *Password（key 图标 + Generate + 可见性）
//     + 4 段Strong度条 / Website / Tags（芯片流式输入）/ Notes（markdown 源码）
//   - 底部：Clear + 保存按钮
//
// 带 * 号为必填：entry_name / account / password。
// 保存Success后 emit entry_added(id)，清空表单并弹提示。
// 「Generate」按钮 emit password_generator_requested，由 MainWindow 切换到Generator视图。
// =============================================================================
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QShowEvent;
class QTimer;

namespace yuli::vault::core {
struct StrengthEstimate;
}

namespace yuli::vault::ui {

class IpcClient;
class TagInputWidget;

class InputView : public QWidget {
    Q_OBJECT
public:
    explicit InputView(IpcClient* client, QWidget* parent = nullptr);
    ~InputView() override;

    /// 由 MainWindow 调用：将GeneratorGenerate的Password填入Password输入框。
    void set_password(const QString& password);

    /// 让第一个字段获得焦点（由 MainWindow 在点击「新增」时调用）。
    void focus_first_field();

signals:
    /// 新条目保存Success时触发，\p id 为 service 分配的主键。
    void entry_added(int64_t id);

    /// 用户点击「Generate password」时触发，MainWindow 切换到Generator视图。
    void password_generator_requested();

protected:
    /// 首次显示时触发Tags补全列表异步加载（Task 28 防重）。
    void showEvent(QShowEvent* event) override;

private slots:
    void on_generate_clicked();
    void on_toggle_password_clicked();
    void on_password_changed(const QString& text);
    void on_save_clicked();
    void on_clear_clicked();

private:
    void build_ui();
    /// 入口：启动 debounce 计时器，300ms 无新输入后发起异步Strong度评估。
    void update_strength(const QString& password);
    /// UI 更新：根据 service 返回的 StrengthEstimate RefreshStrong度条与文案。
    void update_strength_ui(const core::StrengthEstimate& estimate);
    void set_error(const QString& message);
    /// 同步入口（向后兼容），内部委托给 async Version。
    void refresh_existing_tags();
    /// 异步加载全部已知Tags，Refresh TagInputWidget 的补全列表。
    void refresh_existing_tags_async();

    IpcClient* client_;

    QLineEdit* entry_name_edit_ = nullptr;  ///< *必填* 条目显示标题
    QLineEdit* account_edit_ = nullptr;      ///< *必填* 登录Account
    QLineEdit* username_edit_ = nullptr;     ///< 可选 显示名
    QLineEdit* password_edit_ = nullptr;     ///< *必填* 明文Password
    QLineEdit* website_edit_ = nullptr;     ///< 可选 站点 URL
    TagInputWidget* tag_input_ = nullptr;    ///< Tags芯片输入
    QPlainTextEdit* note_edit_ = nullptr;     ///< Notes（markdown 源码）
    QPushButton* generate_button_ = nullptr;
    QPushButton* visibility_btn_ = nullptr;
    QProgressBar* strength_bar_ = nullptr;
    QLabel* strength_label_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* clear_button_ = nullptr;
    QLabel* error_label_ = nullptr;
    QTimer* strength_timer_ = nullptr;  ///< Strong度评估 debounce 计时器
    bool password_visible_ = false;
    bool tags_loaded_ = false;       ///< Task 28：Tags补全列表是否已加载，防重复请求
    bool saving_ = false;            ///< 保存Medium状态，防重复点击
    QString pending_password_;       ///< debounce 期间捕获的Password文本
};

}  // namespace yuli::vault::ui

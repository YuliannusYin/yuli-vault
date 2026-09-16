// coding: utf-8
// =============================================================================
// EditEntryDialog.h
//
// Yuli Vault Edit条目对话框（新设计）。模态遮罩弹窗：
//   - 半透明遮罩层（rgba(0,0,0,0.55)）覆盖父窗口
//   - 560px 居Medium卡片：头部（pencil 图标 + 标题 + X Close）
//                       + 体（*Title / Username / *Account / *Password（带可见性+Generate）
//                            / Website / Tags / Notes (Markdown) / 更新Time）
//                       + 尾部（Cancel + Save changes）
//
// 预填已有数据，保存时调用 client->update_entry。
// 仍继承 QDialog 以便 PasswordBookView 用 dlg.exec() 阻塞。
// =============================================================================
#pragma once

#include <QDialog>
#include <QString>

#include "Types.h"

class QCloseEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTimer;

namespace yuli::vault::ui {

class IpcClient;
class TagInputWidget;

class EditEntryDialog : public QDialog {
    Q_OBJECT
public:
    EditEntryDialog(IpcClient* client, const core::PasswordEntry& entry,
                    QWidget* parent = nullptr);
    ~EditEntryDialog() override;

signals:
    /// 保存Success时触发，\p id 为更新条目的 id。
    void entry_updated(int64_t id);

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    /// 首次显示时触发Tags补全列表异步加载（Task 28 防重）。
    void showEvent(QShowEvent* event) override;

private slots:
    void on_toggle_password_clicked();
    void on_generate_clicked();
    void on_password_changed(const QString& text);
    void on_save_clicked();
    void on_cancel_clicked();

private:
    void build_ui();
    /// 入口：启动 debounce 计时器，300ms 无新输入后发起异步Strong度评估。
    void update_strength(const QString& password);
    /// UI 更新：根据 service 返回的 StrengthEstimate RefreshStrong度文案。
    void update_strength_ui(const core::StrengthEstimate& estimate);
    void set_error(const QString& message);
    /// 同步入口（向后兼容），内部委托给 async Version。
    void refresh_existing_tags();
    /// 异步加载全部已知Tags，Refresh TagInputWidget 的补全列表。
    void refresh_existing_tags_async();

    IpcClient* client_;
    core::PasswordEntry entry_;

    QLabel* error_label_ = nullptr;
    QLineEdit* entry_name_edit_ = nullptr;  ///< *必填* 条目显示标题
    QLineEdit* account_edit_ = nullptr;     ///< *必填* 登录Account
    QLineEdit* username_edit_ = nullptr;    ///< 可选 显示名
    QLineEdit* password_edit_ = nullptr;    ///< *必填* 明文Password
    QLineEdit* website_edit_ = nullptr;    ///< 可选 站点 URL
    TagInputWidget* tag_input_ = nullptr;  ///< Tags芯片输入
    QPlainTextEdit* note_edit_ = nullptr;    ///< Notes（markdown 源码）
    QPushButton* visibility_btn_ = nullptr;
    QPushButton* generate_btn_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QLabel* strength_label_ = nullptr;
    QLabel* updated_label_ = nullptr;
    QTimer* strength_timer_ = nullptr;  ///< Strong度评估 debounce 计时器
    bool password_visible_ = false;
    bool tags_loaded_ = false;       ///< Task 28：Tags补全列表是否已加载，防重复请求
    bool saving_ = false;            ///< 保存Medium状态，防重复点击
    QString pending_password_;       ///< debounce 期间捕获的Password文本
};

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// ProgramPasswordDialog.h
//
// Yuli Vault Program password管理对话框（新设计）。
//
// 单个 Tab 切换弹窗（480px 居Medium卡片 + 模态遮罩），三种模式通过分段控件切换：
//   - Enable：首次EnableProgram password，输入新Password + 确认 + Strong度条，
//             调用 client->enable_program_password(password)
//   - Change：ChangeProgram password，输入当前Password + 新Password + 确认 + Strong度条，
//             调用 client->change_program_password(old, new)
//   - Disable：DisableProgram password，输入当前Password验证，
//             调用 client->disable_program_password(password)
//
// ActionsSuccess后 emit succeeded()，由 SettingsView Refresh状态显示。
// 用户Close（X / Cancel / ESC）则 emit rejected()。
// =============================================================================
#pragma once

#include <QDialog>

class QButtonGroup;
class QCloseEvent;
class QFrame;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QTimer;

namespace yuli::vault::ui {

class IpcClient;

class ProgramPasswordDialog : public QDialog {
    Q_OBJECT
public:
    /// 模式（同时是 Tab 索引）。
    enum class Mode {
        Enable,   ///< EnableProgram password
        Change,   ///< ChangeProgram password
        Disable,  ///< DisableProgram password
    };
    Q_ENUM(Mode)

    /// 构造对话框。
    /// \param initial_mode 默认显示哪个 Tab
    ProgramPasswordDialog(IpcClient* client,
                          Mode initial_mode = Mode::Change,
                          QWidget* parent = nullptr);
    ~ProgramPasswordDialog() override;

signals:
    /// ActionsSuccess时触发，调用方应RefreshProgram password状态显示。
    void succeeded();

    /// 用户Close对话框但Actions未Success时触发。
    void rejected();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void on_mode_tab_clicked(int idx);
    void on_toggle_current_clicked();
    void on_toggle_new_clicked();
    void on_toggle_confirm_clicked();
    void on_new_password_changed(const QString& text);
    void on_cancel_clicked();
    void on_submit_clicked();

private:
    void build_ui();
    /// 切换模式时显示/隐藏对应字段
    void apply_mode(Mode mode);
    /// 更新Strong度条
    void update_strength(const QString& password);
    /// Settings错误提示
    void set_error(const QString& message);
    /// Close对话框（标记为非Success）
    void close_dialog();

    IpcClient* client_;
    Mode current_mode_;
    bool succeeded_ = false;
    bool rejected_emitted_ = false;  ///< 防止 closeEvent 与 close_dialog 重复 emit rejected
    QTimer* strength_timer_ = nullptr;  ///< Strong度评估 debounce 计时器

    // 模式 Tab
    QButtonGroup* mode_group_ = nullptr;
    QPushButton* tab_enable_ = nullptr;
    QPushButton* tab_change_ = nullptr;
    QPushButton* tab_disable_ = nullptr;

    // 字段
    QFrame* current_frame_ = nullptr;     // 包含「当前Program password」整行
    QLineEdit* current_edit_ = nullptr;
    QPushButton* toggle_current_btn_ = nullptr;
    bool current_visible_ = false;

    QFrame* new_frame_ = nullptr;          // 包含「新Program password」整行
    QLineEdit* new_edit_ = nullptr;
    QPushButton* toggle_new_btn_ = nullptr;
    bool new_visible_ = false;
    QFrame* strength_bar_ = nullptr;       // 自定义 4 段Strong度条
    QLabel* strength_label_ = nullptr;

    QFrame* confirm_frame_ = nullptr;      // 包含「Confirm new password」整行
    QLineEdit* confirm_edit_ = nullptr;
    QPushButton* toggle_confirm_btn_ = nullptr;
    bool confirm_visible_ = false;

    QLabel* error_label_ = nullptr;

    // 提交按钮文案随模式变化
    QPushButton* submit_btn_ = nullptr;
};

}  // namespace yuli::vault::ui

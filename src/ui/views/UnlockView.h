// coding: utf-8
// =============================================================================
// UnlockView.h
//
// Yuli Vault Unlock视图（新设计）。380px 居Medium卡片：
//   - 盾牌图标
//   - 标题「Yuli Vault」+ 副标题「Enter the program password to unlock the vault」
//   - Password输入框（带 lock 图标前缀 + 可见性切换图标按钮）
//   - 提示行：盾牌图标 + 「Five failed attempts lock the vault for 5 minutes」+ 右侧「Attempts left 5/5」
//   - Unlock按钮（满宽 primary）
//   - 底部分隔线 + 「Local encryption · AES-256-GCM · Argon2id」
//
// 模态遮罩：以独立 QWidget 全屏覆盖父窗口实现（避免 QDialog 风格差异）。
// =============================================================================
#pragma once

#include <QWidget>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace yuli::vault::ui {

class IpcClient;

class UnlockView : public QWidget {
    Q_OBJECT
public:
    explicit UnlockView(IpcClient* client, QWidget* parent = nullptr);
    ~UnlockView() override;

signals:
    /// UnlockSuccess时触发。
    void unlock_succeeded();

    /// 用户Close对话框但未UnlockSuccess时触发。
    void rejected();

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private slots:
    void on_visibility_toggled();
    void on_submit_clicked();
    void on_password_changed(const QString& text);
    /// 冷却倒计时每秒 tick：递减剩余秒数，归零后恢复输入控件。
    void on_cooldown_tick();

private:
    void build_ui();
    void set_error(const QString& message);
    void update_attempts_display();
    /// 从 service 返回的 error_message 解析剩余次数 / 冷却秒数。
    /// 命Medium任一格式返回 true（已同步 UI 显示）；均不命Medium返回 false（调用方回退本地递减）。
    bool parse_unlock_failure(const QString& message);
    /// 进入冷却态：Disable提交与Password输入框，启动每秒倒计时。
    void start_cooldown(int seconds);

    IpcClient* client_;
    bool unlock_succeeded_ = false;
    int remaining_attempts_ = 5;
    int cooldown_remaining_seconds_ = 0;

    QLabel* shield_icon_label_ = nullptr;
    QLabel* title_label_ = nullptr;
    QLabel* subtitle_label_ = nullptr;
    QLineEdit* password_edit_ = nullptr;
    QPushButton* visibility_btn_ = nullptr;
    QLabel* attempts_label_ = nullptr;
    QPushButton* submit_button_ = nullptr;
    QLabel* error_label_ = nullptr;
    QTimer* cooldown_timer_ = nullptr;
    bool password_visible_ = false;
};

}  // namespace yuli::vault::ui

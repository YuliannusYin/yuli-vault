// coding: utf-8
// =============================================================================
// UnlockView.cpp
//
// Yuli Vault Unlock视图实现（新设计）。380px 居Medium卡片 + 盾牌图标 + 可见性切换。
// =============================================================================
#include "UnlockView.h"
#include "ErrorMessages.h"
#include "IpcClient.h"
#include "IconKit.h"

#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QString>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace yuli::vault::ui {

UnlockView::UnlockView(IpcClient* client, QWidget* parent)
    : QWidget(parent), client_(client)
{
    // 模态全屏遮罩：覆盖父窗口整个区域
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Yuli Vault — Unlock"));

    // 自动覆盖父窗口大小
    if (parent) {
        setGeometry(parent->geometry());
    }

    build_ui();
}

UnlockView::~UnlockView() = default;

void UnlockView::build_ui() {
    // 根容器：垂直布局，整体居Medium
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // 半透明背景层（模拟遮罩）
    auto* overlay = new QFrame(this);
    overlay->setProperty("cssClass", QStringLiteral("modalOverlay"));
    auto* overlay_layout = new QVBoxLayout(overlay);
    overlay_layout->setContentsMargins(16, 16, 16, 16);
    overlay_layout->setAlignment(Qt::AlignCenter);

    // 卡片
    auto* card = new QFrame(overlay);
    card->setFixedWidth(380);
    card->setProperty("cssClass", QStringLiteral("modal"));
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(36, 36, 36, 28);
    card_layout->setSpacing(0);

    // 盾牌图标
    shield_icon_label_ = new QLabel(card);
    shield_icon_label_->setAlignment(Qt::AlignCenter);
    shield_icon_label_->setPixmap(
        QIcon(QStringLiteral(":/logo.png")).pixmap(QSize(56, 56)));
    card_layout->addWidget(shield_icon_label_);

    // 标题
    title_label_ = new QLabel(QStringLiteral("Yuli Vault"), card);
    title_label_->setAlignment(Qt::AlignCenter);
    title_label_->setProperty("cssClass", QStringLiteral("titleLg"));
    card_layout->addWidget(title_label_);

    // 副标题
    subtitle_label_ = new QLabel(
        tr("Enter the program password to unlock the vault"), card);
    subtitle_label_->setAlignment(Qt::AlignCenter);
    subtitle_label_->setProperty("cssClass", QStringLiteral("muted"));
    card_layout->addWidget(subtitle_label_);

    // Password输入框容器：lock 图标 + 输入框 + 可见性按钮 一体化
    auto* pwd_container = new QFrame(card);
    pwd_container->setFixedHeight(40);
    pwd_container->setProperty("cssClass", QStringLiteral("inputField"));
    auto* pwd_layout = new QHBoxLayout(pwd_container);
    pwd_layout->setContentsMargins(0, 0, 0, 0);
    pwd_layout->setSpacing(0);

    // lock 图标（输入框左侧，绝对位于容器内）
    auto* lock_icon = new QLabel(pwd_container);
    lock_icon->setPixmap(
        tinted_pixmap(QStringLiteral(":/icons/lock.svg"), IconRole::Normal, QSize(18, 18)));
    lock_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    lock_icon->setFixedSize(36, 40);
    lock_icon->setAlignment(Qt::AlignCenter);
    pwd_layout->addWidget(lock_icon);

    // 输入框（透明背景，无边框，融入容器）
    password_edit_ = new QLineEdit(pwd_container);
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setPlaceholderText(tr("Program password"));
    password_edit_->setProperty("cssClass", QStringLiteral("inlineEdit"));
    pwd_layout->addWidget(password_edit_, 1);

    // 可见性切换按钮（输入框右侧）
    visibility_btn_ = new QPushButton(pwd_container);
    visibility_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
    visibility_btn_->setIconSize(QSize(18, 18));
    visibility_btn_->setCursor(Qt::PointingHandCursor);
    visibility_btn_->setFixedSize(36, 40);
    visibility_btn_->setProperty("cssClass", QStringLiteral("inlineBtn"));
    pwd_layout->addWidget(visibility_btn_);

    card_layout->addWidget(pwd_container);
    card_layout->addSpacing(12);

    // 提示行：盾牌图标 + 提示文字 + 剩余尝试次数
    auto* hint_row = new QHBoxLayout();
    hint_row->setSpacing(6);

    auto* hint_icon = new QLabel(card);
    hint_icon->setPixmap(
        tinted_pixmap(QStringLiteral(":/icons/shield-check.svg"), IconRole::Normal, QSize(14, 14)));
    hint_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    hint_row->addWidget(hint_icon);

    auto* hint_text = new QLabel(
        tr("Five failed attempts lock the vault for 5 minutes"), card);
    hint_text->setProperty("cssClass", QStringLiteral("caption"));
    hint_row->addWidget(hint_text);
    hint_row->addStretch(1);

    attempts_label_ = new QLabel(
        tr("Attempts left 5/5"), card);
    attempts_label_->setProperty("cssClass", QStringLiteral("caption"));
    attempts_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hint_row->addWidget(attempts_label_);
    card_layout->addLayout(hint_row);

    // Unlock按钮
    submit_button_ = new QPushButton(card);
    submit_button_->setIcon(tinted_icon(QStringLiteral(":/icons/lock-open.svg"), IconRole::OnPrimary));
    submit_button_->setIconSize(QSize(18, 18));
    submit_button_->setText(tr("Unlock"));
    submit_button_->setCursor(Qt::PointingHandCursor);
    submit_button_->setFixedHeight(40);
    submit_button_->setProperty("cssClass", QStringLiteral("primary"));
    card_layout->addSpacing(20);
    card_layout->addWidget(submit_button_);

    // 错误提示
    error_label_ = new QLabel(card);
    error_label_->setWordWrap(true);
    error_label_->setProperty("cssClass", QStringLiteral("error"));
    error_label_->setAlignment(Qt::AlignCenter);
    card_layout->addWidget(error_label_);

    // 底部分隔线 + 加密信息
    card_layout->addSpacing(20);
    auto* divider = new QFrame(card);
    divider->setFixedHeight(1);
    divider->setProperty("cssClass", QStringLiteral("divider"));
    card_layout->addWidget(divider);

    auto* footer_row = new QHBoxLayout();
    footer_row->setSpacing(6);
    footer_row->setContentsMargins(0, 12, 0, 0);
    auto* footer_icon = new QLabel(card);
    footer_icon->setPixmap(
        tinted_pixmap(QStringLiteral(":/icons/shield.svg"), IconRole::Normal, QSize(12, 12)));
    footer_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    footer_row->addWidget(footer_icon);

    auto* footer_text = new QLabel(
        tr("Local encryption · AES-256-GCM · Argon2id"), card);
    footer_text->setProperty("cssClass", QStringLiteral("caption"));
    footer_row->addWidget(footer_text);
    footer_row->addStretch(1);
    card_layout->addLayout(footer_row);

    overlay_layout->addWidget(card);
    root_layout->addWidget(overlay);

    // 信号槽
    connect(visibility_btn_, &QPushButton::clicked,
            this, &UnlockView::on_visibility_toggled);
    connect(submit_button_, &QPushButton::clicked,
            this, &UnlockView::on_submit_clicked);
    connect(password_edit_, &QLineEdit::returnPressed,
            this, &UnlockView::on_submit_clicked);
    connect(password_edit_, &QLineEdit::textChanged,
            this, &UnlockView::on_password_changed);

    // 冷却倒计时定时器：每秒 tick，由 start_cooldown 启动
    cooldown_timer_ = new QTimer(this);
    cooldown_timer_->setInterval(1000);
    connect(cooldown_timer_, &QTimer::timeout,
            this, &UnlockView::on_cooldown_tick);

    password_edit_->setFocus();
}

void UnlockView::closeEvent(QCloseEvent* event) {
    if (!unlock_succeeded_) {
        emit rejected();
    }
    QWidget::closeEvent(event);
}

void UnlockView::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && !unlock_succeeded_) {
        emit rejected();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void UnlockView::on_visibility_toggled() {
    password_visible_ = !password_visible_;
    password_edit_->setEchoMode(
        password_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    visibility_btn_->setIcon(tinted_icon(password_visible_
        ? QStringLiteral(":/icons/eye-off.svg")
        : QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
}

void UnlockView::on_password_changed(const QString& text) {
    (void)text;
    // 清空错误提示
    if (error_label_) error_label_->setText(QString());
}

void UnlockView::on_submit_clicked() {
    set_error(QString());

    if (!client_) {
        set_error(tr("Internal error: IPC client is unavailable."));
        return;
    }

    const std::string password = password_edit_->text().toStdString();
    if (password.empty()) {
        set_error(tr("Program password cannot be empty."));
        return;
    }

    auto result = client_->unlock(password);
    if (result.ok() && result.value().success) {
        unlock_succeeded_ = true;
        emit unlock_succeeded();
    } else {
        QString msg;
        if (result.ok()) {
            // service 已响应（通常为 Unauthorized）：保留 service 返回的具体 message
            // （含"剩余 X 次尝试"/"请 X 秒后重试"等计数信息，由 parse_unlock_failure 解析）
            msg = QString::fromStdString(result.value().error_message);
        } else {
            // IPC 传输失败（IpcError/InternalError）：使用友好文案，技术细节进 qDebug
            msg = friendly_message(result.error());
        }

        // 优先从 service error_message 解析真实剩余次数 / 冷却秒数；
        // 解析失败（IPC 错误或 message 无可识别格式）回退本地递减，保留原行为
        const bool parsed = parse_unlock_failure(msg);
        if (!parsed) {
            --remaining_attempts_;
            update_attempts_display();
            if (remaining_attempts_ <= 0) {
                if (submit_button_) submit_button_->setEnabled(false);
                set_error(tr("No attempts left. Try again later."));
                password_edit_->clear();
                return;
            }
        }

        set_error(msg.isEmpty()
                      ? tr("Unlock failed. Check the program password.")
                      : tr("Unlock failed: %1").arg(msg));

        password_edit_->clear();
        password_edit_->setFocus();
    }
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

void UnlockView::set_error(const QString& message) {
    if (error_label_) error_label_->setText(message);
}

void UnlockView::update_attempts_display() {
    if (!attempts_label_) return;
    if (cooldown_remaining_seconds_ > 0) {
        // 冷却态：显示倒计时秒数（红色 error 样式）
        attempts_label_->setText(
            tr("Locked. Retry in %1 seconds")
                .arg(cooldown_remaining_seconds_));
        attempts_label_->setProperty("cssClass", QStringLiteral("error"));
    } else if (remaining_attempts_ > 0) {
        attempts_label_->setText(
            tr("Attempts left %1/5").arg(remaining_attempts_));
        attempts_label_->setProperty("cssClass", QStringLiteral("caption"));
    } else {
        attempts_label_->setText(tr("Locked"));
        attempts_label_->setProperty("cssClass", QStringLiteral("error"));
    }
    // 切换 dynamic property 后必须 unpolish + polish 才能让 QSS 重新生效
    attempts_label_->style()->unpolish(attempts_label_);
    attempts_label_->style()->polish(attempts_label_);
}

bool UnlockView::parse_unlock_failure(const QString& message) {
    // 冷却格式优先：匹配 "请 N 秒后重试"
    // （service 在 is_in_cooldown 命Medium与刚触发锁定两条路径均写入此子串）
    static const QRegularExpression cooldown_re(
        QStringLiteral("retry in (\\d+) seconds"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m1 = cooldown_re.match(message);
    if (m1.hasMatch()) {
        const int seconds = m1.captured(1).toInt();
        if (seconds > 0) {
            start_cooldown(seconds);
        } else {
            // service 报 0 秒：冷却刚结束，恢复初始剩余次数
            remaining_attempts_ = 5;
            update_attempts_display();
        }
        return true;
    }

    // 剩余次数格式：匹配 "剩余 N 次尝试"
    static const QRegularExpression attempts_re(
        QStringLiteral("(\\d+) attempts remaining"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m2 = attempts_re.match(message);
    if (m2.hasMatch()) {
        remaining_attempts_ = m2.captured(1).toInt();
        update_attempts_display();
        return true;
    }

    return false;
}

void UnlockView::start_cooldown(int seconds) {
    cooldown_remaining_seconds_ = seconds;
    if (submit_button_) submit_button_->setEnabled(false);
    if (password_edit_) password_edit_->setEnabled(false);
    update_attempts_display();
    if (cooldown_timer_ && !cooldown_timer_->isActive()) {
        cooldown_timer_->start();
    }
}

void UnlockView::on_cooldown_tick() {
    if (cooldown_remaining_seconds_ > 0) {
        --cooldown_remaining_seconds_;
    }
    if (cooldown_remaining_seconds_ <= 0) {
        if (cooldown_timer_) cooldown_timer_->stop();
        if (submit_button_) submit_button_->setEnabled(true);
        if (password_edit_) {
            password_edit_->setEnabled(true);
            password_edit_->setFocus();
        }
        // 冷却结束：恢复初始剩余次数（spec 要求"倒计时归零后恢复'Attempts left 5/5'"）
        remaining_attempts_ = 5;
        update_attempts_display();
    } else {
        update_attempts_display();
    }
}

}  // namespace yuli::vault::ui

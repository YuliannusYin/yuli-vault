// coding: utf-8
// =============================================================================
// InputView.cpp
//
// Yuli Vault New Item视图实现（新设计）。640px 居Medium卡片 + 图标前缀输入框 + Strong度条。
// =============================================================================
#include "InputView.h"
#include "ErrorMessages.h"
#include "IconKit.h"
#include "IpcClient.h"
#include "StrengthUtil.h"
#include "TagInputWidget.h"
#include "Theme.h"
#include "Toast.h"

#include <QApplication>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QTimer>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

namespace yuli::vault::ui {

namespace {

// Strong度等级判定与文案统一通过 StrengthUtil 提供（按 core::StrengthLevel 输入）。

/// 创建图标前缀的输入框容器：icon (36px) + QLineEdit + 右侧可选按钮区。
/// 容器自身有 border，输入框透明融入。
QFrame* make_icon_input_field(QWidget* parent, const QString& icon_path,
                               QLineEdit*& out_edit,
                               QPushButton* right_btn1 = nullptr,
                               QPushButton* right_btn2 = nullptr) {
    auto* container = new QFrame(parent);
    container->setFixedHeight(40);
    container->setProperty("cssClass", QStringLiteral("inputField"));
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* icon = new QLabel(container);
    icon->setPixmap(tinted_pixmap(icon_path, IconRole::Normal, QSize(18, 18)));
    icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    icon->setFixedSize(36, 40);
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon);

    out_edit = new QLineEdit(container);
    out_edit->setProperty("cssClass", QStringLiteral("inlineEdit"));
    layout->addWidget(out_edit, 1);

    if (right_btn1) {
        layout->addWidget(right_btn1);
    }
    if (right_btn2) {
        layout->addWidget(right_btn2);
    }
    return container;
}

}  // namespace

InputView::InputView(IpcClient* client, QWidget* parent)
    : QWidget(parent), client_(client)
{
    build_ui();
}

InputView::~InputView() = default;

void InputView::build_ui() {
    // 外层用滚动区域，确保小屏幕下表单可滚动
    auto* outer_layout = new QVBoxLayout(this);
    outer_layout->setContentsMargins(0, 0, 0, 0);
    outer_layout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* scroll_content = new QWidget(scroll);
    // 不设 setStyleSheet("background: transparent;")：widget 级样式表优先级
    // 高于 qss 文件，无选择器的 "background: transparent" 会级联到所有子 widget，
    // 覆盖 card 的 background-color。content 继承全局 QWidget 底色即可。
    auto* scroll_layout = new QVBoxLayout(scroll_content);
    scroll_layout->setContentsMargins(24, 16, 24, 16);
    scroll_layout->setSpacing(0);
    scroll_layout->addStretch(1);  // 顶部弹性，让卡片垂直居Medium

    // ── 640px 居Medium卡片 ──
    auto* card = new QFrame(scroll_content);
    card->setFixedWidth(640);
    card->setObjectName(QStringLiteral("formCard"));
    card->setProperty("cssClass", QStringLiteral("card"));
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(32, 32, 32, 32);
    card_layout->setSpacing(0);

    // 标题
    auto* title = new QLabel(tr("New login"), card);
    title->setProperty("cssClass", QStringLiteral("sectionTitle"));
    card_layout->addWidget(title);

    auto* subtitle = new QLabel(
        tr("Sensitive fields are encrypted locally"), card);
    subtitle->setProperty("cssClass", QStringLiteral("muted"));
    card_layout->addWidget(subtitle);

    card_layout->addSpacing(24);

    // ── *Title（必填） ──
    // 必填红星颜色按Theme动态决定（QSS 选择器对 QLabel 富文本 span 不生效）
    const QString danger_color = Theme::is_dark()
        ? QStringLiteral("#f56363") : QStringLiteral("#dc2626");
    auto* entry_name_label = new QLabel(
        tr("Title <span style=\"color:%1;\">*</span>").arg(danger_color), card);
    entry_name_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    entry_name_label->setTextFormat(Qt::RichText);
    card_layout->addWidget(entry_name_label);
    card_layout->addSpacing(6);
    auto* entry_name_field = make_icon_input_field(
        card, QStringLiteral(":/icons/database.svg"), entry_name_edit_);
    card_layout->addWidget(entry_name_field);
    entry_name_edit_->setPlaceholderText(tr("e.g. GitHub, work email"));

    card_layout->addSpacing(16);

    // ── Username（可选） ──
    auto* username_label = new QLabel(tr("Username"), card);
    username_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(username_label);
    card_layout->addSpacing(6);
    auto* username_field = make_icon_input_field(
        card, QStringLiteral(":/icons/user.svg"), username_edit_);
    card_layout->addWidget(username_field);
    username_edit_->setPlaceholderText(tr("Display name, e.g. Alice"));

    card_layout->addSpacing(16);

    // ── *Account（必填） ──
    auto* account_label = new QLabel(
        tr("Account <span style=\"color:%1;\">*</span>").arg(danger_color), card);
    account_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    account_label->setTextFormat(Qt::RichText);
    card_layout->addWidget(account_label);
    card_layout->addSpacing(6);
    auto* account_field = make_icon_input_field(
        card, QStringLiteral(":/icons/at-sign.svg"), account_edit_);
    card_layout->addWidget(account_field);
    account_edit_->setPlaceholderText(tr("Login ID or email"));

    card_layout->addSpacing(16);

    // ── *Password（必填，带Generate + 可见性按钮） ──
    auto* pwd_label = new QLabel(
        tr("Password <span style=\"color:%1;\">*</span>").arg(danger_color), card);
    pwd_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    pwd_label->setTextFormat(Qt::RichText);
    card_layout->addWidget(pwd_label);
    card_layout->addSpacing(6);

    // Generate按钮（小尺寸，融入输入框右侧）
    generate_button_ = new QPushButton(card);
    generate_button_->setIcon(tinted_icon(QStringLiteral(":/icons/wand-2.svg"), IconRole::Normal));
    generate_button_->setIconSize(QSize(14, 14));
    generate_button_->setText(tr("Generate"));
    generate_button_->setCursor(Qt::PointingHandCursor);
    generate_button_->setFixedHeight(40);
    generate_button_->setProperty("cssClass", QStringLiteral("inlineTextBtn"));

    // 可见性按钮
    visibility_btn_ = new QPushButton(card);
    visibility_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
    visibility_btn_->setIconSize(QSize(16, 16));
    visibility_btn_->setCursor(Qt::PointingHandCursor);
    visibility_btn_->setFixedSize(36, 40);
    visibility_btn_->setProperty("cssClass", QStringLiteral("inlineBtn"));

    auto* pwd_field = make_icon_input_field(
        card, QStringLiteral(":/icons/key.svg"), password_edit_,
        generate_button_, visibility_btn_);
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setProperty("cssClass", QStringLiteral("inlineEdit"));
    card_layout->addWidget(pwd_field);

    // Strong度条
    auto* strength_row = new QHBoxLayout();
    strength_row->setContentsMargins(0, 8, 0, 0);
    strength_row->setSpacing(8);
    strength_bar_ = new QProgressBar(card);
    strength_bar_->setRange(0, 100);
    strength_bar_->setValue(0);
    strength_bar_->setTextVisible(false);
    strength_bar_->setFixedHeight(4);
    strength_bar_->setProperty("strength", QStringLiteral("weak"));
    strength_row->addWidget(strength_bar_, 1);
    strength_label_ = new QLabel(tr("Strength: —"), card);
    strength_label_->setProperty("cssClass", QStringLiteral("caption"));
    strength_row->addWidget(strength_label_);
    card_layout->addLayout(strength_row);

    card_layout->addSpacing(16);

    // ── Website（可选） ──
    auto* website_label = new QLabel(tr("Website"), card);
    website_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(website_label);
    card_layout->addSpacing(6);
    auto* website_field = make_icon_input_field(
        card, QStringLiteral(":/icons/globe.svg"), website_edit_);
    card_layout->addWidget(website_field);
    website_edit_->setPlaceholderText(tr("example.com"));

    card_layout->addSpacing(16);

    // ── Tags（可选，芯片流式输入） ──
    auto* tag_label = new QLabel(tr("Tags"), card);
    tag_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(tag_label);
    card_layout->addSpacing(6);
    // Tags输入容器：左侧 tag 图标 + TagInputWidget（融入 inputField 一体化边框）
    auto* tag_field = new QFrame(card);
    tag_field->setProperty("cssClass", QStringLiteral("inputField"));
    auto* tag_field_layout = new QHBoxLayout(tag_field);
    tag_field_layout->setContentsMargins(0, 0, 0, 0);
    tag_field_layout->setSpacing(0);

    auto* tag_icon = new QLabel(tag_field);
    tag_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/tag.svg"), IconRole::Normal, QSize(18, 18)));
    tag_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    tag_icon->setFixedSize(36, 40);
    tag_icon->setAlignment(Qt::AlignCenter);
    tag_field_layout->addWidget(tag_icon, 0, Qt::AlignVCenter);

    tag_input_ = new TagInputWidget(tag_field);
    tag_input_->setProperty("cssClass", QStringLiteral("tagInput"));
    tag_input_->setMinimumHeight(40);
    tag_field_layout->addWidget(tag_input_, 1);
    card_layout->addWidget(tag_field);

    card_layout->addSpacing(16);

    // ── Notes（可选，markdown 源码） ──
    auto* note_label = new QLabel(tr("Notes (Markdown)"), card);
    note_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(note_label);
    card_layout->addSpacing(6);
    note_edit_ = new QPlainTextEdit(card);
    note_edit_->setPlaceholderText(
        tr("Optional. Markdown is supported (# headings, **bold**, `code`, - lists)."));
    // Notes框可随内容增长到 240px，超出后内部滚动，避免与小窗口内滚动冲突
    note_edit_->setMinimumHeight(96);
    note_edit_->setMaximumHeight(240);
    card_layout->addWidget(note_edit_);

    // 错误提示
    card_layout->addSpacing(12);
    error_label_ = new QLabel(card);
    error_label_->setWordWrap(true);
    error_label_->setProperty("cssClass", QStringLiteral("error"));
    error_label_->setVisible(false);
    card_layout->addWidget(error_label_);

    // 底部分隔线 + Actions按钮
    card_layout->addSpacing(20);
    auto* divider = new QFrame(card);
    divider->setFixedHeight(1);
    divider->setProperty("cssClass", QStringLiteral("divider"));
    card_layout->addWidget(divider);
    card_layout->addSpacing(16);

    auto* footer_row = new QHBoxLayout();
    footer_row->setContentsMargins(0, 0, 0, 0);
    footer_row->setSpacing(12);
    footer_row->addStretch(1);

    clear_button_ = new QPushButton(card);
    clear_button_->setIcon(tinted_icon(QStringLiteral(":/icons/eraser.svg"), IconRole::Normal));
    clear_button_->setIconSize(QSize(16, 16));
    clear_button_->setText(tr("Clear"));
    clear_button_->setCursor(Qt::PointingHandCursor);
    clear_button_->setFixedHeight(40);
    clear_button_->setProperty("cssClass", QStringLiteral("outline"));
    footer_row->addWidget(clear_button_);

    save_button_ = new QPushButton(card);
    save_button_->setIcon(tinted_icon(QStringLiteral(":/icons/save.svg"), IconRole::OnPrimary));
    save_button_->setIconSize(QSize(16, 16));
    save_button_->setText(tr("Save item"));
    save_button_->setCursor(Qt::PointingHandCursor);
    save_button_->setFixedHeight(40);
    save_button_->setProperty("cssClass", QStringLiteral("primary"));
    footer_row->addWidget(save_button_);
    card_layout->addLayout(footer_row);

    scroll_layout->addWidget(card, 0, Qt::AlignCenter);
    scroll_layout->addStretch(1);  // 底部弹性

    scroll->setWidget(scroll_content);
    outer_layout->addWidget(scroll);

    // 信号槽
    connect(generate_button_, &QPushButton::clicked,
            this, &InputView::on_generate_clicked);
    connect(visibility_btn_, &QPushButton::clicked,
            this, &InputView::on_toggle_password_clicked);
    connect(password_edit_, &QLineEdit::textChanged,
            this, &InputView::on_password_changed);
    connect(save_button_, &QPushButton::clicked,
            this, &InputView::on_save_clicked);
    // Password字段按 Enter 直接触发保存；其余字段保持 Qt 默认 focusNextChild 跳转
    connect(password_edit_, &QLineEdit::returnPressed,
            this, &InputView::on_save_clicked);
    connect(clear_button_, &QPushButton::clicked,
            this, &InputView::on_clear_clicked);

    // Strong度评估 debounce：每次输入触发 IPC 会卡顿，用 300ms 计时器合并连续输入。
    strength_timer_ = new QTimer(this);
    strength_timer_->setSingleShot(true);
    connect(strength_timer_, &QTimer::timeout, this, [this]() {
        // 异步发起Strong度评估：QtConcurrent 线程池执行 IPC，finished 回主线程更新 UI。
        if (!client_ || pending_password_.isEmpty()) {
            update_strength_ui(core::StrengthEstimate{});
            return;
        }
        const std::string pwd = pending_password_.toStdString();
        auto* watcher = new QFutureWatcher<core::Result<protocol::EstimateStrengthResponse>>(this);
        connect(watcher, &QFutureWatcher<core::Result<protocol::EstimateStrengthResponse>>::finished,
                this, [this, watcher]() {
            auto r = watcher->result();
            if (r.ok()) {
                update_strength_ui(r.value().estimate);
            }
            // 失败静默：Strong度条保持上一次状态，不打扰用户输入
            watcher->deleteLater();
        });
        watcher->setFuture(client_->estimate_strength_async(pwd));
    });

    // Tags补全列表改由 showEvent 首次显示时异步加载（避免构造期同步 IPC）
}

void InputView::set_password(const QString& password) {
    if (password_edit_) password_edit_->setText(password);
}

void InputView::focus_first_field() {
    if (entry_name_edit_) entry_name_edit_->setFocus();
}

void InputView::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Task 28：首次显示时异步加载Tags补全列表，避免重复加载
    if (!tags_loaded_) {
        tags_loaded_ = true;
        refresh_existing_tags_async();
    }
}

void InputView::on_generate_clicked() {
    // 通知 MainWindow 切换到Generator视图；Generate后由 MainWindow 调用 set_password 回填
    emit password_generator_requested();
}

void InputView::on_toggle_password_clicked() {
    password_visible_ = !password_visible_;
    password_edit_->setEchoMode(
        password_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    visibility_btn_->setIcon(tinted_icon(password_visible_
        ? QStringLiteral(":/icons/eye-off.svg")
        : QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
}

void InputView::on_password_changed(const QString& text) {
    // 捕获当前文本，debounce 计时器 timeout 时使用；300ms 内无新输入才发起 IPC
    pending_password_ = text;
    if (strength_timer_) {
        if (text.isEmpty()) {
            // 立即清空Strong度 UI，避免空Password还显示上一次的Strong度
            strength_timer_->stop();
            update_strength_ui(core::StrengthEstimate{});
        } else {
            strength_timer_->start(300);
        }
    }
}

void InputView::update_strength(const QString& password) {
    // 入口：保留同步签名以兼容旧调用点。实际异步流程通过 on_password_changed
    // + strength_timer_ timeout 处理。这里仅同步Refresh UI 状态（空Password立即清空）。
    pending_password_ = password;
    if (password.isEmpty()) {
        update_strength_ui(core::StrengthEstimate{});
    } else if (strength_timer_) {
        strength_timer_->start(300);
    }
}

void InputView::update_strength_ui(const core::StrengthEstimate& estimate) {
    if (!strength_bar_ || !strength_label_) return;

    // 空Password：重置为初始状态（「Strength: —」），不展示评估结果
    if (pending_password_.isEmpty()) {
        strength_bar_->setValue(0);
        strength_bar_->setProperty("strength", QStringLiteral("weak"));
        strength_bar_->style()->unpolish(strength_bar_);
        strength_bar_->style()->polish(strength_bar_);
        strength_label_->setText(tr("Strength: —"));
        strength_label_->setProperty("cssClass", QStringLiteral("caption"));
        strength_label_->style()->unpolish(strength_label_);
        strength_label_->style()->polish(strength_label_);
        return;
    }

    const int pct = (estimate.bits >= 128) ? 100 : (estimate.bits * 100 / 128);
    strength_bar_->setValue(pct);

    // 通过 dynamic property 让 QSS 接管 chunk 颜色（深浅Theme适配）
    const QString strength_key = strength_qss_key(estimate.level);
    const QString label_class = strength_label_class(estimate.level);
    strength_bar_->setProperty("strength", strength_key);
    strength_bar_->style()->unpolish(strength_bar_);
    strength_bar_->style()->polish(strength_bar_);

    strength_label_->setProperty("cssClass", label_class);
    strength_label_->style()->unpolish(strength_label_);
    strength_label_->style()->polish(strength_label_);
    strength_label_->setText(
        tr("Strength: %1 (%2 bits)").arg(strength_text(estimate.level)).arg(estimate.bits));
}

void InputView::on_save_clicked() {
    if (saving_) return;  // 防重复点击
    set_error(QString());

    if (!client_) {
        set_error(tr("Internal error: IPC client is unavailable."));
        return;
    }

    const QString entry_name = entry_name_edit_->text().trimmed();
    const QString account = account_edit_->text().trimmed();
    const QString password = password_edit_->text();

    // 必填校验：entry_name / account / password
    if (entry_name.isEmpty()) {
        set_error(tr("Title cannot be empty."));
        entry_name_edit_->setFocus();
        return;
    }
    if (account.isEmpty()) {
        set_error(tr("Account cannot be empty."));
        account_edit_->setFocus();
        return;
    }
    if (password.isEmpty()) {
        set_error(tr("Password cannot be empty."));
        password_edit_->setFocus();
        return;
    }

    core::PasswordEntry entry;
    entry.id = 0;
    entry.type = core::VaultItemType::Login;
    entry.title = entry_name.toStdString();
    entry.account = account.toStdString();
    entry.username = username_edit_->text().trimmed().toStdString();
    entry.password = password.toStdString();
    entry.website = website_edit_->text().trimmed().toStdString();
    entry.note = note_edit_->toPlainText().toStdString();
    if (tag_input_) {
        entry.tags = tag_input_->selected_tags();
    }

    // 异步保存：Disable按钮 + 文案改为「Saving…」，回调Medium恢复
    saving_ = true;
    save_button_->setEnabled(false);
    save_button_->setText(tr("Saving…"));

    auto* watcher = new QFutureWatcher<core::Result<protocol::AddEntryResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::AddEntryResponse>>::finished,
            this, [this, watcher]() {
        saving_ = false;
        save_button_->setEnabled(true);
        save_button_->setText(tr("Save item"));

        auto result = watcher->result();
        if (result.ok()) {
            // Task 18.1：用 Toast 替代 QMessageBox::information，非阻塞反馈
            Toast::show(this, tr("Login saved"));
            const int64_t new_id = result.value().entry.id;
            emit entry_added(new_id);
            on_clear_clicked();
        } else {
            set_error(friendly_message(result.error()));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->add_entry_async(entry));
}

void InputView::on_clear_clicked() {
    entry_name_edit_->clear();
    account_edit_->clear();
    username_edit_->clear();
    password_edit_->clear();
    website_edit_->clear();
    if (tag_input_) tag_input_->set_selected_tags({});
    note_edit_->clear();
    if (password_visible_) {
        on_toggle_password_clicked();
    }
    set_error(QString());
    entry_name_edit_->setFocus();
}

void InputView::set_error(const QString& message) {
    if (!error_label_) return;
    error_label_->setText(message);
    error_label_->setVisible(!message.isEmpty());
}

void InputView::refresh_existing_tags() {
    // 同步入口（向后兼容）：实际由 async Version执行 IPC
    refresh_existing_tags_async();
}

void InputView::refresh_existing_tags_async() {
    if (!client_ || !tag_input_) return;
    auto* watcher = new QFutureWatcher<core::Result<protocol::ListTagsResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::ListTagsResponse>>::finished,
            this, [this, watcher]() {
        auto result = watcher->result();
        if (result.ok() && tag_input_) {
            tag_input_->set_existing_tags(result.value().tags);
        }
        // 失败静默：补全列表不可用不影响主流程
        watcher->deleteLater();
    });
    watcher->setFuture(client_->list_tags_async());
}

}  // namespace yuli::vault::ui

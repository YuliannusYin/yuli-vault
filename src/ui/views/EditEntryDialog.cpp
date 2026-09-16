// coding: utf-8
// =============================================================================
// EditEntryDialog.cpp
//
// Yuli Vault Edit条目对话框实现（新设计）。
// 模态遮罩 + 560px 居Medium卡片 + 头部/表单/尾部三段式布局。
// =============================================================================
#include "EditEntryDialog.h"
#include "ErrorMessages.h"
#include "IpcClient.h"
#include "IconKit.h"
#include "StrengthUtil.h"
#include "TagInputWidget.h"
#include "Theme.h"
#include "Toast.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QString>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace yuli::vault::ui {

namespace {

QString format_time(int64_t ts) {
    if (ts <= 0) return QStringLiteral("-");
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ts))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

// Strong度等级判定与文案统一通过 StrengthUtil 提供（按 core::StrengthLevel 输入）。

}  // namespace

EditEntryDialog::EditEntryDialog(IpcClient* client, const core::PasswordEntry& entry,
                                 QWidget* parent)
    : QDialog(parent), client_(client), entry_(entry)
{
    // 模态 + 无原生标题栏，自定义遮罩 + 卡片
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Edit login"));

    // 自动覆盖父窗口大小
    // parent 通常是 PasswordBookView（MainWindow 的子 widget，非 top-level），
    // 其 geometry() 返回的是相对于父 widget 的坐标，不能直接用于 top-level QDialog。
    // 用 window()->geometry() 拿到顶层主窗口的屏幕坐标。
    if (parent) {
        setGeometry(parent->window()->geometry());
    }

    build_ui();
}

EditEntryDialog::~EditEntryDialog() = default;

void EditEntryDialog::build_ui() {
    // 根布局：半透明遮罩
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* overlay = new QFrame(this);
    overlay->setProperty("cssClass", QStringLiteral("modalOverlay"));
    auto* overlay_layout = new QVBoxLayout(overlay);
    overlay_layout->setContentsMargins(16, 16, 16, 16);
    overlay_layout->setAlignment(Qt::AlignCenter);

    // ── 560px 卡片 ──
    // maximumHeight 防止内容多时卡片超出 overlay 可视区（overlay 可用高度 ≈ 主窗口高 - 32 margin）；
    // body 内嵌 QScrollArea，内容超出时内部滚动，卡片不溢出。
    auto* card = new QFrame(overlay);
    card->setFixedWidth(560);
    card->setMaximumHeight(760);  // 800(主窗口) - 32(overlay margin×2) - 8(余量)
    card->setProperty("cssClass", QStringLiteral("modal"));
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(0, 0, 0, 0);
    card_layout->setSpacing(0);

    // ── 头部（56px 高） ──
    auto* header = new QFrame(card);
    header->setFixedHeight(56);
    header->setProperty("cssClass", QStringLiteral("modalHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 0, 16, 0);
    header_layout->setSpacing(10);

    auto* pencil_icon = new QLabel(header);
    pencil_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/pencil.svg"), IconRole::Normal, QSize(18, 18)));
    pencil_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    header_layout->addWidget(pencil_icon);

    auto* title = new QLabel(tr("Edit login"), header);
    title->setProperty("cssClass", QStringLiteral("sectionTitle"));
    header_layout->addWidget(title);
    header_layout->addStretch(1);

    auto* close_btn = new QPushButton(header);
    close_btn->setIcon(tinted_icon(QStringLiteral(":/icons/x.svg"), IconRole::Normal));
    close_btn->setIconSize(QSize(18, 18));
    close_btn->setCursor(Qt::PointingHandCursor);
    close_btn->setFixedSize(36, 36);
    close_btn->setProperty("cssClass", QStringLiteral("closeBtn"));
    header_layout->addWidget(close_btn);
    card_layout->addWidget(header);

    // ── 体（表单） ──
    // body 包在 QScrollArea 内：内容超出卡片可视区时内部滚动，
    // 卡片本身受 maximumHeight 约束不溢出 overlay。
    auto* body_scroll = new QScrollArea(card);
    body_scroll->setWidgetResizable(true);
    body_scroll->setFrameShape(QFrame::NoFrame);
    body_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 透明背景由全局 QScrollArea QSS 规则接管，无需 setStyleSheet

    auto* body = new QFrame(body_scroll);
    body->setProperty("cssClass", QStringLiteral("modalBody"));
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 20, 24, 20);
    body_layout->setSpacing(16);

    // 字段Tags样式（支持必填 * 标记）
    auto make_label = [body](const QString& text) {
        auto* lbl = new QLabel(text, body);
        lbl->setProperty("cssClass", QStringLiteral("fieldLabel"));
        lbl->setTextFormat(Qt::RichText);
        return lbl;
    };

    // 普通输入框（inputField 容器 + 图标 + 透明 QLineEdit，与Password框风格统一）
    auto make_line_edit = [body](QLineEdit*& edit, const QString& icon_path) {
        auto* container = new QFrame(body);
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
        edit = new QLineEdit(container);
        edit->setProperty("cssClass", QStringLiteral("inlineEdit"));
        layout->addWidget(edit, 1);
        return container;
    };

    // ── *Title（必填） ──
    // 必填红星颜色按Theme动态决定（QSS 选择器对 QLabel 富文本 span 不生效）
    const QString danger_color = Theme::is_dark()
        ? QStringLiteral("#f56363") : QStringLiteral("#dc2626");
    body_layout->addWidget(make_label(
        tr("Title <span style=\"color:%1;\">*</span>").arg(danger_color)));
    body_layout->addWidget(make_line_edit(entry_name_edit_,
        QStringLiteral(":/icons/database.svg")));
    entry_name_edit_->setText(QString::fromStdString(entry_.title));
    entry_name_edit_->setPlaceholderText(tr("e.g. GitHub personal"));

    // ── Username（可选） ──
    body_layout->addWidget(make_label(tr("Username")));
    body_layout->addWidget(make_line_edit(username_edit_,
        QStringLiteral(":/icons/at-sign.svg")));
    username_edit_->setText(QString::fromStdString(entry_.username));
    username_edit_->setPlaceholderText(tr("Display name, e.g. Alice"));

    // ── *Account（必填） ──
    body_layout->addWidget(make_label(
        tr("Account <span style=\"color:%1;\">*</span>").arg(danger_color)));
    body_layout->addWidget(make_line_edit(account_edit_,
        QStringLiteral(":/icons/at-sign.svg")));
    account_edit_->setText(QString::fromStdString(entry_.account));
    account_edit_->setPlaceholderText(tr("Login ID or email"));

    // ── *Password（必填，带可见性 + Generate） ──
    body_layout->addWidget(make_label(
        tr("Password <span style=\"color:%1;\">*</span>").arg(danger_color)));
    auto* pwd_container = new QFrame(body);
    pwd_container->setFixedHeight(40);
    pwd_container->setProperty("cssClass", QStringLiteral("inputField"));
    auto* pwd_layout = new QHBoxLayout(pwd_container);
    pwd_layout->setContentsMargins(0, 0, 0, 0);
    pwd_layout->setSpacing(0);

    auto* key_icon = new QLabel(pwd_container);
    key_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/key-round.svg"), IconRole::Normal, QSize(16, 16)));
    key_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    key_icon->setFixedSize(36, 40);
    key_icon->setAlignment(Qt::AlignCenter);
    pwd_layout->addWidget(key_icon);

    password_edit_ = new QLineEdit(pwd_container);
    password_edit_->setEchoMode(QLineEdit::Password);
    password_edit_->setText(QString::fromStdString(entry_.password));
    password_edit_->setProperty("cssClass", QStringLiteral("inlineEdit"));
    pwd_layout->addWidget(password_edit_, 1);

    // Generate按钮
    generate_btn_ = new QPushButton(pwd_container);
    generate_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/wand-2.svg"), IconRole::Normal));
    generate_btn_->setIconSize(QSize(16, 16));
    generate_btn_->setCursor(Qt::PointingHandCursor);
    generate_btn_->setFixedSize(40, 40);
    generate_btn_->setToolTip(tr("Generate password"));
    generate_btn_->setProperty("cssClass", QStringLiteral("inlineBtn"));
    pwd_layout->addWidget(generate_btn_);

    // 可见性按钮
    visibility_btn_ = new QPushButton(pwd_container);
    visibility_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
    visibility_btn_->setIconSize(QSize(16, 16));
    visibility_btn_->setCursor(Qt::PointingHandCursor);
    visibility_btn_->setFixedSize(40, 40);
    visibility_btn_->setToolTip(tr("Show/hide password"));
    visibility_btn_->setProperty("cssClass", QStringLiteral("inlineBtn"));
    pwd_layout->addWidget(visibility_btn_);
    body_layout->addWidget(pwd_container);

    // Strong度行
    auto* strength_row = new QHBoxLayout();
    strength_row->setContentsMargins(0, 4, 0, 0);
    strength_row->setSpacing(8);
    strength_label_ = new QLabel(body);
    strength_label_->setProperty("cssClass", QStringLiteral("caption"));
    strength_row->addWidget(strength_label_);
    strength_row->addStretch(1);
    body_layout->addLayout(strength_row);

    // ── Website（可选） ──
    body_layout->addWidget(make_label(tr("Website")));
    body_layout->addWidget(make_line_edit(website_edit_,
        QStringLiteral(":/icons/globe.svg")));
    website_edit_->setText(QString::fromStdString(entry_.website));
    website_edit_->setPlaceholderText(tr("example.com"));

    // ── Tags（可选，芯片流式输入） ──
    body_layout->addWidget(make_label(tr("Tags")));
    tag_input_ = new TagInputWidget(body);
    tag_input_->setProperty("cssClass", QStringLiteral("tagInput"));
    body_layout->addWidget(tag_input_);
    // 预填已有Tags
    tag_input_->set_selected_tags(entry_.tags);
    // Tags补全列表改由 showEvent 首次显示时异步加载（避免构造期同步 IPC）

    // ── Notes（可选，markdown 源码） ──
    body_layout->addWidget(make_label(tr("Notes (Markdown)")));
    note_edit_ = new QPlainTextEdit(body);
    note_edit_->setPlainText(QString::fromStdString(entry_.note));
    note_edit_->setFixedHeight(96);
    note_edit_->setPlaceholderText(
        tr("Optional. Markdown is supported (# headings, **bold**, `code`, - lists)."));
    body_layout->addWidget(note_edit_);

    // 更新Time行
    auto* updated_row = new QHBoxLayout();
    updated_row->setContentsMargins(0, 4, 0, 0);
    updated_row->setSpacing(6);
    auto* clock_icon = new QLabel(body);
    clock_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/clock.svg"), IconRole::Normal, QSize(14, 14)));
    clock_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    updated_row->addWidget(clock_icon);
    updated_label_ = new QLabel(
        tr("Updated %1").arg(format_time(entry_.updated_at)), body);
    updated_label_->setProperty("cssClass", QStringLiteral("caption"));
    updated_row->addWidget(updated_label_);
    updated_row->addStretch(1);
    body_layout->addLayout(updated_row);

    // 错误提示
    error_label_ = new QLabel(body);
    error_label_->setWordWrap(true);
    error_label_->setProperty("cssClass", QStringLiteral("error"));
    error_label_->setVisible(false);
    body_layout->addWidget(error_label_);

    // body 的 sizeHint 需在 setWidget 前完整构建，QScrollArea 据此决定是否显示滚动条
    body_scroll->setWidget(body);
    // stretch=1 让 body_scroll 占据 header 与 footer 之间的全部可用高度
    card_layout->addWidget(body_scroll, 1);

    // ── 尾部（64px 高） ──
    auto* footer = new QFrame(card);
    footer->setFixedHeight(64);
    footer->setProperty("cssClass", QStringLiteral("modalFooter"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(24, 0, 24, 0);
    footer_layout->setSpacing(12);
    footer_layout->addStretch(1);

    cancel_button_ = new QPushButton(tr("Cancel"), footer);
    cancel_button_->setCursor(Qt::PointingHandCursor);
    cancel_button_->setFixedHeight(40);
    cancel_button_->setProperty("cssClass", QStringLiteral("outline"));
    footer_layout->addWidget(cancel_button_);

    save_button_ = new QPushButton(footer);
    save_button_->setIcon(tinted_icon(QStringLiteral(":/icons/save.svg"), IconRole::OnPrimary));
    save_button_->setIconSize(QSize(16, 16));
    save_button_->setText(tr("Save changes"));
    save_button_->setCursor(Qt::PointingHandCursor);
    save_button_->setFixedHeight(40);
    save_button_->setProperty("cssClass", QStringLiteral("primary"));
    footer_layout->addWidget(save_button_);
    card_layout->addWidget(footer);

    overlay_layout->addWidget(card);
    root_layout->addWidget(overlay);

    // 信号槽
    connect(close_btn, &QPushButton::clicked,
            this, &EditEntryDialog::on_cancel_clicked);
    connect(cancel_button_, &QPushButton::clicked,
            this, &EditEntryDialog::on_cancel_clicked);
    connect(save_button_, &QPushButton::clicked,
            this, &EditEntryDialog::on_save_clicked);
    // Password字段按 Enter 直接触发保存；其余字段保持 Qt 默认 focusNextChild 跳转。
    // 不重写 keyPressEvent 拦截 Enter，避免影响 QDialog 默认行为
    connect(password_edit_, &QLineEdit::returnPressed,
            this, &EditEntryDialog::on_save_clicked);
    connect(visibility_btn_, &QPushButton::clicked,
            this, &EditEntryDialog::on_toggle_password_clicked);
    connect(generate_btn_, &QPushButton::clicked,
            this, &EditEntryDialog::on_generate_clicked);
    connect(password_edit_, &QLineEdit::textChanged,
            this, &EditEntryDialog::on_password_changed);

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
            // 失败静默：Strong度文案保持上一次状态，不打扰用户输入
            watcher->deleteLater();
        });
        watcher->setFuture(client_->estimate_strength_async(pwd));
    });

    // 初始Strong度：通过 update_strength 入口Settings pending_password_ 并启动 debounce
    update_strength(QString::fromStdString(entry_.password));
    entry_name_edit_->setFocus();
}

void EditEntryDialog::closeEvent(QCloseEvent* event) {
    // Close = Cancel
    reject();
    QDialog::closeEvent(event);
}

void EditEntryDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Task 28：首次显示时异步加载Tags补全列表，避免重复加载
    if (!tags_loaded_) {
        tags_loaded_ = true;
        refresh_existing_tags_async();
    }
}

void EditEntryDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        on_cancel_clicked();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void EditEntryDialog::on_toggle_password_clicked() {
    password_visible_ = !password_visible_;
    password_edit_->setEchoMode(
        password_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    visibility_btn_->setIcon(tinted_icon(password_visible_
        ? QStringLiteral(":/icons/eye-off.svg")
        : QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
}

void EditEntryDialog::on_generate_clicked() {
    if (!client_) return;
    // 使用默认参数Generate
    core::PasswordGeneratorOptions opts;
    opts.length = 20;
    opts.use_uppercase = true;
    opts.use_lowercase = true;
    opts.use_digits = true;
    opts.use_symbols = true;
    auto r = client_->generate_password(opts);
    if (r.ok()) {
        password_edit_->setText(QString::fromStdString(r.value().password));
    } else {
        QMessageBox::warning(this, tr("Generation failed"),
            tr("Could not generate a password: %1")
                .arg(QString::fromStdString(r.error().what())));
    }
}

void EditEntryDialog::on_password_changed(const QString& text) {
    // 捕获当前文本，debounce 计时器 timeout 时使用；300ms 内无新输入才发起 IPC
    pending_password_ = text;
    if (strength_timer_) {
        if (text.isEmpty()) {
            // 立即清空Strong度文案，避免空Password还显示上一次的Strong度
            strength_timer_->stop();
            update_strength_ui(core::StrengthEstimate{});
        } else {
            strength_timer_->start(300);
        }
    }
}

void EditEntryDialog::on_save_clicked() {
    if (saving_) return;  // 防重复点击
    set_error(QString());

    if (!client_) {
        set_error(tr("Internal error: IPC client is unavailable."));
        return;
    }

    const QString entry_name = entry_name_edit_->text().trimmed();
    const QString account = account_edit_->text().trimmed();
    const QString username = username_edit_->text().trimmed();
    const QString website = website_edit_->text().trimmed();
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

    core::PasswordEntry updated = entry_;
    updated.title = entry_name.toStdString();
    updated.account = account.toStdString();
    updated.username = username.toStdString();
    updated.password = password.toStdString();
    updated.website = website.toStdString();
    updated.note = note_edit_->toPlainText().toStdString();
    if (tag_input_) {
        updated.tags = tag_input_->selected_tags();
    }

    // 异步保存：Disable按钮 + 文案改为「Saving…」，回调Medium恢复
    saving_ = true;
    save_button_->setEnabled(false);
    save_button_->setText(tr("Saving…"));

    auto* watcher = new QFutureWatcher<core::Result<protocol::UpdateEntryResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::UpdateEntryResponse>>::finished,
            this, [this, watcher]() {
        saving_ = false;
        save_button_->setEnabled(true);
        save_button_->setText(tr("Save changes"));

        auto result = watcher->result();
        if (result.ok()) {
            entry_ = result.value().entry;
            // Toast 显示在 parentWidget（PasswordBookView）：accept() 后 this 即将销毁，
            // 不能把 toast 挂在即将销毁的对话框上。
            Toast::show(this->parentWidget(), tr("Changes saved"));
            emit entry_updated(entry_.id);
            accept();
        } else {
            set_error(friendly_message(result.error()));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->update_entry_async(updated));
}

void EditEntryDialog::on_cancel_clicked() {
    reject();
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

void EditEntryDialog::update_strength(const QString& password) {
    // 入口：保留同步签名以兼容旧调用点。实际异步流程通过 on_password_changed
    // + strength_timer_ timeout 处理。这里仅同步Refresh UI 状态（空Password立即清空）。
    pending_password_ = password;
    if (password.isEmpty()) {
        update_strength_ui(core::StrengthEstimate{});
    } else if (strength_timer_) {
        strength_timer_->start(300);
    }
}

void EditEntryDialog::update_strength_ui(const core::StrengthEstimate& estimate) {
    if (!strength_label_) return;

    // 空Password：重置为初始状态（「Strength: —」），不展示评估结果
    if (pending_password_.isEmpty()) {
        strength_label_->setText(tr("Strength: —"));
        strength_label_->setProperty("cssClass", QStringLiteral("caption"));
        strength_label_->style()->unpolish(strength_label_);
        strength_label_->style()->polish(strength_label_);
        return;
    }

    // 通过 cssClass 让 QSS 接管颜色（深浅Theme适配）
    const QString label_class = strength_label_class(estimate.level);
    strength_label_->setProperty("cssClass", label_class);
    strength_label_->style()->unpolish(strength_label_);
    strength_label_->style()->polish(strength_label_);
    strength_label_->setText(
        tr("Strength: %1 (%2 bits)").arg(strength_text(estimate.level)).arg(estimate.bits));
}

void EditEntryDialog::set_error(const QString& message) {
    if (!error_label_) return;
    error_label_->setText(message);
    error_label_->setVisible(!message.isEmpty());
}

void EditEntryDialog::refresh_existing_tags() {
    // 同步入口（向后兼容）：实际由 async Version执行 IPC
    refresh_existing_tags_async();
}

void EditEntryDialog::refresh_existing_tags_async() {
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

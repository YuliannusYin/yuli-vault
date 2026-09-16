// coding: utf-8
// =============================================================================
// PasswordBookView.cpp
//
// Yuli Vault Vault视图实现（新设计）。master-detail 布局。
// =============================================================================
#include "PasswordBookView.h"
#include "EditEntryDialog.h"
#include "ErrorMessages.h"
#include "FlowLayout.h"
#include "IpcClient.h"
#include "IconKit.h"
#include "MarkdownUtil.h"
#include "PasswordBookDelegate.h"
#include "PasswordBookModel.h"
#include "StrengthUtil.h"
#include "Theme.h"
#include "Toast.h"
#include "VaultItemUi.h"

#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

namespace yuli::vault::ui {

namespace {

/// Unix Time戳（秒）→ 可读字符串。
QString format_time(int64_t ts) {
    if (ts <= 0) return QStringLiteral("-");
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ts))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

/// 取字符串首字母（大写）作为头像字符。
/// 优先使用 entry_name，回退到 account / website。
QString avatar_letter(const std::string& text) {
    std::string s = text;
    if (s.empty()) return QStringLiteral("?");
    QChar ch = QChar::fromLatin1(s[0]).toUpper();
    if (!ch.isLetterOrNumber()) return QStringLiteral("?");
    return ch;
}

/// Strong度颜色（< 40 红、40-80 黄、> 80 绿）。
/// 等级判定与文案统一通过 StrengthUtil 提供（按 core::StrengthLevel 输入）。

/// 36x36 图标按钮（无文字）。
QPushButton* make_icon_btn(const QString& svg_path, IconRole role,
                           const QString& tooltip, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setIcon(tinted_icon(svg_path, role));
    btn->setIconSize(QSize(16, 16));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tooltip);
    btn->setFixedSize(36, 36);
    btn->setProperty("cssClass", QStringLiteral("icon"));
    return btn;
}

/// 28x28 小图标按钮。
QPushButton* make_small_icon_btn(const QString& svg_path, IconRole role,
                                 const QString& tooltip, QWidget* parent) {
    auto* btn = new QPushButton(parent);
    btn->setIcon(tinted_icon(svg_path, role));
    btn->setIconSize(QSize(14, 14));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tooltip);
    btn->setFixedSize(28, 28);
    btn->setProperty("cssClass", QStringLiteral("iconSm"));
    return btn;
}

/// 创建只读Tags芯片（详情区展示用，无Delete按钮）。
QFrame* make_readonly_chip(const QString& name, QWidget* parent) {
    auto* chip = new QFrame(parent);
    chip->setProperty("cssClass", QStringLiteral("tagChip"));
    auto* hl = new QHBoxLayout(chip);
    hl->setContentsMargins(8, 2, 8, 2);
    hl->setSpacing(0);
    auto* lbl = new QLabel(name, chip);
    lbl->setObjectName(QStringLiteral("tagChipName"));
    lbl->setProperty("cssClass", QStringLiteral("tagChipName"));
    hl->addWidget(lbl);
    return chip;
}

}  // namespace

PasswordBookView::PasswordBookView(IpcClient* client, QWidget* parent)
    : QWidget(parent), client_(client)
{
    build_ui();
    set_detail_actions_enabled(false);
}

PasswordBookView::~PasswordBookView() = default;

void PasswordBookView::build_ui() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(12);

    // ── 顶部搜索行 ──
    auto* search_row = new QHBoxLayout();
    search_row->setSpacing(10);

    // 搜索输入框容器（search 图标 + 输入框）
    auto* search_container = new QFrame(this);
    search_container->setFixedHeight(40);
    search_container->setProperty("cssClass", QStringLiteral("inputField"));
    auto* search_layout = new QHBoxLayout(search_container);
    search_layout->setContentsMargins(0, 0, 0, 0);
    search_layout->setSpacing(0);

    auto* search_icon = new QLabel(search_container);
    search_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/search.svg"), IconRole::Normal, QSize(18, 18)));
    search_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    search_icon->setFixedSize(36, 40);
    search_icon->setAlignment(Qt::AlignCenter);
    search_layout->addWidget(search_icon);

    search_edit_ = new QLineEdit(search_container);
    search_edit_->setPlaceholderText(tr("Search title, account, username, notes…"));
    search_edit_->setProperty("cssClass", QStringLiteral("inlineEdit"));
    search_layout->addWidget(search_edit_, 1);
    search_row->addWidget(search_container, 1);

    // 字段下拉
    field_combo_ = new QComboBox(this);
    field_combo_->setFixedSize(140, 40);
    field_combo_->addItem(tr("All fields"), QStringLiteral("all"));
    field_combo_->setItemData(0, tr("Search all fields"), Qt::ToolTipRole);
    field_combo_->addItem(tr("Title"), QStringLiteral("entry_name"));
    field_combo_->setItemData(1, tr("Search titles only"), Qt::ToolTipRole);
    field_combo_->addItem(tr("Account"), QStringLiteral("account"));
    field_combo_->setItemData(2, tr("Search accounts only"), Qt::ToolTipRole);
    field_combo_->addItem(tr("Username"), QStringLiteral("username"));
    field_combo_->setItemData(3, tr("Search usernames only"), Qt::ToolTipRole);
    field_combo_->addItem(tr("Website"), QStringLiteral("website"));
    field_combo_->setItemData(4, tr("Search websites only"), Qt::ToolTipRole);
    field_combo_->addItem(tr("Notes"), QStringLiteral("note"));
    field_combo_->setItemData(5, tr("Search notes only"), Qt::ToolTipRole);
    search_row->addWidget(field_combo_);

    // Refresh按钮
    refresh_button_ = make_icon_btn(
        QStringLiteral(":/icons/refresh-cw.svg"),
        IconRole::Normal, tr("Refresh"), this);
    refresh_button_->setFixedSize(40, 40);
    search_row->addWidget(refresh_button_);

    layout->addLayout(search_row);

    // ── master-detail ──
    auto* md_layout = new QHBoxLayout();
    md_layout->setSpacing(12);

    // 左侧列表
    auto* list_card = new QFrame(this);
    list_card->setProperty("cssClass", QStringLiteral("card"));
    list_card->setFixedWidth(340);
    auto* list_layout = new QVBoxLayout(list_card);
    list_layout->setContentsMargins(0, 0, 0, 0);
    list_layout->setSpacing(0);

    list_ = new QListView(list_card);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setFocusPolicy(Qt::StrongFocus);
    list_->setCursor(Qt::PointingHandCursor);
    list_->setMouseTracking(true);  // 用于 entered 信号触发 tooltip（model 已提供 ToolTipRole）
    list_->setUniformItemSizes(true);  // 所有 item 高度统一 72px，性能优化
    // Disable横向滚动：条目内容（名称/Account/Tags）由 delegate 自绘，固定宽度 340px
    // 内截断，无需横向滚动；保持 card 视觉整洁，避免意外出现横向滚动条
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // cardScroll：QSS 据此应用细线滚动条（2px 宽，上下留 12px 避让 card 圆角）
    list_->setProperty("cssClass", QStringLiteral("cardScroll"));
    model_ = new PasswordBookModel(list_);
    delegate_ = new PasswordBookDelegate(list_);
    list_->setModel(model_);
    list_->setItemDelegate(delegate_);
    list_layout->addWidget(list_);
    md_layout->addWidget(list_card, 0);

    // 右侧详情（卡片 + 滚动）
    auto* detail_card = new QFrame(this);
    detail_card->setProperty("cssClass", QStringLiteral("card"));
    auto* detail_card_layout = new QVBoxLayout(detail_card);
    detail_card_layout->setContentsMargins(0, 0, 0, 0);
    detail_card_layout->setSpacing(0);

    detail_scroll_ = new QScrollArea(detail_card);
    detail_scroll_->setWidgetResizable(true);
    detail_scroll_->setFrameShape(QFrame::NoFrame);
    // cardScroll：QSS 据此应用细线滚动条（2px 宽，上下留 12px 避让 card 圆角）
    detail_scroll_->setProperty("cssClass", QStringLiteral("cardScroll"));

    detail_container_ = new QWidget(detail_scroll_);
    detail_container_->setProperty("cssClass", QStringLiteral("transparentBg"));
    auto* detail_layout = new QVBoxLayout(detail_container_);
    detail_layout->setContentsMargins(28, 28, 28, 28);
    detail_layout->setSpacing(0);

    // 空状态提示（empty_hint_ + empty_add_button_ 居Medium容器）
    auto* empty_state_layout = new QVBoxLayout();
    empty_state_layout->setSpacing(12);
    empty_hint_ = new QLabel(detail_container_);
    empty_hint_->setAlignment(Qt::AlignCenter);
    empty_hint_->setProperty("cssClass", QStringLiteral("emptyHint"));
    empty_hint_->setText(tr("Select an item on the left to view details"));
    empty_state_layout->addWidget(empty_hint_);

    empty_add_button_ = new QPushButton(detail_container_);
    empty_add_button_->setText(tr("New item"));
    empty_add_button_->setCursor(Qt::PointingHandCursor);
    empty_add_button_->setFixedHeight(40);
    empty_add_button_->setMaximumWidth(180);
    empty_add_button_->setProperty("cssClass", QStringLiteral("primary"));
    empty_add_button_->hide();  // 默认隐藏，show_empty_state 时按需显示
    empty_state_layout->addWidget(empty_add_button_, 0, Qt::AlignCenter);

    detail_layout->addStretch(1);
    detail_layout->addLayout(empty_state_layout);
    detail_layout->addStretch(1);

    detail_scroll_->setWidget(detail_container_);
    detail_card_layout->addWidget(detail_scroll_);
    md_layout->addWidget(detail_card, 1);

    layout->addLayout(md_layout, 1);

    // ── 详情头部（默认隐藏，load_detail 时显示） ──
    // 头部 row
    auto* header_row = new QHBoxLayout();
    header_row->setSpacing(16);

    detail_avatar_ = new QLabel(detail_container_);
    detail_avatar_->setFixedSize(56, 56);
    detail_avatar_->setAlignment(Qt::AlignCenter);
    detail_avatar_->setProperty("cssClass", QStringLiteral("avatarLg"));
    header_row->addWidget(detail_avatar_);

    auto* title_col = new QVBoxLayout();
    title_col->setSpacing(2);
    detail_title_ = new QLabel(detail_container_);
    detail_title_->setProperty("cssClass", QStringLiteral("titleLg"));
    title_col->addWidget(detail_title_);
    detail_subtitle_ = new QLabel(detail_container_);
    detail_subtitle_->setProperty("cssClass", QStringLiteral("muted"));
    title_col->addWidget(detail_subtitle_);
    title_col->addStretch(1);
    header_row->addLayout(title_col, 1);

    // 头部右侧Actions按钮
    copy_account_btn_ = make_icon_btn(
        QStringLiteral(":/icons/user.svg"),
        IconRole::Normal, tr("Copy account"), detail_container_);
    header_row->addWidget(copy_account_btn_);

    copy_pwd_btn_ = make_icon_btn(
        QStringLiteral(":/icons/key.svg"),
        IconRole::Normal, tr("Copy password"), detail_container_);
    header_row->addWidget(copy_pwd_btn_);

    edit_btn_ = new QPushButton(detail_container_);
    edit_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/pencil.svg"), IconRole::OnPrimary));
    edit_btn_->setIconSize(QSize(14, 14));
    edit_btn_->setText(tr("Edit"));
    edit_btn_->setCursor(Qt::PointingHandCursor);
    edit_btn_->setFixedHeight(40);
    edit_btn_->setProperty("cssClass", QStringLiteral("primary"));
    header_row->addWidget(edit_btn_);

    delete_btn_ = make_icon_btn(
        QStringLiteral(":/icons/trash-2.svg"),
        IconRole::Danger, tr("Delete"), detail_container_);
    header_row->addWidget(delete_btn_);

    detail_layout->addLayout(header_row);

    detail_layout->addSpacing(28);

    // 字段网格
    auto* fields_layout = new QVBoxLayout();
    fields_layout->setSpacing(16);

    auto add_field_row = [&](const QString& caption, QLabel*& value_label,
                              QPushButton*& copy_btn) {
        auto* row = new QFrame(detail_container_);
        row->setProperty("cssClass", QStringLiteral("fieldRow"));
        auto* row_layout = new QVBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 12);
        row_layout->setSpacing(6);

        auto* caption_label = new QLabel(caption, row);
        caption_label->setProperty("cssClass", QStringLiteral("fieldCaption"));
        row_layout->addWidget(caption_label);

        auto* value_row = new QHBoxLayout();
        value_row->setSpacing(8);
        value_label = new QLabel(row);
        value_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value_label->setWordWrap(true);
        value_row->addWidget(value_label, 1);
        copy_btn = make_small_icon_btn(
            QStringLiteral(":/icons/copy.svg"),
            IconRole::Normal, tr("Copy"), row);
        value_row->addWidget(copy_btn, 0, Qt::AlignRight);
        row_layout->addLayout(value_row);
        fields_layout->addWidget(row);
        return row;
    };

    // Title
    add_field_row(tr("Title"), field_entry_name_, copy_entry_name_btn_);
    // Username
    add_field_row(tr("Username"), field_username_, copy_user_field_btn_);
    // Account（用 user.svg 区分于Password字段的 key.svg）
    add_field_row(tr("Account"), field_account_, copy_account_field_btn_);
    if (copy_account_field_btn_)
        copy_account_field_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/user.svg"), IconRole::Normal));
    // Website
    add_field_row(tr("Website"), field_website_, copy_website_btn_);

    // Password行（特殊：带可见性切换 + Strong度 badge）
    auto* pwd_row = new QFrame(detail_container_);
    pwd_row->setProperty("cssClass", QStringLiteral("fieldRow"));
    auto* pwd_row_layout = new QVBoxLayout(pwd_row);
    pwd_row_layout->setContentsMargins(0, 0, 0, 12);
    pwd_row_layout->setSpacing(6);

    auto* pwd_caption = new QLabel(tr("Password"), pwd_row);
    pwd_caption->setProperty("cssClass", QStringLiteral("fieldCaption"));
    pwd_row_layout->addWidget(pwd_caption);

    auto* pwd_value_row = new QHBoxLayout();
    pwd_value_row->setSpacing(8);
    field_password_ = new QLabel(pwd_row);
    field_password_->setProperty("cssClass", QStringLiteral("textMono"));
    pwd_value_row->addWidget(field_password_, 1);

    toggle_pwd_btn_ = make_small_icon_btn(
        QStringLiteral(":/icons/eye.svg"),
        IconRole::Normal, tr("Show password"), pwd_row);
    pwd_value_row->addWidget(toggle_pwd_btn_);

    copy_pwd_field_btn_ = make_small_icon_btn(
        QStringLiteral(":/icons/key.svg"),
        IconRole::Normal, tr("Copy password"), pwd_row);
    pwd_value_row->addWidget(copy_pwd_field_btn_);

    strength_badge_ = new QLabel(pwd_row);
    strength_badge_->setProperty("cssClass", QStringLiteral("badgeSuccess"));
    strength_badge_->setAlignment(Qt::AlignCenter);
    pwd_value_row->addWidget(strength_badge_);
    pwd_row_layout->addLayout(pwd_value_row);
    fields_layout->addWidget(pwd_row);

    // Tags行（只读芯片流式展示）
    auto* tags_row = new QFrame(detail_container_);
    tags_row->setProperty("cssClass", QStringLiteral("fieldRow"));
    auto* tags_row_layout = new QVBoxLayout(tags_row);
    tags_row_layout->setContentsMargins(0, 0, 0, 12);
    tags_row_layout->setSpacing(6);

    auto* tags_caption = new QLabel(tr("Tags"), tags_row);
    tags_caption->setProperty("cssClass", QStringLiteral("fieldCaption"));
    tags_row_layout->addWidget(tags_caption);

    field_tags_container_ = new QWidget(tags_row);
    field_tags_layout_ = new FlowLayout(field_tags_container_, /*margin=*/0, /*hSpacing=*/6, /*vSpacing=*/6);
    tags_row_layout->addWidget(field_tags_container_);
    fields_layout->addWidget(tags_row);

    // Notes（markdown 渲染）
    auto* note_row = new QFrame(detail_container_);
    note_row->setProperty("cssClass", QStringLiteral("fieldRow"));
    auto* note_row_layout = new QVBoxLayout(note_row);
    note_row_layout->setContentsMargins(0, 0, 0, 12);
    note_row_layout->setSpacing(6);

    auto* note_caption = new QLabel(tr("Notes"), note_row);
    note_caption->setProperty("cssClass", QStringLiteral("fieldCaption"));
    note_row_layout->addWidget(note_caption);

    field_note_ = new QTextBrowser(note_row);
    field_note_->setOpenExternalLinks(true);
    field_note_->setProperty("cssClass", QStringLiteral("markdownView"));
    field_note_->setMinimumHeight(60);
    // 不设上限让其完全展开，高度跟随内容；避免内部滚动与外层 detail_scroll_ 嵌套。
    field_note_->setMaximumHeight(QWIDGETSIZE_MAX);
    note_row_layout->addWidget(field_note_);
    fields_layout->addWidget(note_row);

    // Time行
    auto* time_row = new QHBoxLayout();
    time_row->setSpacing(16);

    auto* created_col = new QVBoxLayout();
    created_col->setSpacing(6);
    auto* created_caption = new QLabel(tr("Created"), detail_container_);
    created_caption->setProperty("cssClass", QStringLiteral("fieldCaption"));
    created_col->addWidget(created_caption);
    field_created_ = new QLabel(detail_container_);
    field_created_->setProperty("cssClass", QStringLiteral("caption"));
    created_col->addWidget(field_created_);
    time_row->addLayout(created_col);

    auto* updated_col = new QVBoxLayout();
    updated_col->setSpacing(6);
    auto* updated_caption = new QLabel(tr("Modified"), detail_container_);
    updated_caption->setProperty("cssClass", QStringLiteral("fieldCaption"));
    updated_col->addWidget(updated_caption);
    field_updated_ = new QLabel(detail_container_);
    field_updated_->setProperty("cssClass", QStringLiteral("caption"));
    updated_col->addWidget(field_updated_);
    time_row->addLayout(updated_col);
    time_row->addStretch(1);
    fields_layout->addLayout(time_row);

    detail_layout->addLayout(fields_layout);

    // 外部链接按钮
    open_external_btn_ = new QPushButton(detail_container_);
    open_external_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/external-link.svg"), IconRole::Normal));
    open_external_btn_->setIconSize(QSize(14, 14));
    open_external_btn_->setCursor(Qt::PointingHandCursor);
    open_external_btn_->setFixedHeight(40);
    open_external_btn_->setProperty("cssClass", QStringLiteral("outline"));
    detail_layout->addSpacing(24);
    detail_layout->addWidget(open_external_btn_);
    detail_layout->addStretch(1);

    // 默认隐藏所有详情控件
    clear_detail();

    // ── 信号槽 ──
    connect(search_edit_, &QLineEdit::returnPressed,
            this, &PasswordBookView::on_search_clicked);
    connect(search_edit_, &QLineEdit::textChanged,
            this, &PasswordBookView::on_search_text_changed);
    connect(refresh_button_, &QPushButton::clicked,
            this, &PasswordBookView::on_refresh_clicked);
    connect(list_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &PasswordBookView::on_list_current_changed);
    connect(copy_account_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_copy_account_clicked);
    connect(copy_pwd_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_copy_password_clicked);
    connect(copy_entry_name_btn_, &QPushButton::clicked,
            this, [this]() {
                if (current_id_ == 0) return;
                const auto* entry = current_entry();
                if (!entry) return;
                copy_secure_to_clipboard(
                    QString::fromStdString(entry->title));
                Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
            });
    connect(copy_account_field_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_copy_account_clicked);
    connect(copy_user_field_btn_, &QPushButton::clicked,
            this, [this]() {
                if (current_id_ == 0) return;
                const auto* entry = current_entry();
                if (!entry) return;
                copy_secure_to_clipboard(
                    QString::fromStdString(entry->username));
                Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
            });
    connect(copy_website_btn_, &QPushButton::clicked,
            this, [this]() {
                if (current_id_ == 0) return;
                const auto* entry = current_entry();
                if (!entry) return;
                copy_secure_to_clipboard(
                    QString::fromStdString(entry->website));
                Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
            });
    connect(copy_pwd_field_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_copy_password_clicked);
    connect(toggle_pwd_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_toggle_password_clicked);
    connect(edit_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_edit_clicked);
    connect(delete_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_delete_clicked);
    connect(open_external_btn_, &QPushButton::clicked,
            this, &PasswordBookView::on_open_external_clicked);
    connect(empty_add_button_, &QPushButton::clicked,
            this, &PasswordBookView::entry_add_requested);

    // ── 增量搜索 debounce 定时器（300ms） ──
    search_timer_ = new QTimer(this);
    search_timer_->setSingleShot(true);
    search_timer_->setInterval(300);
    connect(search_timer_, &QTimer::timeout, this, [this]() {
        const QString text = pending_search_text_.trimmed();
        const QString field = field_combo_->currentData().toString();
        search_async(text, field);
    });

    // loading_hint_ 复用 empty_hint_ 的位置/样式，不新建控件
    loading_hint_ = empty_hint_;
}

// ---------------------------------------------------------------------------
// 列表加载与搜索
// ---------------------------------------------------------------------------

void PasswordBookView::refresh() {
    refresh_async();
}

void PasswordBookView::focus_search() {
    if (search_edit_) search_edit_->setFocus();
}

void PasswordBookView::focus_list() {
    if (list_) list_->setFocus();
}

void PasswordBookView::refresh_async() {
    if (!client_) {
        show_empty_state(tr("IPC client unavailable"));
        return;
    }
    begin_loading(tr("Loading…"));
    auto* watcher = new QFutureWatcher<core::Result<protocol::ListEntriesResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::ListEntriesResponse>>::finished,
            this, [this, watcher]() {
        auto result = watcher->result();
        if (result.ok()) {
            populate_list(result.value().entries);
        } else {
            populate_list({});
            show_empty_state(
                tr("Load failed: %1").arg(friendly_message(result.error())));
        }
        end_loading();
        watcher->deleteLater();
    });
    watcher->setFuture(client_->list_entries_async());
}

void PasswordBookView::on_search_clicked() {
    // 立即搜索：Cancel pending debounce，直接发起请求
    if (search_timer_) search_timer_->stop();
    const QString text = search_edit_->text().trimmed();
    const QString field = field_combo_->currentData().toString();
    search_async(text, field);
}

void PasswordBookView::search_async(const QString& text, const QString& field) {
    if (!client_) return;
    if (text.isEmpty()) {
        refresh_async();
        return;
    }

    core::SearchQuery query;
    query.text = text.toStdString();
    query.case_sensitive = false;
    if (field == QStringLiteral("entry_name")) query.fields = {"entry_name"};
    else if (field == QStringLiteral("account")) query.fields = {"account"};
    else if (field == QStringLiteral("username")) query.fields = {"username"};
    else if (field == QStringLiteral("website")) query.fields = {"website"};
    else if (field == QStringLiteral("note")) query.fields = {"note"};

    begin_loading(tr("Searching…"));
    auto* watcher = new QFutureWatcher<core::Result<protocol::SearchEntriesResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::SearchEntriesResponse>>::finished,
            this, [this, watcher]() {
        auto result = watcher->result();
        if (result.ok()) {
            populate_list(result.value().entries);
        } else {
            populate_list({});
            show_empty_state(
                tr("Search failed: %1").arg(friendly_message(result.error())));
        }
        end_loading();
        watcher->deleteLater();
    });
    watcher->setFuture(client_->search_entries_async(query));
}

void PasswordBookView::on_refresh_clicked() {
    // 用 QSignalBlocker 阻止 clear() 触发 textChanged → on_search_text_changed，
    // 避免与下面的显式 refresh_async() 重复发起 IPC 请求。
    {
        QSignalBlocker blocker(search_edit_);
        search_edit_->clear();
    }
    field_combo_->setCurrentIndex(0);
    clear_all_note_cache();  // Refresh时清空 Markdown 缓存，避免显示过期内容
    refresh();
}

void PasswordBookView::on_search_text_changed(const QString& text) {
    if (text.isEmpty()) {
        // 清空时立即 refresh，不等 debounce
        if (search_timer_) search_timer_->stop();
        refresh_async();
        return;
    }
    // 累积文本并启动 debounce
    pending_search_text_ = text;
    search_timer_->start();
}

void PasswordBookView::populate_list(const std::vector<core::PasswordEntry>& new_entries) {
    entries_ = new_entries;
    model_->set_entries(entries_);

    emit entry_count_changed(static_cast<int>(entries_.size()));

    if (entries_.empty()) {
        show_empty_state(tr("No items yet"));
    } else {
        // 默认选Medium第一条；currentChanged 信号会触发 load_detail
        const QModelIndex first = model_->index(0);
        list_->setCurrentIndex(first);
    }
}

// ---------------------------------------------------------------------------
// 详情显示
// ---------------------------------------------------------------------------

void PasswordBookView::show_empty_state(const QString& message) {
    empty_hint_->setText(message);
    empty_hint_->show();
    // 仅当 message 是"No items yet"类（非加载Medium、非错误）时才显示新建按钮
    const bool show_add = (message == tr("No items yet"));
    empty_add_button_->setVisible(show_add);
    clear_detail();
}

void PasswordBookView::begin_loading(const QString& message) {
    // 复用 empty_hint_ 显示 loading 文案
    if (loading_hint_) {
        loading_hint_->setText(message);
        loading_hint_->show();
    }
    if (empty_add_button_) empty_add_button_->hide();  // loading 时不显示新建按钮
    // 列表区Disable，防止用户在加载MediumActions
    if (list_) list_->setEnabled(false);
}

void PasswordBookView::end_loading() {
    if (list_) list_->setEnabled(true);
    // empty_hint_ 的隐藏由 populate_list/show_empty_state 控制
}

void PasswordBookView::clear_note_cache_for(int64_t id) {
    note_cache_.remove(id);
}

void PasswordBookView::clear_all_note_cache() {
    note_cache_.clear();
}

void PasswordBookView::clear_detail() {
    current_id_ = 0;
    password_visible_ = false;
    if (detail_avatar_) detail_avatar_->hide();
    if (detail_title_) detail_title_->hide();
    if (detail_subtitle_) detail_subtitle_->hide();
    if (copy_account_btn_) copy_account_btn_->hide();
    if (copy_pwd_btn_) copy_pwd_btn_->hide();
    if (edit_btn_) edit_btn_->hide();
    if (delete_btn_) delete_btn_->hide();
    auto hide_field_row = [](QLabel* lbl) {
        if (lbl && lbl->parentWidget()) lbl->parentWidget()->hide();
    };
    hide_field_row(field_entry_name_);
    hide_field_row(field_account_);
    hide_field_row(field_username_);
    hide_field_row(field_website_);
    hide_field_row(field_password_);
    if (field_note_ && field_note_->parentWidget()) field_note_->parentWidget()->hide();
    if (field_tags_container_ && field_tags_container_->parentWidget()) {
        field_tags_container_->parentWidget()->hide();
    }
    if (field_created_) field_created_->hide();
    if (field_updated_) field_updated_->hide();
    if (strength_badge_) strength_badge_->hide();
    if (toggle_pwd_btn_) toggle_pwd_btn_->hide();
    if (copy_entry_name_btn_) copy_entry_name_btn_->hide();
    if (copy_account_field_btn_) copy_account_field_btn_->hide();
    if (copy_user_field_btn_) copy_user_field_btn_->hide();
    if (copy_website_btn_) copy_website_btn_->hide();
    if (copy_pwd_field_btn_) copy_pwd_field_btn_->hide();
    if (open_external_btn_) open_external_btn_->hide();
    set_detail_actions_enabled(false);
}

void PasswordBookView::load_detail(const core::PasswordEntry& entry) {
    current_id_ = entry.id;
    password_visible_ = false;

    empty_hint_->hide();
    empty_add_button_->hide();  // 加载详情时隐藏空状态按钮

    // 头部
    std::string avatar_src = entry.title;
    if (avatar_src.empty()) avatar_src = entry.account;
    if (avatar_src.empty()) avatar_src = entry.website;
    detail_avatar_->setText(avatar_letter(avatar_src));
    detail_avatar_->show();

    // 主标题：Title
    detail_title_->setText(QString::fromStdString(entry.title));
    detail_title_->show();
    const QString type_label = vault_item_type_label(entry.type);
    if (entry.account.empty() || !vault_item_type_has_login_editor(entry.type)) {
        detail_subtitle_->setText(type_label);
    } else {
        detail_subtitle_->setText(
            tr("%1 · %2").arg(type_label, QString::fromStdString(entry.account)));
    }
    detail_subtitle_->show();

    if (!vault_item_type_has_login_editor(entry.type)) {
        auto hide_field_row = [](QLabel* lbl) {
            if (lbl && lbl->parentWidget()) lbl->parentWidget()->hide();
        };
        copy_account_btn_->hide();
        copy_pwd_btn_->hide();
        edit_btn_->hide();
        delete_btn_->show();
        hide_field_row(field_entry_name_);
        hide_field_row(field_account_);
        hide_field_row(field_username_);
        hide_field_row(field_website_);
        hide_field_row(field_password_);
        if (field_tags_container_ && field_tags_container_->parentWidget()) {
            field_tags_container_->parentWidget()->hide();
        }
        if (strength_badge_) strength_badge_->hide();
        if (toggle_pwd_btn_) toggle_pwd_btn_->hide();
        if (copy_entry_name_btn_) copy_entry_name_btn_->hide();
        if (copy_account_field_btn_) copy_account_field_btn_->hide();
        if (copy_user_field_btn_) copy_user_field_btn_->hide();
        if (copy_website_btn_) copy_website_btn_->hide();
        if (copy_pwd_field_btn_) copy_pwd_field_btn_->hide();
        if (open_external_btn_) open_external_btn_->hide();
        field_note_->setHtml(tr("<p>This item type is not supported in this version. Update Yuli Vault to open it.</p>"));
        field_note_->parentWidget()->show();
        field_created_->setText(format_time(entry.created_at));
        field_created_->show();
        field_updated_->setText(format_time(entry.updated_at));
        field_updated_->show();
        set_detail_actions_enabled(true);
        if (edit_btn_) edit_btn_->setEnabled(false);
        if (copy_account_btn_) copy_account_btn_->setEnabled(false);
        if (copy_pwd_btn_) copy_pwd_btn_->setEnabled(false);
        return;
    }

    copy_account_btn_->show();
    copy_pwd_btn_->show();
    edit_btn_->show();
    delete_btn_->show();

    // 字段（顺序与 UI 布局保持一致：Title→Username→Account→Website）
    field_entry_name_->setText(QString::fromStdString(entry.title));
    field_entry_name_->parentWidget()->show();
    field_username_->setText(QString::fromStdString(entry.username));
    field_username_->parentWidget()->show();
    field_account_->setText(QString::fromStdString(entry.account));
    field_account_->parentWidget()->show();
    field_website_->setText(QString::fromStdString(entry.website));
    field_website_->parentWidget()->show();

    // Password：默认掩码（方块字符 ████，参考 Telegram 桌面端风格）
    field_password_->setText(QStringLiteral("████████████"));
    field_password_->parentWidget()->show();
    toggle_pwd_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
    toggle_pwd_btn_->setToolTip(tr("Show password"));
    toggle_pwd_btn_->show();
    copy_pwd_field_btn_->show();

    // Strong度 badge：异步查询，结果回来后再显示
    //   仅当用户仍在看同一 entry 时才更新（避免竞态导致显示错位）
    if (client_) {
        strength_badge_->hide();
        const int64_t entry_id = entry.id;
        auto* watcher = new QFutureWatcher<core::Result<protocol::EstimateStrengthResponse>>(this);
        connect(watcher, &QFutureWatcher<core::Result<protocol::EstimateStrengthResponse>>::finished,
                this, [this, watcher, entry_id]() {
            auto r = watcher->result();
            if (current_id_ == entry_id && r.ok()) {
                const auto level = r.value().estimate.level;
                strength_badge_->setText(strength_text(level));
                strength_badge_->setProperty("cssClass", strength_badge_class(level));
                strength_badge_->style()->unpolish(strength_badge_);
                strength_badge_->style()->polish(strength_badge_);
                strength_badge_->show();
            }
            watcher->deleteLater();
        });
        watcher->setFuture(client_->estimate_strength_async(entry.password));
    } else {
        strength_badge_->hide();
    }

    // Tags：清空旧芯片，按 entry.tags 重建
    while (field_tags_layout_->count() > 0) {
        QLayoutItem* item = field_tags_layout_->takeAt(0);
        if (!item) break;
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    if (entry.tags.empty()) {
        // 无Tags时隐藏整个Tags行
        field_tags_container_->parentWidget()->hide();
    } else {
        for (const auto& tag : entry.tags) {
            auto* chip = make_readonly_chip(QString::fromStdString(tag.name),
                                            field_tags_container_);
            field_tags_layout_->addWidget(chip);
        }
        field_tags_container_->parentWidget()->show();
    }

    // Notes：markdown → HTML 渲染（按 entry.id 缓存，避免重复渲染）
    //   空Notes用 <p class="muted"> 显示「(无)」，由 QSS 接管颜色，深浅Theme自动适配。
    if (entry.note.empty()) {
        field_note_->setHtml(tr("<p class=\"muted\">(none)</p>"));
    } else {
        QString html;
        const auto cache_it = note_cache_.find(entry.id);
        if (cache_it != note_cache_.end()) {
            html = cache_it.value();
        } else {
            html = markdown_to_html(entry.note);
            note_cache_.insert(entry.id, html);
        }
        field_note_->setHtml(html);
    }
    field_note_->parentWidget()->show();

    // Time
    field_created_->setText(format_time(entry.created_at));
    field_created_->show();
    field_updated_->setText(format_time(entry.updated_at));
    field_updated_->show();

    copy_entry_name_btn_->show();
    copy_account_field_btn_->show();
    copy_user_field_btn_->show();
    copy_website_btn_->show();

    // 外部链接
    if (entry.website.empty()) {
        open_external_btn_->hide();
    } else {
        open_external_btn_->setText(
            tr("Open %1").arg(QString::fromStdString(entry.website)));
        open_external_btn_->show();
    }

    set_detail_actions_enabled(true);
}

void PasswordBookView::set_detail_actions_enabled(bool enabled) {
    if (copy_account_btn_) copy_account_btn_->setEnabled(enabled);
    if (copy_pwd_btn_) copy_pwd_btn_->setEnabled(enabled);
    if (edit_btn_) edit_btn_->setEnabled(enabled);
    if (delete_btn_) delete_btn_->setEnabled(enabled);
    if (toggle_pwd_btn_) toggle_pwd_btn_->setEnabled(enabled);
    if (copy_entry_name_btn_) copy_entry_name_btn_->setEnabled(enabled);
    if (copy_account_field_btn_) copy_account_field_btn_->setEnabled(enabled);
    if (copy_user_field_btn_) copy_user_field_btn_->setEnabled(enabled);
    if (copy_website_btn_) copy_website_btn_->setEnabled(enabled);
    if (copy_pwd_field_btn_) copy_pwd_field_btn_->setEnabled(enabled);
    if (open_external_btn_) open_external_btn_->setEnabled(enabled);
}

const core::PasswordEntry* PasswordBookView::current_entry() const {
    if (current_id_ == 0) return nullptr;
    for (const auto& e : entries_) {
        if (e.id == current_id_) return &e;
    }
    return nullptr;
}

void PasswordBookView::on_list_current_changed(const QModelIndex& current, const QModelIndex& previous) {
    (void)previous;
    if (!current.isValid()) {
        clear_detail();
        return;
    }
    const auto* entry = model_->entry_at(current.row());
    if (entry) {
        load_detail(*entry);
    } else {
        clear_detail();
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void PasswordBookView::on_copy_account_clicked() {
    const auto* entry = current_entry();
    if (!entry) return;
    copy_secure_to_clipboard(QString::fromStdString(entry->account));
    Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
}

void PasswordBookView::on_copy_password_clicked() {
    const auto* entry = current_entry();
    if (!entry) return;
    // 直接用缓存Medium的明文Password：列表加载时 service 已解密返回最新明文，
    // Edit保存后会 refresh，缓存也会更新。无需再次调用 get_entry。
    copy_secure_to_clipboard(QString::fromStdString(entry->password));
    Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
}

void PasswordBookView::on_toggle_password_clicked() {
    const auto* entry = current_entry();
    if (!entry) return;
    password_visible_ = !password_visible_;
    if (password_visible_) {
        // 直接用缓存Password（同 on_copy_password_clicked 的理由）
        field_password_->setText(QString::fromStdString(entry->password));
        toggle_pwd_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eye-off.svg"), IconRole::Normal));
        toggle_pwd_btn_->setToolTip(tr("Hide password"));
    } else {
        field_password_->setText(QStringLiteral("████████████"));
        toggle_pwd_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
        toggle_pwd_btn_->setToolTip(tr("Show password"));
    }
}

void PasswordBookView::on_edit_clicked() {
    const auto* entry = current_entry();
    if (!entry) return;
    if (!vault_item_type_has_login_editor(entry->type)) {
        QMessageBox::information(
            this, tr("Unsupported item"),
            tr("This item type is not supported in this version. Update Yuli Vault to open it."));
        return;
    }

    // 调用 get_entry 拿最新完整数据（含明文Password）。
    // 此处保持同步调用：dlg.exec() 是模态阻塞，调用期间 UI 本来就不响应；
    // get_entry 通常很快（< 100ms），异步化反而让对话框弹出时机延迟。
    core::PasswordEntry current = *entry;
    if (client_) {
        auto r = client_->get_entry(entry->id);
        if (r.ok()) {
            current = r.value().entry;
        }
    }

    EditEntryDialog dlg(client_, current, this);
    connect(&dlg, &EditEntryDialog::entry_updated,
            this, &PasswordBookView::entry_updated);
    if (dlg.exec() == QDialog::Accepted) {
        clear_all_note_cache();  // Edit可能改了Notes，清缓存重渲染
        refresh_async();
        emit entry_updated(current_id_);
    }
}

void PasswordBookView::on_delete_clicked() {
    if (current_id_ == 0) return;
    const auto* entry = current_entry();
    const QString name = entry
        ? QString::fromStdString(entry->title.empty()
              ? entry->account : entry->title)
        : QStringLiteral("id=%1").arg(current_id_);

    const auto answer = QMessageBox::question(
        this,
        tr("Confirm delete"),
        tr("Delete entry “%1”? This cannot be undone.").arg(name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    if (!client_) return;

    // 立即清缓存（避免Delete后还能从缓存拿到旧 HTML）
    clear_note_cache_for(current_id_);

    auto* watcher = new QFutureWatcher<core::Result<protocol::RemoveEntryResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::RemoveEntryResponse>>::finished,
            this, [this, watcher]() {
        auto result = watcher->result();
        if (result.ok()) {
            refresh_async();
        } else {
            const QString msg = friendly_message(result.error());
            QMessageBox::warning(this, tr("Delete failed"),
                msg.isEmpty() ? tr("Could not delete the item.")
                              : tr("Delete failed: %1").arg(msg));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->remove_entry_async(current_id_));
}

void PasswordBookView::on_open_external_clicked() {
    const auto* entry = current_entry();
    if (!entry) return;
    if (entry->website.empty()) return;
    QString url = QString::fromStdString(entry->website);
    if (!url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
        !url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        url = QStringLiteral("https://") + url;
    }
    QDesktopServices::openUrl(QUrl(url));
}

}  // namespace yuli::vault::ui

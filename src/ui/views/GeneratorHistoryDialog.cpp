// coding: utf-8
// =============================================================================
// GeneratorHistoryDialog.cpp
//
// Yuli Vault Generator history对话框实现。
// 模态遮罩 + 720px 居Medium卡片 + 工具栏 + 表格 + 尾部按钮。
// =============================================================================
#include "GeneratorHistoryDialog.h"
#include "IpcClient.h"
#include "IconKit.h"
#include "Toast.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

namespace yuli::vault::ui {

namespace {

/// 默认的Password遮罩字符，按Length重复填充。
/// 用全角字符 U+2022 与 QSS 配合以与项目其他位置一致。
QString masked_password(int length) {
    if (length <= 0) return QStringLiteral("•");
    return QString(QStringLiteral("•")).repeated(length);
}

QString format_time(int64_t ts) {
    if (ts <= 0) return QStringLiteral("-");
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ts))
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

}  // namespace

GeneratorHistoryDialog::GeneratorHistoryDialog(IpcClient* client, QWidget* parent)
    : QDialog(parent), client_(client)
{
    // 模态 + 无原生标题栏，自定义遮罩 + 卡片
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Generator history"));

    // 自动覆盖父窗口大小
    if (parent) {
        setGeometry(parent->window()->geometry());
    }

    build_ui();
}

GeneratorHistoryDialog::~GeneratorHistoryDialog() = default;

void GeneratorHistoryDialog::closeEvent(QCloseEvent* event) {
    QDialog::closeEvent(event);
}

void GeneratorHistoryDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QDialog::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// UI 构建
// ---------------------------------------------------------------------------

void GeneratorHistoryDialog::build_ui() {
    // 根布局：半透明遮罩
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* overlay = new QFrame(this);
    overlay->setProperty("cssClass", QStringLiteral("modalOverlay"));
    auto* overlay_layout = new QVBoxLayout(overlay);
    overlay_layout->setContentsMargins(16, 16, 16, 16);
    overlay_layout->setAlignment(Qt::AlignCenter);

    // ── 720px 卡片 ──
    auto* card = new QFrame(overlay);
    card->setFixedWidth(720);
    card->setProperty("cssClass", QStringLiteral("modal"));
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(0, 0, 0, 0);
    card_layout->setSpacing(0);

    // ── 头部（56px 高） ──
    auto* header = new QFrame(card);
    header->setFixedHeight(56);
    header->setProperty("cssClass", QStringLiteral("modalHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(24, 0, 12, 0);
    header_layout->setSpacing(10);

    auto* clock_icon = new QLabel(header);
    clock_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/clock.svg"),
                                       IconRole::Normal, QSize(18, 18)));
    clock_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    header_layout->addWidget(clock_icon);

    auto* title = new QLabel(tr("Generator history"), header);
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

    // ── 体 ──
    auto* body = new QFrame(card);
    body->setProperty("cssClass", QStringLiteral("modalBody"));
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 20, 24, 20);
    body_layout->setSpacing(12);

    // 工具栏：[Show password] + stretch + [状态Tags] + [Refresh]
    auto* toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(12);

    show_password_check_ = new QCheckBox(tr("Show password"), body);
    show_password_check_->setCursor(Qt::PointingHandCursor);
    toolbar->addWidget(show_password_check_);
    toolbar->addStretch(1);

    status_label_ = new QLabel(body);
    status_label_->setProperty("cssClass", QStringLiteral("caption"));
    toolbar->addWidget(status_label_);

    refresh_btn_ = new QPushButton(body);
    refresh_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/refresh-cw.svg"), IconRole::Normal));
    refresh_btn_->setIconSize(QSize(16, 16));
    refresh_btn_->setText(tr("Refresh"));
    refresh_btn_->setCursor(Qt::PointingHandCursor);
    refresh_btn_->setFixedHeight(36);
    refresh_btn_->setProperty("cssClass", QStringLiteral("outline"));
    toolbar->addWidget(refresh_btn_);

    body_layout->addLayout(toolbar);

    // ── 表格 ──
    table_ = new QTableWidget(body);
    table_->setColumnCount(5);
    // cardScroll：QSS 据此应用细线滚动条（2px 宽，上下留 12px 避让 card 圆角）
    table_->setProperty("cssClass", QStringLiteral("cardScroll"));
    table_->setHorizontalHeaderLabels(
        QStringList() << QStringLiteral("#")
                       << tr("Time")
                       << tr("Length")
                       << tr("Password")
                       << tr("Actions"));
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setFocusPolicy(Qt::StrongFocus);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 列宽：固定列 + Password列按内容自适应（最小 200 防止长Password被压缩）
    table_->horizontalHeader()->setMinimumSectionSize(200);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->setMinimumHeight(320);
    // 行高与单元格内嵌按钮配合（按钮 32px + 8 padding）
    table_->verticalHeader()->setDefaultSectionSize(40);
    body_layout->addWidget(table_, 1);

    // ── 空状态（提示文案 + 「Generate a password」按钮） ──
    // 与 table_ 共享 body_layout 的可伸展空间（两者都带 stretch=1），
    // 通过 setVisible 切换：表格有数据时显示表格，否则显示空状态。
    auto* empty_state_layout = new QVBoxLayout();
    empty_state_layout->setSpacing(12);
    empty_label_ = new QLabel(body);
    empty_label_->setAlignment(Qt::AlignCenter);
    empty_label_->setProperty("cssClass", QStringLiteral("emptyHint"));
    empty_label_->setText(tr("No generated passwords yet"));
    empty_label_->hide();
    empty_state_layout->addWidget(empty_label_);

    empty_action_button_ = new QPushButton(body);
    empty_action_button_->setText(tr("Generate a password"));
    empty_action_button_->setCursor(Qt::PointingHandCursor);
    empty_action_button_->setFixedHeight(40);
    empty_action_button_->setMaximumWidth(180);
    empty_action_button_->setProperty("cssClass", QStringLiteral("primary"));
    empty_action_button_->hide();
    empty_state_layout->addWidget(empty_action_button_, 0, Qt::AlignCenter);

    body_layout->addLayout(empty_state_layout, 1);

    card_layout->addWidget(body, 1);

    // ── 尾部 ──
    auto* footer = new QFrame(card);
    footer->setProperty("cssClass", QStringLiteral("modalFooter"));
    footer->setFixedHeight(64);
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(24, 0, 24, 0);
    footer_layout->setSpacing(12);

    clear_all_btn_ = new QPushButton(footer);
    clear_all_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/eraser.svg"), IconRole::Danger));
    clear_all_btn_->setIconSize(QSize(16, 16));
    clear_all_btn_->setText(tr("Clear all"));
    clear_all_btn_->setCursor(Qt::PointingHandCursor);
    clear_all_btn_->setFixedHeight(40);
    clear_all_btn_->setProperty("cssClass", QStringLiteral("danger"));
    footer_layout->addWidget(clear_all_btn_);

    footer_layout->addStretch(1);

    close_btn_ = new QPushButton(footer);
    close_btn_->setText(tr("Close"));
    close_btn_->setCursor(Qt::PointingHandCursor);
    close_btn_->setFixedHeight(40);
    close_btn_->setProperty("cssClass", QStringLiteral("primary"));
    footer_layout->addWidget(close_btn_);

    card_layout->addWidget(footer);
    overlay_layout->addWidget(card);
    root_layout->addWidget(overlay);

    // 信号槽
    connect(show_password_check_, &QCheckBox::toggled,
            this, &GeneratorHistoryDialog::on_show_password_toggled);
    connect(refresh_btn_, &QPushButton::clicked,
            this, &GeneratorHistoryDialog::on_refresh_clicked);
    connect(clear_all_btn_, &QPushButton::clicked,
            this, &GeneratorHistoryDialog::on_clear_all_clicked);
    connect(close_btn_, &QPushButton::clicked,
            this, &GeneratorHistoryDialog::on_close_clicked);
    // 头部 X Close按钮也调用 close_dialog（与 close_btn 同语义）
    connect(close_btn, &QPushButton::clicked, this, &QDialog::close);
    // 空状态按钮：emit 信号 + accept Close对话框，由父窗口切换到 GeneratorView
    connect(empty_action_button_, &QPushButton::clicked, this, [this]() {
        emit generate_requested();
        accept();
    });

    // 初次加载
    populate_table();
}

// ---------------------------------------------------------------------------
// 加载与渲染
// ---------------------------------------------------------------------------

void GeneratorHistoryDialog::reload() {
    populate_table();
}

void GeneratorHistoryDialog::populate_table() {
    records_.clear();
    table_->setRowCount(0);

    if (!client_) {
        set_status(tr("Not connected to the service"), /*is_error=*/true);
        return;
    }

    auto result = client_->list_generated_records();
    if (!result.ok()) {
        set_status(QString::fromStdString(result.error().what()), /*is_error=*/true);
        return;
    }

    records_ = std::move(result.value().records);

    table_->setRowCount(static_cast<int>(records_.size()));
    for (int row = 0; row < static_cast<int>(records_.size()); ++row) {
        const auto& rec = records_[row];
        // # 行号（从 1 起，按Time倒序）
        auto* idx_item = new QTableWidgetItem(QString::number(row + 1));
        idx_item->setTextAlignment(Qt::AlignCenter);
        table_->setItem(row, 0, idx_item);

        // Time
        auto* time_item = new QTableWidgetItem(format_time(rec.created_at));
        time_item->setTextAlignment(Qt::AlignCenter);
        table_->setItem(row, 1, time_item);

        // Length
        auto* len_item = new QTableWidgetItem(QString::number(rec.length));
        len_item->setTextAlignment(Qt::AlignCenter);
        table_->setItem(row, 2, len_item);

        // Password（默认遮罩）
        auto* pwd_item = new QTableWidgetItem(masked_password(rec.length));
        // 用户不应能Copy遮罩文本：Disable selectable 即可
        pwd_item->setFlags(pwd_item->flags() & ~Qt::ItemIsEditable);
        // mono 字体确保Password字符等宽对齐，长Password可读性更好
        QFont mono_font(QStringLiteral("Consolas"));
        mono_font.setStyleHint(QFont::Monospace);
        pwd_item->setFont(mono_font);
        table_->setItem(row, 3, pwd_item);

        // Actions（Copy / Delete）
        install_action_widget(row, rec.id);
    }

    refresh_password_cells();

    if (records_.empty()) {
        set_status(tr("No records"), /*is_error=*/false);
        if (table_) table_->hide();
        if (empty_label_) empty_label_->show();
        if (empty_action_button_) empty_action_button_->show();
    } else {
        set_status(tr("%1 records").arg(records_.size()),
                   /*is_error=*/false);
        if (table_) table_->show();
        if (empty_label_) empty_label_->hide();
        if (empty_action_button_) empty_action_button_->hide();
    }
}

void GeneratorHistoryDialog::refresh_password_cells() {
    if (!table_) return;
    const bool show = show_password_check_ && show_password_check_->isChecked();
    for (int row = 0; row < static_cast<int>(records_.size()); ++row) {
        QString text = show ? QString::fromStdString(records_[row].password)
                           : masked_password(records_[row].length);
        auto* item = table_->item(row, 3);
        if (item) {
            item->setText(text);
            // 仅在Show password时设 tooltip 展示完整Password，遮罩状态下不设
            item->setToolTip(show ? text : QString());
        }
    }
}

void GeneratorHistoryDialog::install_action_widget(int row, int64_t record_id) {
    auto* widget = new QWidget(table_);
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(6);

    auto* copy_btn = new QPushButton(widget);
    copy_btn->setIcon(tinted_icon(QStringLiteral(":/icons/copy.svg"), IconRole::Normal));
    copy_btn->setIconSize(QSize(14, 14));
    copy_btn->setToolTip(tr("Copy password"));
    copy_btn->setCursor(Qt::PointingHandCursor);
    copy_btn->setFixedSize(32, 32);
    copy_btn->setProperty("cssClass", QStringLiteral("icon"));
    layout->addWidget(copy_btn);

    auto* del_btn = new QPushButton(widget);
    del_btn->setIcon(tinted_icon(QStringLiteral(":/icons/trash-2.svg"), IconRole::Danger));
    del_btn->setIconSize(QSize(14, 14));
    del_btn->setToolTip(tr("Delete this record"));
    del_btn->setCursor(Qt::PointingHandCursor);
    del_btn->setFixedSize(32, 32);
    del_btn->setProperty("cssClass", QStringLiteral("icon"));
    layout->addWidget(del_btn);

    // 捕获 record_id 用于回调
    connect(copy_btn, &QPushButton::clicked, this, [this, record_id]() {
        // 从 records_ Medium找到对应Password
        for (const auto& rec : records_) {
            if (rec.id == record_id) {
                copy_secure_to_clipboard(QString::fromStdString(rec.password));
                Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
                set_status(tr("Copied. Clipboard clears in 30 seconds."),
                           /*is_error=*/false);
                return;
            }
        }
    });
    connect(del_btn, &QPushButton::clicked, this, [this, record_id]() {
        delete_record(record_id);
    });

    table_->setCellWidget(row, 4, widget);
}

void GeneratorHistoryDialog::delete_record(int64_t record_id) {
    if (!client_) return;
    auto result = client_->remove_generated_record(record_id);
    if (!result.ok()) {
        QMessageBox::warning(this, tr("Delete failed"),
            QString::fromStdString(result.error().what()));
        return;
    }
    populate_table();
}

void GeneratorHistoryDialog::set_status(const QString& message, bool is_error) {
    if (!status_label_) return;
    status_label_->setText(message);
    // 切换 cssClass 以反映Success / 错误状态
    status_label_->setProperty("cssClass",
        is_error ? QStringLiteral("error") : QStringLiteral("caption"));
    // unpolish/polish 让 QSS 重新应用
    status_label_->style()->unpolish(status_label_);
    status_label_->style()->polish(status_label_);
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void GeneratorHistoryDialog::on_show_password_toggled(bool checked) {
    (void)checked;
    refresh_password_cells();
}

void GeneratorHistoryDialog::on_refresh_clicked() {
    populate_table();
}

void GeneratorHistoryDialog::on_clear_all_clicked() {
    if (records_.empty()) return;
    const auto ret = QMessageBox::question(
        this, tr("Clear all records"),
        tr("Delete all %1 generated records? This cannot be undone.")
            .arg(records_.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    if (!client_) return;
    auto result = client_->clear_generated_records();
    if (!result.ok()) {
        QMessageBox::warning(this, tr("Clear failed"),
            QString::fromStdString(result.error().what()));
        return;
    }
    populate_table();
}

void GeneratorHistoryDialog::on_close_clicked() {
    close();
}

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// GeneratorView.cpp
//
// Yuli Vault PasswordGenerator视图实现（新设计）。
// 640px 居Medium卡片 + fieldLabel 字段Tags + inputField 容器 + Strong度条。
// =============================================================================
#include "GeneratorView.h"
#include "ErrorMessages.h"
#include "IconKit.h"
#include "IpcClient.h"
#include "StrengthUtil.h"
#include "Toast.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

namespace yuli::vault::ui {

namespace {

// Strong度等级判定与文案统一通过 StrengthUtil 提供（按 core::StrengthLevel 输入）。

}  // namespace

GeneratorView::GeneratorView(IpcClient* client, QWidget* parent)
    : QWidget(parent), client_(client)
{
    build_ui();
}

GeneratorView::~GeneratorView() = default;

void GeneratorView::build_ui() {
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
    auto* title = new QLabel(tr("Password generator"), card);
    title->setProperty("cssClass", QStringLiteral("sectionTitle"));
    card_layout->addWidget(title);

    auto* subtitle = new QLabel(
        tr("Choose a charset and length, then generate a strong password."), card);
    subtitle->setProperty("cssClass", QStringLiteral("muted"));
    card_layout->addWidget(subtitle);

    card_layout->addSpacing(24);

    // ── Length ──
    auto* length_label = new QLabel(tr("Length"), card);
    length_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(length_label);
    card_layout->addSpacing(6);

    length_spin_ = new QSpinBox(card);
    length_spin_->setRange(4, 128);
    length_spin_->setValue(16);
    length_spin_->setFixedHeight(40);
    length_spin_->setMinimumWidth(160);
    length_spin_->setMaximumWidth(200);
    auto* length_row = new QHBoxLayout();
    length_row->setContentsMargins(0, 0, 0, 0);
    length_row->setSpacing(0);
    length_row->addWidget(length_spin_);
    length_row->addStretch(1);
    card_layout->addLayout(length_row);

    card_layout->addSpacing(16);

    // ── Character set（扁平 checkbox 列） ──
    auto* charset_label = new QLabel(tr("Character set"), card);
    charset_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(charset_label);
    card_layout->addSpacing(6);

    auto* charset_col = new QVBoxLayout();
    charset_col->setContentsMargins(0, 0, 0, 0);
    charset_col->setSpacing(8);

    upper_check_ = new QCheckBox(tr("Uppercase A-Z"), card);
    upper_check_->setChecked(true);
    charset_col->addWidget(upper_check_);

    lower_check_ = new QCheckBox(tr("Lowercase a-z"), card);
    lower_check_->setChecked(true);
    charset_col->addWidget(lower_check_);

    digits_check_ = new QCheckBox(tr("Digits 0-9"), card);
    digits_check_->setChecked(true);
    charset_col->addWidget(digits_check_);

    symbols_check_ = new QCheckBox(tr("Symbols !@#$%^&*..."), card);
    symbols_check_->setChecked(true);
    charset_col->addWidget(symbols_check_);

    exclude_ambiguous_check_ = new QCheckBox(
        tr("Exclude similar characters (i l 1 L o 0 O)"), card);
    charset_col->addWidget(exclude_ambiguous_check_);

    card_layout->addLayout(charset_col);

    card_layout->addSpacing(16);

    // ── Custom characters（inputField 容器 + plus 图标） ──
    auto* custom_label = new QLabel(tr("Custom characters"), card);
    custom_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(custom_label);
    card_layout->addSpacing(6);

    auto* custom_container = new QFrame(card);
    custom_container->setFixedHeight(40);
    custom_container->setProperty("cssClass", QStringLiteral("inputField"));
    auto* custom_layout = new QHBoxLayout(custom_container);
    custom_layout->setContentsMargins(0, 0, 0, 0);
    custom_layout->setSpacing(0);

    auto* custom_icon = new QLabel(custom_container);
    custom_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/plus.svg"), IconRole::Normal, QSize(18, 18)));
    custom_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    custom_icon->setFixedSize(36, 40);
    custom_icon->setAlignment(Qt::AlignCenter);
    custom_layout->addWidget(custom_icon);

    custom_chars_edit_ = new QLineEdit(custom_container);
    custom_chars_edit_->setProperty("cssClass", QStringLiteral("inlineEdit"));
    custom_chars_edit_->setPlaceholderText(tr("Optional extra characters"));
    custom_layout->addWidget(custom_chars_edit_, 1);
    card_layout->addWidget(custom_container);

    card_layout->addSpacing(20);

    // ── Generate按钮（primary，全宽） ──
    generate_button_ = new QPushButton(card);
    generate_button_->setIcon(tinted_icon(QStringLiteral(":/icons/wand-2.svg"), IconRole::OnPrimary));
    generate_button_->setIconSize(QSize(16, 16));
    generate_button_->setText(tr("Generate password"));
    generate_button_->setCursor(Qt::PointingHandCursor);
    generate_button_->setFixedHeight(40);
    generate_button_->setProperty("cssClass", QStringLiteral("primary"));
    card_layout->addWidget(generate_button_);

    card_layout->addSpacing(20);

    // 分隔线
    auto* divider = new QFrame(card);
    divider->setFixedHeight(1);
    divider->setProperty("cssClass", QStringLiteral("divider"));
    card_layout->addWidget(divider);
    card_layout->addSpacing(16);

    // ── Result（inputField 容器 + key 图标 + 内嵌Copy按钮） ──
    auto* result_label = new QLabel(tr("Result"), card);
    result_label->setProperty("cssClass", QStringLiteral("fieldLabel"));
    card_layout->addWidget(result_label);
    card_layout->addSpacing(6);

    // 内嵌Copy按钮
    copy_button_ = new QPushButton(card);
    copy_button_->setIcon(tinted_icon(QStringLiteral(":/icons/copy.svg"), IconRole::Normal));
    copy_button_->setIconSize(QSize(16, 16));
    copy_button_->setCursor(Qt::PointingHandCursor);
    copy_button_->setFixedSize(36, 40);
    copy_button_->setToolTip(tr("Copy to clipboard"));
    copy_button_->setEnabled(false);
    copy_button_->setProperty("cssClass", QStringLiteral("inlineBtn"));

    auto* result_container = new QFrame(card);
    result_container->setFixedHeight(40);
    result_container->setProperty("cssClass", QStringLiteral("inputField"));
    auto* result_layout = new QHBoxLayout(result_container);
    result_layout->setContentsMargins(0, 0, 0, 0);
    result_layout->setSpacing(0);

    auto* result_icon = new QLabel(result_container);
    result_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/key-round.svg"), IconRole::Normal, QSize(18, 18)));
    result_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    result_icon->setFixedSize(36, 40);
    result_icon->setAlignment(Qt::AlignCenter);
    result_layout->addWidget(result_icon);

    result_edit_ = new QLineEdit(result_container);
    result_edit_->setReadOnly(true);
    result_edit_->setProperty("cssClass", QStringLiteral("inlineEdit"));
    result_edit_->setPlaceholderText(tr("Generated password appears here"));
    result_layout->addWidget(result_edit_, 1);
    result_layout->addWidget(copy_button_);
    card_layout->addWidget(result_container);

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

    scroll_layout->addWidget(card, 0, Qt::AlignCenter);
    scroll_layout->addStretch(1);  // 底部弹性

    scroll->setWidget(scroll_content);
    outer_layout->addWidget(scroll);

    // 信号槽
    connect(generate_button_, &QPushButton::clicked,
            this, &GeneratorView::on_generate_clicked);
    connect(copy_button_, &QPushButton::clicked,
            this, &GeneratorView::on_copy_clicked);
}

void GeneratorView::on_generate_clicked() {
    if (generating_) return;
    if (!client_) return;

    core::PasswordGeneratorOptions options;
    options.length = static_cast<size_t>(length_spin_->value());
    options.use_uppercase = upper_check_->isChecked();
    options.use_lowercase = lower_check_->isChecked();
    options.use_digits = digits_check_->isChecked();
    options.use_symbols = symbols_check_->isChecked();
    options.exclude_ambiguous = exclude_ambiguous_check_->isChecked();
    options.custom_chars = custom_chars_edit_->text().toStdString();

    // 至少需要一种Character set
    if (!options.use_uppercase && !options.use_lowercase &&
        !options.use_digits && !options.use_symbols &&
        options.custom_chars.empty()) {
        result_edit_->setText(QString());
        copy_button_->setEnabled(false);
        strength_bar_->setValue(0);
        set_strength_label(tr("Select at least one character set."),
                           QStringLiteral("error"));
        return;
    }

    generating_ = true;
    generate_button_->setEnabled(false);
    generate_button_->setText(tr("Generating…"));

    auto* watcher = new QFutureWatcher<core::Result<protocol::GeneratePasswordResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::GeneratePasswordResponse>>::finished,
            this, [this, watcher]() {
        generating_ = false;
        generate_button_->setEnabled(true);
        generate_button_->setText(tr("Generate password"));

        auto result = watcher->result();
        if (result.ok()) {
            const QString password = QString::fromStdString(result.value().password);
            result_edit_->setText(password);
            copy_button_->setEnabled(true);
            estimate_strength_async(password);
            emit password_generated(password);
        } else {
            result_edit_->setText(QString());
            copy_button_->setEnabled(false);
            strength_bar_->setValue(0);
            strength_bar_->setProperty("strength", QStringLiteral("weak"));
            strength_bar_->style()->unpolish(strength_bar_);
            strength_bar_->style()->polish(strength_bar_);
            set_strength_label(tr("Strength: —"),
                               QStringLiteral("caption"));
            Toast::show(this, friendly_message(result.error()));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->generate_password_async(options));
}

void GeneratorView::on_copy_clicked() {
    const QString password = result_edit_->text();
    if (password.isEmpty()) return;
    copy_secure_to_clipboard(password);
    Toast::show(this, tr("Copied. Clipboard clears in 30 seconds."));
}

void GeneratorView::estimate_strength_async(const QString& password) {
    if (!client_ || password.isEmpty()) {
        strength_bar_->setValue(0);
        strength_bar_->setProperty("strength", QStringLiteral("weak"));
        strength_bar_->style()->unpolish(strength_bar_);
        strength_bar_->style()->polish(strength_bar_);
        set_strength_label(tr("Strength: —"),
                           QStringLiteral("caption"));
        return;
    }

    const std::string pwd = password.toStdString();
    auto* watcher = new QFutureWatcher<core::Result<protocol::EstimateStrengthResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::EstimateStrengthResponse>>::finished,
            this, [this, watcher]() {
        auto r = watcher->result();
        if (r.ok()) {
            const auto& estimate = r.value().estimate;
            const int pct = (estimate.bits >= 128) ? 100 : (estimate.bits * 100 / 128);
            strength_bar_->setValue(pct);
            // 通过 strength 属性让 QSS 接管 chunk 颜色
            strength_bar_->setProperty("strength", strength_qss_key(estimate.level));
            strength_bar_->style()->unpolish(strength_bar_);
            strength_bar_->style()->polish(strength_bar_);
            QString label = tr("Strength: %1 (%2 bits)")
                                .arg(strength_text(estimate.level))
                                .arg(estimate.bits);
            if (!estimate.warnings.empty()) {
                QStringList parts;
                for (const auto& w : estimate.warnings) {
                    parts << translate_strength_warning(QString::fromStdString(w));
                }
                strength_label_->setToolTip(parts.join(QStringLiteral("\n")));
            } else if (strength_label_) {
                strength_label_->setToolTip(QString());
            }
            set_strength_label(label, strength_label_class(estimate.level));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->estimate_strength_async(pwd));
}

void GeneratorView::set_strength_label(const QString& text, const QString& css_class) {
    if (!strength_label_) return;
    strength_label_->setText(text);
    strength_label_->setProperty("cssClass", css_class);
    strength_label_->style()->unpolish(strength_label_);
    strength_label_->style()->polish(strength_label_);
}

}  // namespace yuli::vault::ui

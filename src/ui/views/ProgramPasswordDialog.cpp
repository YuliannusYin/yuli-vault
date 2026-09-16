// coding: utf-8
// =============================================================================
// ProgramPasswordDialog.cpp
//
// Yuli Vault Program password管理对话框实现（新设计）。
// 单 Tab 切换弹窗（480px 居Medium + 模态遮罩），三种模式通过分段控件切换。
// =============================================================================
#include "ProgramPasswordDialog.h"
#include "IpcClient.h"
#include "IconKit.h"
#include "StrengthUtil.h"
#include "Toast.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace yuli::vault::ui {

namespace {

// 样式由 QSS Medium对应的 cssClass 提供（inputField / inlineEdit / inlineIcon / inlineBtn）
// Strong度等级判定与文案统一通过 StrengthUtil 提供。

}  // namespace

ProgramPasswordDialog::ProgramPasswordDialog(IpcClient* client,
                                             Mode initial_mode,
                                             QWidget* parent)
    : QDialog(parent), client_(client), current_mode_(initial_mode)
{
    // 模态 + 无原生标题栏，自定义遮罩 + 卡片
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(tr("Program password"));

    // 自动覆盖父窗口大小
    // parent 通常是 SettingsView（MainWindow 的子 widget，非 top-level），
    // 其 geometry() 返回的是相对于父 widget 的坐标，不能直接用于 top-level QDialog。
    // 用 window()->geometry() 拿到顶层主窗口的屏幕坐标。
    if (parent) {
        setGeometry(parent->window()->geometry());
    }

    build_ui();
    apply_mode(current_mode_);
}

ProgramPasswordDialog::~ProgramPasswordDialog() = default;

void ProgramPasswordDialog::closeEvent(QCloseEvent* event) {
    // 用 rejected_emitted_ 守卫防止重复 emit：
    // ESC → keyPressEvent → close_dialog → emit rejected + reject()
    //      → reject() 触发 hide → close → closeEvent，此处不再重复 emit。
    if (!succeeded_ && !rejected_emitted_) {
        rejected_emitted_ = true;
        emit rejected();
    }
    QDialog::closeEvent(event);
}

void ProgramPasswordDialog::keyPressEvent(QKeyEvent* event) {
    // ESC 键Close对话框
    if (event->key() == Qt::Key_Escape) {
        close_dialog();
        return;
    }
    QDialog::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// UI 构建
// ---------------------------------------------------------------------------

void ProgramPasswordDialog::build_ui() {
    // 根布局：半透明遮罩
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* overlay = new QFrame(this);
    overlay->setProperty("cssClass", QStringLiteral("modalOverlay"));
    auto* overlay_layout = new QVBoxLayout(overlay);
    overlay_layout->setContentsMargins(16, 16, 16, 16);
    overlay_layout->setAlignment(Qt::AlignCenter);

    // ── 480px 卡片 ──
    auto* card = new QFrame(overlay);
    card->setFixedWidth(480);
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

    auto* key_icon = new QLabel(header);
    key_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/key-round.svg"),
                                      IconRole::Normal, QSize(18, 18)));
    key_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    header_layout->addWidget(key_icon);

    auto* title = new QLabel(tr("Program password"), header);
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

    // ── 模式分段控件（紧贴头部下方） ──
    auto* tab_container = new QFrame(card);
    tab_container->setProperty("cssClass", QStringLiteral("modalBody"));
    auto* tab_outer = new QHBoxLayout(tab_container);
    tab_outer->setContentsMargins(24, 16, 24, 0);
    tab_outer->setSpacing(0);

    auto* tab_frame = new QFrame(tab_container);
    tab_frame->setProperty("cssClass", QStringLiteral("segmented"));
    auto* tab_layout = new QHBoxLayout(tab_frame);
    tab_layout->setContentsMargins(4, 4, 4, 4);
    tab_layout->setSpacing(4);

    mode_group_ = new QButtonGroup(this);
    mode_group_->setExclusive(true);

    auto make_tab = [tab_frame](const QString& text) {
        auto* btn = new QPushButton(text, tab_frame);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(true);
        btn->setFixedHeight(32);
        btn->setProperty("cssClass", QStringLiteral("segmentedItem"));
        return btn;
    };

    tab_enable_ = make_tab(tr("Enable"));
    tab_change_ = make_tab(tr("Change"));
    tab_disable_ = make_tab(tr("Disable"));

    mode_group_->addButton(tab_enable_, static_cast<int>(Mode::Enable));
    mode_group_->addButton(tab_change_, static_cast<int>(Mode::Change));
    mode_group_->addButton(tab_disable_, static_cast<int>(Mode::Disable));

    tab_layout->addWidget(tab_enable_, 1);
    tab_layout->addWidget(tab_change_, 1);
    tab_layout->addWidget(tab_disable_, 1);

    tab_outer->addWidget(tab_frame);
    card_layout->addWidget(tab_container);

    // ── 体（表单） ──
    auto* body = new QFrame(card);
    body->setProperty("cssClass", QStringLiteral("modalBody"));
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(24, 20, 24, 20);
    body_layout->setSpacing(16);

    auto make_label = [body](const QString& text) {
        auto* lbl = new QLabel(text, body);
        lbl->setProperty("cssClass", QStringLiteral("fieldLabel"));
        return lbl;
    };

    // 辅助：构造「图标 + 输入框 + 可见性按钮」的容器
    auto make_field = [body](QFrame*& frame, QLineEdit*& edit,
                              QPushButton*& toggle_btn, const QString& icon_path,
                              const QString& placeholder) {
        frame = new QFrame(body);
        frame->setFixedHeight(40);
        frame->setProperty("cssClass", QStringLiteral("inputField"));
        auto* fl = new QHBoxLayout(frame);
        fl->setContentsMargins(0, 0, 0, 0);
        fl->setSpacing(0);

        auto* icon_lbl = new QLabel(frame);
        icon_lbl->setPixmap(tinted_pixmap(icon_path, IconRole::Normal, QSize(18, 18)));
        icon_lbl->setProperty("cssClass", QStringLiteral("inlineIcon"));
        icon_lbl->setFixedSize(36, 40);
        icon_lbl->setAlignment(Qt::AlignCenter);
        fl->addWidget(icon_lbl);

        edit = new QLineEdit(frame);
        edit->setEchoMode(QLineEdit::Password);
        edit->setPlaceholderText(placeholder);
        edit->setProperty("cssClass", QStringLiteral("inlineEdit"));
        fl->addWidget(edit, 1);

        toggle_btn = new QPushButton(frame);
        toggle_btn->setIcon(tinted_icon(QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
        toggle_btn->setIconSize(QSize(18, 18));
        toggle_btn->setCursor(Qt::PointingHandCursor);
        toggle_btn->setFixedSize(40, 40);
        toggle_btn->setProperty("cssClass", QStringLiteral("inlineBtn"));
        fl->addWidget(toggle_btn);
    };

    // 当前Program password
    body_layout->addWidget(make_label(tr("Current program password")));
    make_field(current_frame_, current_edit_, toggle_current_btn_,
               QStringLiteral(":/icons/lock.svg"),
               tr("Enter the current program password"));
    body_layout->addWidget(current_frame_);

    // 新Program password
    body_layout->addWidget(make_label(tr("New program password")));
    make_field(new_frame_, new_edit_, toggle_new_btn_,
               QStringLiteral(":/icons/key.svg"),
               tr("Enter a new program password"));
    body_layout->addWidget(new_frame_);

    // Strong度行：4 段 + 文字
    auto* strength_row = new QHBoxLayout();
    strength_row->setContentsMargins(0, 4, 0, 0);
    strength_row->setSpacing(8);
    strength_bar_ = new QFrame(body);
    strength_bar_->setFixedHeight(6);
    strength_bar_->setProperty("cssClass", QStringLiteral("strengthBar"));
    auto* seg_layout = new QHBoxLayout(strength_bar_);
    seg_layout->setContentsMargins(0, 0, 0, 0);
    seg_layout->setSpacing(4);
    // 创建 4 个分段（QLabel），初始用空 cssClass（暗色），on 时切换
    for (int i = 0; i < 4; ++i) {
        auto* seg = new QLabel(strength_bar_);
        seg->setProperty("cssClass", QStringLiteral("strengthSeg"));
        seg_layout->addWidget(seg, 1);
    }
    strength_row->addWidget(strength_bar_, 1);
    strength_label_ = new QLabel(QStringLiteral("-"), body);
    strength_label_->setProperty("cssClass", QStringLiteral("caption"));
    strength_label_->setFixedWidth(80);
    strength_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    strength_row->addWidget(strength_label_);
    body_layout->addLayout(strength_row);

    // Confirm new password
    body_layout->addWidget(make_label(tr("Confirm new password")));
    make_field(confirm_frame_, confirm_edit_, toggle_confirm_btn_,
               QStringLiteral(":/icons/shield-check.svg"),
               tr("Re-enter the new password"));
    body_layout->addWidget(confirm_frame_);

    // 错误提示
    error_label_ = new QLabel(body);
    error_label_->setWordWrap(true);
    error_label_->setProperty("cssClass", QStringLiteral("error"));
    body_layout->addWidget(error_label_);

    // 提示条（info box）
    auto* info_box = new QFrame(body);
    info_box->setProperty("cssClass", QStringLiteral("infoBox"));
    auto* info_layout = new QHBoxLayout(info_box);
    info_layout->setContentsMargins(12, 10, 12, 10);
    info_layout->setSpacing(10);
    auto* info_icon = new QLabel(info_box);
    info_icon->setPixmap(tinted_pixmap(QStringLiteral(":/icons/info.svg"), IconRole::Normal, QSize(16, 16)));
    info_icon->setProperty("cssClass", QStringLiteral("inlineIcon"));
    info_layout->addWidget(info_icon, 0, Qt::AlignTop);
    auto* info_text = new QLabel(
        tr("The program password derives the encryption key (Argon2id). Changing it re-encrypts the vault. Keep the new password safe."),
        info_box);
    info_text->setWordWrap(true);
    info_text->setProperty("cssClass", QStringLiteral("caption"));
    info_layout->addWidget(info_text, 1);
    body_layout->addWidget(info_box);

    card_layout->addWidget(body, 1);

    // ── 尾部（64px 高） ──
    auto* footer = new QFrame(card);
    footer->setFixedHeight(64);
    footer->setProperty("cssClass", QStringLiteral("modalFooter"));
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(24, 0, 24, 0);
    footer_layout->setSpacing(12);
    footer_layout->addStretch(1);

    auto* cancel_btn = new QPushButton(tr("Cancel"), footer);
    cancel_btn->setCursor(Qt::PointingHandCursor);
    cancel_btn->setFixedHeight(40);
    cancel_btn->setMinimumWidth(80);
    cancel_btn->setProperty("cssClass", QStringLiteral("outline"));
    footer_layout->addWidget(cancel_btn);

    submit_btn_ = new QPushButton(footer);
    submit_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/check.svg"), IconRole::OnPrimary));
    submit_btn_->setIconSize(QSize(18, 18));
    submit_btn_->setCursor(Qt::PointingHandCursor);
    submit_btn_->setFixedHeight(40);
    submit_btn_->setMinimumWidth(120);
    submit_btn_->setProperty("cssClass", QStringLiteral("primary"));
    footer_layout->addWidget(submit_btn_);
    card_layout->addWidget(footer);

    overlay_layout->addWidget(card);
    root_layout->addWidget(overlay);

    // 信号槽
    connect(mode_group_, &QButtonGroup::idClicked,
            this, &ProgramPasswordDialog::on_mode_tab_clicked);
    connect(close_btn, &QPushButton::clicked,
            this, &ProgramPasswordDialog::on_cancel_clicked);
    connect(cancel_btn, &QPushButton::clicked,
            this, &ProgramPasswordDialog::on_cancel_clicked);
    connect(submit_btn_, &QPushButton::clicked,
            this, &ProgramPasswordDialog::on_submit_clicked);

    connect(toggle_current_btn_, &QPushButton::clicked,
            this, &ProgramPasswordDialog::on_toggle_current_clicked);
    connect(toggle_new_btn_, &QPushButton::clicked,
            this, &ProgramPasswordDialog::on_toggle_new_clicked);
    connect(toggle_confirm_btn_, &QPushButton::clicked,
            this, &ProgramPasswordDialog::on_toggle_confirm_clicked);
    connect(new_edit_, &QLineEdit::textChanged,
            this, &ProgramPasswordDialog::on_new_password_changed);

    // Strong度评估 debounce：每次输入触发 IPC 会卡顿，用 300ms 计时器合并连续输入。
    strength_timer_ = new QTimer(this);
    strength_timer_->setSingleShot(true);
    connect(strength_timer_, &QTimer::timeout, this, [this]() {
        if (new_edit_) update_strength(new_edit_->text());
    });
}

// ---------------------------------------------------------------------------
// 模式切换
// ---------------------------------------------------------------------------

void ProgramPasswordDialog::apply_mode(Mode mode) {
    current_mode_ = mode;
    // 同步 Tab 选Medium状态
    QSignalBlocker b(mode_group_);
    auto* btn = mode_group_->button(static_cast<int>(mode));
    if (btn) btn->setChecked(true);

    // 根据模式显示/隐藏字段
    switch (mode) {
        case Mode::Enable:
            current_frame_->hide();
            new_frame_->show();
            strength_bar_->show();
            strength_label_->show();
            confirm_frame_->show();
            submit_btn_->setText(tr("Enable"));
            set_error(QString());
            break;
        case Mode::Change:
            current_frame_->show();
            new_frame_->show();
            strength_bar_->show();
            strength_label_->show();
            confirm_frame_->show();
            submit_btn_->setText(tr("Save changes"));
            set_error(QString());
            break;
        case Mode::Disable:
            current_frame_->show();
            new_frame_->hide();
            strength_bar_->hide();
            strength_label_->hide();
            confirm_frame_->hide();
            submit_btn_->setText(tr("Confirm disable"));
            set_error(QString());
            break;
    }
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void ProgramPasswordDialog::on_mode_tab_clicked(int idx) {
    apply_mode(static_cast<Mode>(idx));
}

void ProgramPasswordDialog::on_toggle_current_clicked() {
    current_visible_ = !current_visible_;
    current_edit_->setEchoMode(current_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    toggle_current_btn_->setIcon(tinted_icon(current_visible_
        ? QStringLiteral(":/icons/eye-off.svg")
        : QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
}

void ProgramPasswordDialog::on_toggle_new_clicked() {
    new_visible_ = !new_visible_;
    new_edit_->setEchoMode(new_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    toggle_new_btn_->setIcon(tinted_icon(new_visible_
        ? QStringLiteral(":/icons/eye-off.svg")
        : QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
}

void ProgramPasswordDialog::on_toggle_confirm_clicked() {
    confirm_visible_ = !confirm_visible_;
    confirm_edit_->setEchoMode(confirm_visible_ ? QLineEdit::Normal : QLineEdit::Password);
    toggle_confirm_btn_->setIcon(tinted_icon(confirm_visible_
        ? QStringLiteral(":/icons/eye-off.svg")
        : QStringLiteral(":/icons/eye.svg"), IconRole::Normal));
}

void ProgramPasswordDialog::on_new_password_changed(const QString& text) {
    (void)text;
    // debounce：重启计时器，300ms 内无新输入才真正发起Strong度评估 IPC
    if (strength_timer_) strength_timer_->start(300);
}

void ProgramPasswordDialog::on_cancel_clicked() {
    close_dialog();
}

void ProgramPasswordDialog::on_submit_clicked() {
    set_error(QString());

    if (!client_) {
        set_error(tr("Internal error: IPC client is unavailable."));
        return;
    }

    switch (current_mode_) {
        case Mode::Enable: {
            const std::string password = new_edit_->text().toStdString();
            if (password.empty()) {
                set_error(tr("Program password cannot be empty."));
                return;
            }
            const std::string confirm = confirm_edit_->text().toStdString();
            if (password != confirm) {
                set_error(tr("The passwords do not match."));
                return;
            }
            auto result = client_->enable_program_password(password);
            if (result.ok() && result.value().success) {
                succeeded_ = true;
                Toast::show(this, tr("Program password enabled"));
                emit succeeded();
                accept();
            } else {
                QString msg = result.ok()
                    ? QString::fromStdString(result.value().error_message)
                    : QString::fromStdString(result.error().what());
                set_error(msg.isEmpty()
                              ? tr("Could not enable the program password.")
                              : tr("Enable failed: %1").arg(msg));
            }
            break;
        }
        case Mode::Change: {
            const std::string old_pw = current_edit_->text().toStdString();
            const std::string new_pw = new_edit_->text().toStdString();
            if (old_pw.empty() || new_pw.empty()) {
                set_error(tr("Password cannot be empty."));
                return;
            }
            const std::string confirm = confirm_edit_->text().toStdString();
            if (new_pw != confirm) {
                set_error(tr("The new passwords do not match."));
                return;
            }
            if (old_pw == new_pw) {
                set_error(tr("The new password must differ from the old one."));
                return;
            }
            auto result = client_->change_program_password(old_pw, new_pw);
            if (result.ok() && result.value().success) {
                succeeded_ = true;
                Toast::show(this, tr("Program password changed"));
                emit succeeded();
                accept();
            } else {
                QString msg = result.ok()
                    ? QString::fromStdString(result.value().error_message)
                    : QString::fromStdString(result.error().what());
                set_error(msg.isEmpty()
                              ? tr("Could not change the program password.")
                              : tr("Change failed: %1").arg(msg));
            }
            break;
        }
        case Mode::Disable: {
            const std::string password = current_edit_->text().toStdString();
            if (password.empty()) {
                set_error(tr("Enter the current program password."));
                return;
            }
            // Task 21: 二次确认——Disable后所有Password将以明文Storage，需用户再次确认
            const auto ret = QMessageBox::warning(
                this,
                tr("Confirm disable"),
                tr("All items will be stored unencrypted. Continue?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No
            );
            if (ret != QMessageBox::Yes) {
                return;  // 用户Cancel，不执行Disable
            }
            auto result = client_->disable_program_password(password);
            if (result.ok() && result.value().success) {
                succeeded_ = true;
                Toast::show(this, tr("Program password disabled"));
                emit succeeded();
                accept();
            } else {
                QString msg = result.ok()
                    ? QString::fromStdString(result.value().error_message)
                    : QString::fromStdString(result.error().what());
                set_error(msg.isEmpty()
                              ? tr("Could not disable the program password.")
                              : tr("Disable failed: %1").arg(msg));
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 辅助
// ---------------------------------------------------------------------------

void ProgramPasswordDialog::update_strength(const QString& password) {
    if (!strength_bar_ || !strength_label_ || !client_) return;

    core::StrengthEstimate estimate;
    if (!password.isEmpty()) {
        auto result = client_->estimate_strength(password.toStdString());
        if (result.ok()) estimate = result.value().estimate;
    }

    const int seg_count = password.isEmpty() ? 0 : strength_segments(estimate.level);
    const QString key = strength_qss_key(estimate.level);

    // 找到 strength_bar_ 下的 4 个 QLabel 分段
    auto segs = strength_bar_->findChildren<QLabel*>();
    for (int i = 0; i < segs.size(); ++i) {
        // 点亮的前 seg_count 段Settings strength 属性，其余Clear属性恢复默认暗色
        if (i < seg_count) {
            segs[i]->setProperty("strength", key);
        } else {
            segs[i]->setProperty("strength", QVariant());
        }
        segs[i]->style()->unpolish(segs[i]);
        segs[i]->style()->polish(segs[i]);
    }

    QString label_class;
    if (password.isEmpty()) {
        strength_label_->setText(QStringLiteral("-"));
        label_class = QStringLiteral("caption");
    } else {
        strength_label_->setText(
            tr("%1 (%2 bits)").arg(strength_text(estimate.level)).arg(estimate.bits));
        label_class = strength_label_class(estimate.level);
    }
    strength_label_->setProperty("cssClass", label_class);
    strength_label_->style()->unpolish(strength_label_);
    strength_label_->style()->polish(strength_label_);
}

void ProgramPasswordDialog::set_error(const QString& message) {
    if (error_label_) error_label_->setText(message);
}

void ProgramPasswordDialog::close_dialog() {
    if (!succeeded_ && !rejected_emitted_) {
        rejected_emitted_ = true;
        emit rejected();
    }
    reject();
}

}  // namespace yuli::vault::ui

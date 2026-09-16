// coding: utf-8
// =============================================================================
// SettingsView.cpp
//
// Yuli Vault Settings视图实现（新设计）。
// 卡片分节布局：Security / Appearance / Storage / About / Danger zone。
// =============================================================================
#include "SettingsView.h"
#include "ErrorMessages.h"
#include "IpcClient.h"
#include "ProgramPasswordDialog.h"
#include "GeneratorHistoryDialog.h"
#include "Theme.h"
#include "AppSettings.h"
#include "IconKit.h"
#include "Toast.h"
#include "Version.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QString>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

namespace yuli::vault::ui {

namespace {

/// 应用Version号（由 CMake configure_file 从 Version.h.in Generate，取自顶层 project() VERSION）。
constexpr const char* kAppVersion = YULI_VAULT_VERSION;

/// GitHub Project home URL。
constexpr const char* kGitHubUrl = "https://github.com/YuliannusYin/yuli-vault";

/// License URL。
constexpr const char* kLicenseUrl = "https://opensource.org/licenses/MIT";

/// Returns %APPDATA%\YuliVault (matches the service data directory).
QString data_storage_path() {
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) {
        return QDir(appdata).filePath(QStringLiteral("YuliVault"));
    }
    const QStringList locs = QStandardPaths::standardLocations(
        QStandardPaths::AppDataLocation);
    if (!locs.isEmpty()) {
        return locs.first();
    }
    return QStringLiteral("YuliVault");
}

}  // namespace

SettingsView::SettingsView(IpcClient* client, QWidget* parent)
    : QWidget(parent), client_(client)
{
    setObjectName(QStringLiteral("settingsView"));
    build_ui();
    sync_theme_segment();
    // 监听Theme切换：顶栏切换Theme后同步分段控件选Medium项，
    // 否则用户OpenSettings页会看到旧的选Medium状态。
    if (auto* theme = Theme::instance()) {
        connect(theme, &Theme::theme_changed, this, &SettingsView::sync_theme_segment);
    }
}

SettingsView::~SettingsView() = default;

// ---------------------------------------------------------------------------
// UI 构建
// ---------------------------------------------------------------------------

void SettingsView::build_ui() {
    // 外层：可滚动区域
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);

    auto* content = new QWidget(scroll);
    // 不设 setStyleSheet("background: transparent;")：widget 级样式表优先级
    // 高于 qss 文件，无选择器的 "background: transparent" 会级联到所有子 widget，
    // 覆盖 card 的 background-color。content 继承全局 QWidget 底色即可（与
    // 右侧栏一致，scroll 已设 transparent 让滚动区不遮挡）。
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(24, 24, 24, 24);
    content_layout->setSpacing(16);

    // 水平居Medium容器：左右 stretch + center_container（max-width 720px）。
    // 垂直方向从顶部开始排列（仅水平居Medium，不垂直居Medium），内容多时滚动。
    auto* hbox = new QHBoxLayout();
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);
    auto* center_container = new QWidget(content);
    center_container->setMaximumWidth(720);
    auto* center_layout = new QVBoxLayout(center_container);
    center_layout->setContentsMargins(0, 0, 0, 0);
    center_layout->setSpacing(16);
    hbox->addStretch(1);
    hbox->addWidget(center_container);
    hbox->addStretch(1);
    content_layout->addLayout(hbox);

    // ── Security section ──
    {
        auto* section = make_section(QStringLiteral(":/icons/shield-check.svg"),
                                     tr("Security"), false, center_container,
                                     icon_color(IconRole::Success));
        center_layout->addWidget(section);
        auto* section_layout = qobject_cast<QVBoxLayout*>(section->layout());

        // Program password行
        pp_badge_ = new QLabel(section);
        pp_badge_->setProperty("cssClass", QStringLiteral("badgeSuccess"));
        // pp_desc_ 作为本行描述文本：通过 add_row 的 description 参数安置，
        // 避免无 layout 管理时飘在 card 左上角。
        manage_pp_btn_ = new QPushButton(section);
        manage_pp_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/settings-2.svg"), IconRole::Normal));
        manage_pp_btn_->setIconSize(QSize(16, 16));
        manage_pp_btn_->setText(tr("Program password"));
        manage_pp_btn_->setCursor(Qt::PointingHandCursor);
        manage_pp_btn_->setFixedHeight(40);
        manage_pp_btn_->setProperty("cssClass", QStringLiteral("outline"));

        // 右侧组合：badge + 按钮
        auto* right_row = new QWidget(section);
        auto* right_layout = new QHBoxLayout(right_row);
        right_layout->setContentsMargins(0, 0, 0, 0);
        right_layout->setSpacing(8);
        right_layout->addWidget(pp_badge_);
        right_layout->addWidget(manage_pp_btn_);
        // pp_desc_ 通过 out_desc 回传，供 refresh_password_badge 动态更新文本
        add_row(section_layout, tr("Program password"),
                tr("Off · vault is stored in plaintext"), right_row, &pp_desc_);

        // Auto-lock行：item data 携带实际分钟数（0 = Never）
        autolock_combo_ = new QComboBox(section);
        autolock_combo_->setObjectName(QStringLiteral("settingsAutolock"));
        autolock_combo_->addItem(tr("Never"), QVariant(0));
        autolock_combo_->addItem(tr("1 minute"), QVariant(1));
        autolock_combo_->addItem(tr("5 minutes"), QVariant(5));
        autolock_combo_->addItem(tr("15 minutes"), QVariant(15));
        autolock_combo_->addItem(tr("30 minutes"), QVariant(30));
        autolock_combo_->setCurrentIndex(2);
        autolock_combo_->setFixedHeight(40);
        autolock_combo_->setMinimumWidth(120);
        // 从 QSettings 读取持久化值，找到对应 item 设为当前项。
        // 用 QSignalBlocker 防止初始 set 期间触发 currentIndexChanged。
        {
            const int persisted = settings_value(
                QStringLiteral("autolock_minutes"), 5).toInt();
            int target = 2;  // 默认 5 minutes
            for (int i = 0; i < autolock_combo_->count(); ++i) {
                if (autolock_combo_->itemData(i).toInt() == persisted) {
                    target = i;
                    break;
                }
            }
            QSignalBlocker blocker(autolock_combo_);
            autolock_combo_->setCurrentIndex(target);
        }
        add_row(section_layout, tr("Auto-lock"),
                tr("Lock the vault after idle time"), autolock_combo_);
    }

    // ── Appearance section ──
    {
        auto* section = make_section(QStringLiteral(":/icons/palette.svg"),
                                     tr("Appearance"), false, center_container);
        center_layout->addWidget(section);
        auto* section_layout = qobject_cast<QVBoxLayout*>(section->layout());
        auto* seg = build_theme_segmented();
        seg->setParent(section);
        add_row(section_layout, tr("Theme"),
                tr("Switch dark / light mode"), seg);

        // High contrast：独立开关，增StrongMedium性边框可见度
        hc_checkbox_ = new QCheckBox(section);
        hc_checkbox_->setChecked(Theme::is_high_contrast());
        hc_checkbox_->setCursor(Qt::PointingHandCursor);
        add_row(section_layout, tr("High contrast"),
                tr("Stronger borders for control outlines"), hc_checkbox_);
        connect(hc_checkbox_, &QCheckBox::toggled, this, [this](bool checked) {
            Theme::set_high_contrast(checked);
        });
        if (auto* t = Theme::instance()) {
            connect(t, &Theme::high_contrast_changed, this, [this](bool enabled) {
                QSignalBlocker b(hc_checkbox_);
                hc_checkbox_->setChecked(enabled);
            });
        }

        language_combo_ = new QComboBox(section);
        language_combo_->setObjectName(QStringLiteral("settingsLanguage"));
        language_combo_->addItem(tr("System"), static_cast<int>(UiLanguage::System));
        language_combo_->addItem(tr("English"), static_cast<int>(UiLanguage::English));
        language_combo_->addItem(QStringLiteral("简体中文"),
                                 static_cast<int>(UiLanguage::ChineseSimplified));
        language_combo_->setFixedHeight(40);
        language_combo_->setMinimumWidth(160);
        {
            const int persisted = static_cast<int>(load_ui_language());
            int target = 0;
            for (int i = 0; i < language_combo_->count(); ++i) {
                if (language_combo_->itemData(i).toInt() == persisted) {
                    target = i;
                    break;
                }
            }
            QSignalBlocker blocker(language_combo_);
            language_combo_->setCurrentIndex(target);
        }
        add_row(section_layout, tr("Language"),
                tr("System follows the OS locale. A restart applies the change."),
                language_combo_);
        connect(language_combo_, &QComboBox::currentIndexChanged,
                this, &SettingsView::on_language_changed);
    }

    // ── Generator section ──
    {
        auto* section = make_section(QStringLiteral(":/icons/wand-2.svg"),
                                     tr("Generator"), false, center_container,
                                     icon_color(IconRole::Info));
        center_layout->addWidget(section);
        auto* section_layout = qobject_cast<QVBoxLayout*>(section->layout());

        // 历史记录行：左描述动态显示「已保存 N 条记录 / No records」
        view_history_btn_ = new QPushButton(section);
        view_history_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/clock.svg"), IconRole::Normal));
        view_history_btn_->setIconSize(QSize(16, 16));
        view_history_btn_->setText(tr("View history"));
        view_history_btn_->setCursor(Qt::PointingHandCursor);
        view_history_btn_->setFixedHeight(40);
        view_history_btn_->setProperty("cssClass", QStringLiteral("outline"));
        add_row(section_layout, tr("Generated passwords"),
                tr("Loading…"), view_history_btn_, &gen_history_desc_);

        // 上限下拉：item data 携带实际数值（0 = Unlimited）
        gen_limit_combo_ = new QComboBox(section);
        gen_limit_combo_->setObjectName(QStringLiteral("settingsGenLimit"));
        gen_limit_combo_->addItem(tr("Unlimited"), QVariant(0));
        gen_limit_combo_->addItem(tr("10"), QVariant(10));
        gen_limit_combo_->addItem(tr("20"), QVariant(20));
        gen_limit_combo_->addItem(tr("50"), QVariant(50));
        gen_limit_combo_->addItem(tr("100"), QVariant(100));
        gen_limit_combo_->addItem(tr("200"), QVariant(200));
        gen_limit_combo_->setCurrentIndex(0);
        gen_limit_combo_->setFixedHeight(40);
        gen_limit_combo_->setMinimumWidth(120);
        add_row(section_layout, tr("History limit"),
                tr("Keep the last N generated passwords"), gen_limit_combo_);
    }

    // ── Storage section ──
    {
        auto* section = make_section(QStringLiteral(":/icons/database.svg"),
                                     tr("Storage"), false, center_container);
        center_layout->addWidget(section);
        auto* section_layout = qobject_cast<QVBoxLayout*>(section->layout());
        // Storage path行
        storage_path_label_ = new QLabel(data_storage_path(), section);
        storage_path_label_->setObjectName(QStringLiteral("settingsPathBox"));
        storage_path_label_->setProperty("cssClass", QStringLiteral("pathBox"));
        storage_path_label_->setFixedHeight(40);
        storage_path_label_->setMinimumWidth(240);
        open_storage_btn_ = new QPushButton(section);
        open_storage_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/folder-open.svg"), IconRole::Normal));
        open_storage_btn_->setIconSize(QSize(16, 16));
        open_storage_btn_->setText(tr("Open"));
        open_storage_btn_->setCursor(Qt::PointingHandCursor);
        open_storage_btn_->setFixedHeight(40);
        open_storage_btn_->setProperty("cssClass", QStringLiteral("outline"));

        auto* right_row1 = new QWidget(section);
        auto* right_layout1 = new QHBoxLayout(right_row1);
        right_layout1->setContentsMargins(0, 0, 0, 0);
        right_layout1->setSpacing(8);
        right_layout1->addWidget(storage_path_label_);
        right_layout1->addWidget(open_storage_btn_);
        add_row(section_layout, tr("Storage path"),
                tr("Location of vault data files"), right_row1);

        // Item count行
        entry_count_label_ = new QLabel(QStringLiteral("-"), section);
        entry_count_label_->setProperty("cssClass", QStringLiteral("fieldLabel"));
        add_row(section_layout, tr("Item count"),
                tr("Saved vault items"), entry_count_label_);
    }

    // ── About section ──
    {
        auto* section = make_section(QStringLiteral(":/icons/info.svg"),
                                     tr("About"), false, center_container);
        center_layout->addWidget(section);
        auto* section_layout = qobject_cast<QVBoxLayout*>(section->layout());
        version_value_ = new QLabel(
            tr("v%1").arg(QString::fromLatin1(kAppVersion)), section);
        version_value_->setProperty("cssClass", QStringLiteral("fieldLabel"));
        add_row(section_layout, tr("Version"),
                tr("Yuli Vault"), version_value_);

        auto* enc_value = new QLabel(tr("AES-256-GCM · Argon2id"), section);
        enc_value->setProperty("cssClass", QStringLiteral("fieldLabel"));
        add_row(section_layout, tr("Encryption"),
                tr("Data encryption and key derivation"), enc_value);

        license_btn_ = new QPushButton(section);
        license_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/external-link.svg"), IconRole::Normal));
        license_btn_->setIconSize(QSize(16, 16));
        license_btn_->setText(tr("View"));
        license_btn_->setCursor(Qt::PointingHandCursor);
        license_btn_->setFixedHeight(40);
        license_btn_->setProperty("cssClass", QStringLiteral("outline"));
        add_row(section_layout, tr("License"),
                tr("MIT License"), license_btn_);

        github_btn_ = new QPushButton(section);
        github_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/github.svg"), IconRole::Normal));
        github_btn_->setIconSize(QSize(16, 16));
        github_btn_->setText(tr("GitHub"));
        github_btn_->setCursor(Qt::PointingHandCursor);
        github_btn_->setFixedHeight(40);
        github_btn_->setProperty("cssClass", QStringLiteral("outline"));
        add_row(section_layout, tr("Project home"),
                tr("Source code and issue tracker"), github_btn_);
    }

    // ── Danger zone section ──
    {
        auto* section = make_section(QStringLiteral(":/icons/shield-off.svg"),
                                     tr("Danger zone"),
                                     /*danger=*/true, center_container);
        center_layout->addWidget(section);
        auto* section_layout = qobject_cast<QVBoxLayout*>(section->layout());
        lock_now_btn_ = new QPushButton(section);
        lock_now_btn_->setIcon(tinted_icon(QStringLiteral(":/icons/lock.svg"), IconRole::Danger));
        lock_now_btn_->setIconSize(QSize(16, 16));
        lock_now_btn_->setText(tr("Lock now"));
        lock_now_btn_->setCursor(Qt::PointingHandCursor);
        lock_now_btn_->setFixedHeight(40);
        lock_now_btn_->setProperty("cssClass", QStringLiteral("danger"));
        add_row(section_layout, tr("Lock vault"),
                tr("Lock now. The program password is required to unlock."), lock_now_btn_);
    }

    // content 不再设 maxWidth：由 center_container->setMaximumWidth(720)
    // 控制模块宽度，配合 hbox stretch 实现水平居Medium。
    scroll->setWidget(content);

    // 信号槽
    connect(manage_pp_btn_, &QPushButton::clicked,
            this, &SettingsView::on_manage_password_clicked);
    connect(open_storage_btn_, &QPushButton::clicked,
            this, &SettingsView::on_open_storage_clicked);
    connect(license_btn_, &QPushButton::clicked,
            this, &SettingsView::on_view_license_clicked);
    connect(github_btn_, &QPushButton::clicked,
            this, &SettingsView::on_open_github_clicked);
    connect(lock_now_btn_, &QPushButton::clicked,
            this, &SettingsView::on_lock_now_clicked);
    connect(view_history_btn_, &QPushButton::clicked,
            this, &SettingsView::on_view_generator_history_clicked);
    connect(gen_limit_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsView::on_generator_limit_changed);
    connect(autolock_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsView::on_autolock_changed);
}

QFrame* SettingsView::make_section(const QString& icon_resource,
                                   const QString& title, bool danger,
                                   QWidget* parent,
                                   const QColor& icon_color) {
    auto* section = new QFrame(parent);
    section->setProperty("cssClass", danger
        ? QStringLiteral("cardDanger")
        : QStringLiteral("card"));

    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(0);

    // 标题行（图标 + 标题）
    auto* header_layout = new QHBoxLayout();
    header_layout->setContentsMargins(0, 0, 0, 12);
    header_layout->setSpacing(10);
    auto* icon_lbl = new QLabel(section);
    // 图标颜色优先级：调用方显式传入 > danger 红 > 品牌蓝
    // 注意：参数名 icon_color 遮蔽了同名的全局函数，调用需全限定
    QColor clr = icon_color.isValid() ? icon_color
        : (danger ? yuli::vault::ui::icon_color(IconRole::Danger)
                  : yuli::vault::ui::icon_color(IconRole::Info));
    icon_lbl->setPixmap(tinted_pixmap(icon_resource, clr, QSize(18, 18)));
    icon_lbl->setProperty("cssClass", QStringLiteral("inlineIcon"));
    header_layout->addWidget(icon_lbl);
    auto* title_lbl = new QLabel(title, section);
    title_lbl->setProperty("cssClass", QStringLiteral("sectionTitle"));
    header_layout->addWidget(title_lbl);
    header_layout->addStretch(1);
    layout->addLayout(header_layout);

    return section;
}

void SettingsView::add_row(QVBoxLayout* section_layout, const QString& title,
                            const QString& description, QWidget* right_widget,
                            QLabel** out_desc) {
    auto* parent_widget = section_layout->parentWidget();
    auto* row = new QFrame(parent_widget);
    row->setProperty("cssClass", QStringLiteral("fieldRow"));
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 12, 0, 12);
    row_layout->setSpacing(12);

    // 左侧：标题 + 描述
    auto* left = new QWidget(row);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(2);
    auto* title_lbl = new QLabel(title, left);
    title_lbl->setProperty("cssClass", QStringLiteral("fieldLabel"));
    left_layout->addWidget(title_lbl);
    QLabel* desc_lbl = nullptr;
    if (!description.isEmpty()) {
        desc_lbl = new QLabel(description, left);
        desc_lbl->setWordWrap(true);
        desc_lbl->setProperty("cssClass", QStringLiteral("caption"));
        left_layout->addWidget(desc_lbl);
    }
    row_layout->addWidget(left, 1);
    if (right_widget) {
        right_widget->setParent(row);
        row_layout->addWidget(right_widget, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    section_layout->addWidget(row);
    if (out_desc) *out_desc = desc_lbl;
}

QFrame* SettingsView::build_theme_segmented() {
    auto* frame = new QFrame();
    frame->setObjectName(QStringLiteral("settingsThemeSeg"));
    frame->setProperty("cssClass", QStringLiteral("segmented"));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    theme_group_ = new QButtonGroup(this);
    theme_group_->setExclusive(true);

    auto make_btn = [](const QString& text, QWidget* parent) {
        auto* btn = new QPushButton(text, parent);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setCheckable(true);
        btn->setFixedHeight(32);
        btn->setMinimumWidth(64);
        btn->setProperty("cssClass", QStringLiteral("segmentedItem"));
        return btn;
    };

    theme_light_btn_ = make_btn(tr("Light"), frame);
    theme_dark_btn_ = make_btn(tr("Dark"), frame);
    theme_system_btn_ = make_btn(tr("System"), frame);

    theme_group_->addButton(theme_light_btn_, 0);
    theme_group_->addButton(theme_dark_btn_, 1);
    theme_group_->addButton(theme_system_btn_, 2);

    layout->addWidget(theme_light_btn_);
    layout->addWidget(theme_dark_btn_);
    layout->addWidget(theme_system_btn_);

    connect(theme_group_, &QButtonGroup::idClicked,
            this, &SettingsView::on_theme_segment_clicked);

    return frame;
}

void SettingsView::sync_theme_segment() {
    if (!theme_group_) return;
    const auto mode = Theme::current_mode();
    const int id = (mode == Theme::Mode::Light) ? 0
                  : (mode == Theme::Mode::Dark) ? 1 : 2;
    QSignalBlocker b(theme_group_);
    auto* btn = theme_group_->button(id);
    if (btn) btn->setChecked(true);
}

void SettingsView::refresh_password_badge() {
    if (!pp_badge_) return;
    if (password_enabled_) {
        pp_badge_->setText(tr("On"));
        pp_badge_->setProperty("cssClass", QStringLiteral("badgeSuccess"));
        if (pp_desc_) pp_desc_->setText(tr("On · vault is encrypted"));
    } else {
        pp_badge_->setText(tr("Off"));
        pp_badge_->setProperty("cssClass", QStringLiteral("badge"));
        if (pp_desc_) pp_desc_->setText(tr("Off · vault is stored in plaintext"));
    }
    // 明文模式（未EnableProgram password）下隐藏「Lock now」按钮：
    // 无加密即无可锁，按钮可见会让用户误Actions后报错。
    if (lock_now_btn_) lock_now_btn_->setVisible(password_enabled_);
    // Refresh dynamic property 样式
    pp_badge_->style()->unpolish(pp_badge_);
    pp_badge_->style()->polish(pp_badge_);
}

void SettingsView::refresh_entry_count() {
    if (!entry_count_label_) return;
    if (!client_) {
        entry_count_label_->setText(QStringLiteral("-"));
        return;
    }
    auto result = client_->list_entries();
    if (result.ok()) {
        entry_count_label_->setText(
            tr("%1 items").arg(result.value().entries.size()));
    } else {
        entry_count_label_->setText(QStringLiteral("-"));
    }
}

void SettingsView::refresh_generator_settings() {
    if (!client_) {
        if (gen_history_desc_) gen_history_desc_->setText(QStringLiteral("-"));
        return;
    }

    // 历史记录条数（异步）
    loading_history_ = true;
    auto* list_watcher = new QFutureWatcher<core::Result<protocol::ListGeneratedRecordsResponse>>(this);
    connect(list_watcher, &QFutureWatcher<core::Result<protocol::ListGeneratedRecordsResponse>>::finished,
            this, [this, list_watcher]() {
        loading_history_ = false;
        auto result = list_watcher->result();
        if (gen_history_desc_) {
            if (result.ok()) {
                const int n = static_cast<int>(result.value().records.size());
                gen_history_desc_->setText(
                    n == 0 ? tr("No records")
                           : tr("%1 records saved").arg(n));
            } else {
                gen_history_desc_->setText(QStringLiteral("-"));
            }
        }
        list_watcher->deleteLater();
    });
    list_watcher->setFuture(client_->list_generated_records_async());

    // 上限下拉：以 service 端持久化的值为准（异步）
    loading_settings_ = true;
    auto* settings_watcher = new QFutureWatcher<core::Result<protocol::GetGeneratorSettingsResponse>>(this);
    connect(settings_watcher, &QFutureWatcher<core::Result<protocol::GetGeneratorSettingsResponse>>::finished,
            this, [this, settings_watcher]() {
        loading_settings_ = false;
        auto result = settings_watcher->result();
        if (!result.ok()) {
            settings_watcher->deleteLater();
            return;
        }
        const int32_t limit = result.value().history_limit;
        // 找到与 limit 匹配的项；无匹配时（数值不在候选Medium）回退到「Unlimited」
        int target_index = 0;
        if (gen_limit_combo_) {
            gen_limit_syncing_ = true;
            for (int i = 0; i < gen_limit_combo_->count(); ++i) {
                const int v = gen_limit_combo_->itemData(i).toInt();
                if (v == limit) {
                    target_index = i;
                    break;
                }
            }
            QSignalBlocker blocker(gen_limit_combo_);
            gen_limit_combo_->setCurrentIndex(target_index);
            gen_limit_syncing_ = false;
        }
        settings_watcher->deleteLater();
    });
    settings_watcher->setFuture(client_->get_generator_settings_async());
}

// ---------------------------------------------------------------------------
// 状态Refresh
// ---------------------------------------------------------------------------

void SettingsView::refresh_status() {
    if (!client_) {
        password_enabled_ = false;
        refresh_password_badge();
        return;
    }

    auto result = client_->get_vault_status();
    if (!result.ok()) {
        password_enabled_ = false;
        refresh_password_badge();
        if (entry_count_label_) entry_count_label_->setText(QStringLiteral("-"));
        return;
    }

    password_enabled_ = result.value().password_enabled;
    refresh_password_badge();
    refresh_entry_count();
    refresh_generator_settings();
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void SettingsView::on_manage_password_clicked() {
    if (!client_) return;
    // 根据当前Password状态选择默认 Tab：已Enable → Change；未Enable → Enable
    const auto initial_mode = password_enabled_
        ? ProgramPasswordDialog::Mode::Change
        : ProgramPasswordDialog::Mode::Enable;
    auto* dlg = new ProgramPasswordDialog(client_, initial_mode, this);
    connect(dlg, &ProgramPasswordDialog::succeeded, this, [this, dlg]() {
        dlg->deleteLater();
        refresh_status();
        emit password_state_changed(password_enabled_);
    });
    connect(dlg, &ProgramPasswordDialog::rejected, dlg, &QWidget::deleteLater);
    dlg->show();
}

void SettingsView::on_open_storage_clicked() {
    const QString path = data_storage_path();
    const QUrl url = QUrl::fromLocalFile(path);
    if (!QDesktopServices::openUrl(url)) {
        QMessageBox::warning(this, tr("Open failed"),
            tr("Could not open the data folder: %1").arg(path));
    }
}

void SettingsView::on_view_license_clicked() {
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kLicenseUrl)));
}

void SettingsView::on_open_github_clicked() {
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kGitHubUrl)));
}

void SettingsView::on_lock_now_clicked() {
    if (!client_) return;
    auto result = client_->lock();
    if (result.ok()) {
        emit lock_requested();
    } else {
        const QString msg = QString::fromStdString(result.error().what());
        QMessageBox::warning(this, tr("Lock failed"),
            msg.isEmpty() ? tr("Could not lock the vault.")
                          : tr("Lock failed: %1").arg(msg));
    }
}

void SettingsView::on_theme_segment_clicked(int idx) {
    switch (idx) {
        case 0: Theme::set_mode(Theme::Mode::Light); break;
        case 1: Theme::set_mode(Theme::Mode::Dark); break;
        case 2: Theme::set_mode(Theme::Mode::System); break;
        default: break;
    }
}

void SettingsView::on_view_generator_history_clicked() {
    if (!client_) return;
    auto* dlg = new GeneratorHistoryDialog(client_, this);
    // 空状态「Generate a password」→ Medium转给 MainWindow 切换到 GeneratorView
    connect(dlg, &GeneratorHistoryDialog::generate_requested,
            this, &SettingsView::generate_requested);
    // Close后自动清理 + Refresh本页「已保存 N 条记录」描述
    connect(dlg, &QDialog::finished, this, [this]() {
        if (!client_) return;
        // 重新查 service：可能用户在弹窗里Delete/清空了记录（异步）
        auto* watcher = new QFutureWatcher<core::Result<protocol::ListGeneratedRecordsResponse>>(this);
        connect(watcher, &QFutureWatcher<core::Result<protocol::ListGeneratedRecordsResponse>>::finished,
                this, [this, watcher]() {
            auto list_result = watcher->result();
            if (gen_history_desc_ && list_result.ok()) {
                const int n = static_cast<int>(list_result.value().records.size());
                gen_history_desc_->setText(
                    n == 0 ? tr("No records")
                           : tr("%1 records saved").arg(n));
            }
            watcher->deleteLater();
        });
        watcher->setFuture(client_->list_generated_records_async());
    });
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void SettingsView::on_generator_limit_changed(int index) {
    if (gen_limit_syncing_ || loading_settings_) return;
    if (!client_ || !gen_limit_combo_) return;
    if (index < 0) return;
    const int limit = gen_limit_combo_->itemData(index).toInt();

    auto* watcher = new QFutureWatcher<core::Result<protocol::SetGeneratorLimitResponse>>(this);
    connect(watcher, &QFutureWatcher<core::Result<protocol::SetGeneratorLimitResponse>>::finished,
            this, [this, watcher, limit]() {
        auto result = watcher->result();
        if (!result.ok()) {
            Toast::show(this, friendly_message(result.error()));
            // 失败时回退下拉到 service 端实际值
            refresh_generator_settings();
        } else if (gen_history_desc_ && limit > 0) {
            // Success时Refresh历史记录条数描述（清理可能减少记录数，异步）
            auto* list_watcher = new QFutureWatcher<core::Result<protocol::ListGeneratedRecordsResponse>>(this);
            connect(list_watcher, &QFutureWatcher<core::Result<protocol::ListGeneratedRecordsResponse>>::finished,
                    this, [this, list_watcher]() {
                auto list_result = list_watcher->result();
                if (gen_history_desc_ && list_result.ok()) {
                    const int n = static_cast<int>(list_result.value().records.size());
                    gen_history_desc_->setText(
                        n == 0 ? tr("No records")
                               : tr("%1 records saved").arg(n));
                }
                list_watcher->deleteLater();
            });
            list_watcher->setFuture(client_->list_generated_records_async());
        }
        watcher->deleteLater();
    });
    watcher->setFuture(client_->set_generator_limit_async(static_cast<int32_t>(limit)));
}

void SettingsView::on_language_changed(int index) {
    if (!language_combo_ || index < 0) return;
    const auto language = static_cast<UiLanguage>(language_combo_->itemData(index).toInt());
    save_ui_language(language);
    QMessageBox::information(
        this, tr("Language"),
        tr("Restart Yuli Vault to apply the language."));
}

int SettingsView::get_autolock_minutes() const {
    if (!autolock_combo_) return 0;
    const int idx = autolock_combo_->currentIndex();
    if (idx < 0) return 0;
    return autolock_combo_->itemData(idx).toInt();
}

void SettingsView::on_autolock_changed(int index) {
    if (!autolock_combo_ || index < 0) return;
    const int minutes = autolock_combo_->itemData(index).toInt();
    // 持久化用户选择：MainWindow::setup_autolock 也会写一份，双写无害
    // （键值相同，确保即使 MainWindow 未连接也能保存）。
    settings_set(QStringLiteral("autolock_minutes"), minutes);
    emit autolock_changed(minutes);
}

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// SettingsView.h
//
// Yuli Vault Settings视图（新设计）。
//
// 卡片分节布局（max 720px 居Medium）：
//   - Security：Program password状态 + 「Program password」按钮 + Auto-lock下拉（占位）
//   - Appearance：Theme分段控件（Light / Dark / System）
//   - Storage：Storage path（mono） + Open按钮 + Item count
//   - About：Version / Encryption / License / Project home
//   - Danger zone：Lock vault按钮（红色边框）
//
// Program password管理通过 ProgramPasswordDialog（单 Tab 弹窗）完成。
// 锁定按钮调用 client->lock()，emit lock_requested 由 MainWindow 切回Unlock视图。
// =============================================================================
#pragma once

#include <QColor>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace yuli::vault::ui {

class IpcClient;

class SettingsView : public QWidget {
    Q_OBJECT
public:
    explicit SettingsView(IpcClient* client, QWidget* parent = nullptr);
    ~SettingsView() override;

signals:
    /// 用户点击「锁定」且 lock() 调用Success后触发，
    /// MainWindow 收到后显示 UnlockView。
    void lock_requested();

    /// Program password状态发生变化（Enable/Disable）后触发，
    /// MainWindow 可据此调整锁定按钮可见性等。
    void password_state_changed(bool enabled);

    /// Auto-lock下拉选择变化后触发，
    /// \param minutes 0 = Never；1/5/15/30 = 对应分钟数。
    /// MainWindow 收到后启动 / 重置 / 停止 autolock_timer_。
    void autolock_changed(int minutes);

    /// 用户在Generate历史对话框空状态点击「Generate a password」时触发，
    /// MainWindow 收到后切换到 GeneratorView。
    void generate_requested();

public slots:
    /// 查询 service 当前 vault 状态并Refresh UI 显示。
    /// 应在视图显示前调用（如主窗口启动后、UnlockSuccess后）。
    void refresh_status();

    /// 返回当前Auto-lock下拉选Medium的分钟数（0 = Never；1/5/15/30）。
    /// MainWindow 启动时调用以读取持久化值并启动 timer。
    int get_autolock_minutes() const;

private slots:
    void on_manage_password_clicked();
    void on_open_storage_clicked();
    void on_view_license_clicked();
    void on_open_github_clicked();
    void on_lock_now_clicked();
    void on_theme_segment_clicked(int idx);
    void on_view_generator_history_clicked();
    void on_generator_limit_changed(int index);
    /// Auto-lock下拉 currentIndexChanged 槽：
    /// 解析 itemData 取 minutes，emit autolock_changed 并持久化到 QSettings。
    void on_autolock_changed(int index);
    void on_language_changed(int index);

private:
    void build_ui();
    /// 构建「卡片分节」式 section 容器。
    /// \param icon_resource qrc Medium section 标题图标的路径（如 ":/icons/shield-check.svg"）
    /// \param title         section 标题
    /// \param danger        是否使用危险边框样式（红色边框）
    /// \param parent        父 widget（section 将作为子控件）
    /// \return 返回 section（已Settings layout，调用者用 layout() 来 add_row）
    QFrame* make_section(const QString& icon_resource, const QString& title,
                         bool danger, QWidget* parent,
                         const QColor& icon_color = QColor());

    /// 在 section 内追加一行（左：标题 + 描述；右：自定义 widget）。
    /// \param out_desc  非空时，回传创建的描述 QLabel 指针（供后续动态更新文本）
    void add_row(QVBoxLayout* section_layout, const QString& title,
                 const QString& description, QWidget* right_widget,
                 QLabel** out_desc = nullptr);

    /// Theme分段控件：Light / Dark / System
    QFrame* build_theme_segmented();

    /// RefreshTheme分段控件选Medium状态（不触发信号）
    void sync_theme_segment();

    /// RefreshProgram password状态显示（badge 文本与样式）
    void refresh_password_badge();

    /// RefreshItem count显示
    void refresh_entry_count();

    /// 拉取GeneratorSettings，Refresh「记录数」描述与上限下拉选Medium项
    void refresh_generator_settings();

    IpcClient* client_;

    // Security区
    QLabel* pp_badge_ = nullptr;          // 「已Enable / 未Enable」徽章
    QLabel* pp_desc_ = nullptr;           // 副标题
    QPushButton* manage_pp_btn_ = nullptr;
    QComboBox* autolock_combo_ = nullptr;

    // Generator区
    QLabel* gen_history_desc_ = nullptr;     // 历史记录行描述（"已保存 N 条记录"）
    QPushButton* view_history_btn_ = nullptr;
    QComboBox* gen_limit_combo_ = nullptr;
    bool gen_limit_syncing_ = false;          // 防止 sync 时触发 on_generator_limit_changed
    bool loading_history_ = false;            // 历史记录异步加载Medium（list_generated_records）
    bool loading_settings_ = false;           // GeneratorSettings异步加载Medium（get_generator_settings）

    // Appearance区
    QButtonGroup* theme_group_ = nullptr;
    QPushButton* theme_light_btn_ = nullptr;
    QPushButton* theme_dark_btn_ = nullptr;
    QPushButton* theme_system_btn_ = nullptr;
    QCheckBox* hc_checkbox_ = nullptr;
    QComboBox* language_combo_ = nullptr;

    // Storage区
    QLabel* storage_path_label_ = nullptr;
    QPushButton* open_storage_btn_ = nullptr;
    QLabel* entry_count_label_ = nullptr;

    // About区
    QLabel* version_value_ = nullptr;
    QPushButton* license_btn_ = nullptr;
    QPushButton* github_btn_ = nullptr;

    // Danger zone区
    QPushButton* lock_now_btn_ = nullptr;

    /// 缓存当前Program password是否已Enable。
    bool password_enabled_ = false;
};

}  // namespace yuli::vault::ui

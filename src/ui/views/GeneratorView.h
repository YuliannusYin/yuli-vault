// coding: utf-8
// =============================================================================
// GeneratorView.h
//
// Yuli Vault PasswordGenerator视图。配置Character set与Length → 调用 generate_password → 显示结果
// 并实时评估Strong度。支持Copy到剪贴板。
// =============================================================================
#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace yuli::vault::ui {

class IpcClient;

class GeneratorView : public QWidget {
    Q_OBJECT
public:
    explicit GeneratorView(IpcClient* client, QWidget* parent = nullptr);
    ~GeneratorView() override;

signals:
    /// Generate password后触发，MainWindow 可将其传给 InputView 回填。
    void password_generated(const QString& password);

private slots:
    void on_generate_clicked();
    void on_copy_clicked();

private:
    void build_ui();
    /// 异步评估PasswordStrong度并更新 strength_bar_ / strength_label_。
    /// 空Password同步重置 UI；非空则通过 estimate_strength_async 走线程池。
    void estimate_strength_async(const QString& password);
    /// 统一更新 strength_label_ 文本与 cssClass（触发 QSS 重新评估）。
    void set_strength_label(const QString& text, const QString& css_class);

    IpcClient* client_;

    /// 防止Generate按钮重复点击（异步调用期间Disable）。
    bool generating_ = false;

    QSpinBox* length_spin_ = nullptr;
    QCheckBox* upper_check_ = nullptr;
    QCheckBox* lower_check_ = nullptr;
    QCheckBox* digits_check_ = nullptr;
    QCheckBox* symbols_check_ = nullptr;
    QCheckBox* exclude_ambiguous_check_ = nullptr;
    QLineEdit* custom_chars_edit_ = nullptr;
    QPushButton* generate_button_ = nullptr;
    QLineEdit* result_edit_ = nullptr;
    QProgressBar* strength_bar_ = nullptr;
    QLabel* strength_label_ = nullptr;
    QPushButton* copy_button_ = nullptr;
};

}  // namespace yuli::vault::ui

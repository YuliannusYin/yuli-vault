// coding: utf-8
// =============================================================================
// AppSettings.h
//
// QSettings identity for Yuli Vault, with a one-time copy from PwdVault keys,
// plus the UI language preference (System / English / Simplified Chinese).
// =============================================================================
#pragma once

#include <QSettings>
#include <QString>
#include <QVariant>

class QApplication;
class QTranslator;

namespace yuli::vault::ui {

inline constexpr auto kSettingsOrg = "YuliVault";
inline constexpr auto kSettingsApp = "YuliVault";
inline constexpr auto kLanguageSettingsKey = "ui/language";

enum class UiLanguage {
    System = 0,
    English = 1,
    ChineseSimplified = 2,
};

QSettings app_settings();

/// Read `key` from YuliVault settings, copying from legacy PwdVault keys if needed.
QVariant settings_value(const QString& key, const QVariant& default_value = {});

void settings_set(const QString& key, const QVariant& value);

UiLanguage load_ui_language();
void save_ui_language(UiLanguage language);

/// True when Simplified Chinese translations should be installed.
bool wants_chinese_ui(UiLanguage preference);

/// Load or unload :/translations/yuli-vault_zh_CN.qm. Returns true if Chinese is active.
bool apply_ui_translator(QApplication* app, QTranslator* translator);

}  // namespace yuli::vault::ui

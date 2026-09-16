// coding: utf-8
// =============================================================================
// AppSettings.cpp
// =============================================================================
#include "AppSettings.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

namespace yuli::vault::ui {

namespace {

struct SettingsIdentity {
    const char* org;
    const char* app;
};

constexpr SettingsIdentity kLegacyIdentities[] = {
    {kSettingsOrg, kSettingsApp},
    {"PwdVault", "PwdVault"},
    {"PwdVault", "Settings"},
    {"Yuli Vault", "Yuli Vault"},
    {"Yuli Vault", "Settings"},
};

QVariant read_first(const QString& key) {
    for (const auto& id : kLegacyIdentities) {
        QSettings s(QString::fromLatin1(id.org), QString::fromLatin1(id.app));
        if (s.contains(key)) {
            return s.value(key);
        }
    }
    return {};
}

}  // namespace

QSettings app_settings() {
    return QSettings(QString::fromLatin1(kSettingsOrg),
                     QString::fromLatin1(kSettingsApp));
}

QVariant settings_value(const QString& key, const QVariant& default_value) {
    QSettings neu = app_settings();
    if (neu.contains(key)) {
        return neu.value(key, default_value);
    }
    const QVariant legacy = read_first(key);
    if (legacy.isValid()) {
        neu.setValue(key, legacy);
        return legacy;
    }
    return default_value;
}

void settings_set(const QString& key, const QVariant& value) {
    QSettings neu = app_settings();
    neu.setValue(key, value);
}

UiLanguage load_ui_language() {
    const int raw = settings_value(QString::fromLatin1(kLanguageSettingsKey),
                                   static_cast<int>(UiLanguage::System))
                        .toInt();
    switch (raw) {
        case static_cast<int>(UiLanguage::English):
            return UiLanguage::English;
        case static_cast<int>(UiLanguage::ChineseSimplified):
            return UiLanguage::ChineseSimplified;
        case static_cast<int>(UiLanguage::System):
        default:
            return UiLanguage::System;
    }
}

void save_ui_language(UiLanguage language) {
    settings_set(QString::fromLatin1(kLanguageSettingsKey),
                 static_cast<int>(language));
}

bool wants_chinese_ui(UiLanguage preference) {
    switch (preference) {
        case UiLanguage::English:
            return false;
        case UiLanguage::ChineseSimplified:
            return true;
        case UiLanguage::System:
        default:
            return QLocale::system().name().startsWith(QStringLiteral("zh"));
    }
}

bool apply_ui_translator(QApplication* app, QTranslator* translator) {
    if (app == nullptr || translator == nullptr) {
        return false;
    }
    app->removeTranslator(translator);
    if (!wants_chinese_ui(load_ui_language())) {
        return false;
    }
    if (translator->load(QStringLiteral(":/translations/yuli-vault_zh_CN.qm"))) {
        app->installTranslator(translator);
        return true;
    }
    return false;
}

}  // namespace yuli::vault::ui

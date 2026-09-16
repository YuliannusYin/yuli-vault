// coding: utf-8
// =============================================================================
// StrengthUtil.cpp
//
// PasswordStrong度等级 → UI 展示属性实现。所有阈值/文案/颜色在此一处定义，
// 5 处 view（ProgramPasswordDialog / PasswordBookView / GeneratorView /
// InputView / EditEntryDialog）共享。
// =============================================================================
#include "StrengthUtil.h"

#include <QCoreApplication>

namespace yuli::vault::ui {

QString strength_text(core::StrengthLevel level) {
    switch (level) {
        case core::StrengthLevel::VeryWeak:   return QCoreApplication::translate("StrengthUtil", "Very weak");
        case core::StrengthLevel::Weak:       return QCoreApplication::translate("StrengthUtil", "Weak");
        case core::StrengthLevel::Medium:     return QCoreApplication::translate("StrengthUtil", "Medium");
        case core::StrengthLevel::Strong:     return QCoreApplication::translate("StrengthUtil", "Strong");
        case core::StrengthLevel::VeryStrong:  return QCoreApplication::translate("StrengthUtil", "Very strong");
    }
    return QCoreApplication::translate("StrengthUtil", "Weak");
}

QString strength_qss_key(core::StrengthLevel level) {
    switch (level) {
        case core::StrengthLevel::VeryWeak:   return QStringLiteral("veryweak");
        case core::StrengthLevel::Weak:       return QStringLiteral("weak");
        case core::StrengthLevel::Medium:     return QStringLiteral("medium");
        case core::StrengthLevel::Strong:     return QStringLiteral("strong");
        case core::StrengthLevel::VeryStrong:  return QStringLiteral("verystrong");
    }
    return QStringLiteral("weak");
}

QString strength_color(core::StrengthLevel level) {
    // 与 badge 颜色谱对齐：红 / 黄 / 蓝 / 浅绿 / 深绿（light Theme色值）
    switch (level) {
        case core::StrengthLevel::VeryWeak:   return QStringLiteral("#dc2626");  // 红
        case core::StrengthLevel::Weak:       return QStringLiteral("#c9820a");  // 黄
        case core::StrengthLevel::Medium:     return QStringLiteral("#2f5fff");  // 蓝
        case core::StrengthLevel::Strong:     return QStringLiteral("#1f9d57");  // 浅绿
        case core::StrengthLevel::VeryStrong:  return QStringLiteral("#0f7a3d");  // 深绿
    }
    return QStringLiteral("#dc2626");
}

QString strength_label_class(core::StrengthLevel level) {
    // 5 级独立 cssClass，颜色与 badge 谱系对齐：
    //   error(红) / warning(黄) / info(蓝) / success(浅绿) / veryStrong(深绿)
    switch (level) {
        case core::StrengthLevel::VeryWeak:   return QStringLiteral("error");
        case core::StrengthLevel::Weak:       return QStringLiteral("warning");
        case core::StrengthLevel::Medium:     return QStringLiteral("info");
        case core::StrengthLevel::Strong:     return QStringLiteral("success");
        case core::StrengthLevel::VeryStrong:  return QStringLiteral("veryStrong");
    }
    return QStringLiteral("error");
}

QString strength_badge_class(core::StrengthLevel level) {
    switch (level) {
        case core::StrengthLevel::VeryWeak:   return QStringLiteral("badgeDanger");
        case core::StrengthLevel::Weak:       return QStringLiteral("badgeWarning");
        case core::StrengthLevel::Medium:     return QStringLiteral("badgeInfo");
        case core::StrengthLevel::Strong:     return QStringLiteral("badgeSuccess");
        case core::StrengthLevel::VeryStrong:  return QStringLiteral("badgeVeryStrong");
    }
    return QStringLiteral("badgeDanger");
}

int strength_segments(core::StrengthLevel level) {
    return static_cast<int>(level);
}

QString translate_strength_warning(const QString& key) {
    if (key.startsWith(QStringLiteral("repeat:"))) {
        return QCoreApplication::translate(
            "StrengthUtil", "Repeated character run of length %1")
            .arg(key.section(QLatin1Char(':'), 1));
    }
    if (key == QStringLiteral("uneven")) {
        return QCoreApplication::translate(
            "StrengthUtil", "Character classes are unevenly mixed");
    }
    if (key.startsWith(QStringLiteral("sequential:"))) {
        return QCoreApplication::translate(
            "StrengthUtil", "Sequential characters of length %1")
            .arg(key.section(QLatin1Char(':'), 1));
    }
    if (key.startsWith(QStringLiteral("keyboard:"))) {
        return QCoreApplication::translate(
            "StrengthUtil", "Keyboard walk of length %1")
            .arg(key.section(QLatin1Char(':'), 1));
    }
    return key;
}

}  // namespace yuli::vault::ui

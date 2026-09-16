// coding: utf-8
// =============================================================================
// StrengthUtil.h
//
// PasswordStrong度等级 → UI 展示属性的统一映射。集Medium存放原本散落在 5 处 view Medium的
// 阈值判断、文案、颜色、cssClass、进度条段数，避免阈值不一致。
//
// 所有函数均按 core::StrengthLevel 输入（不再以 bits 为输入），调用方负责
// 从 StrengthEstimate Medium取 level 字段传入。
// =============================================================================
#pragma once

#include <QString>

#include "Types.h"

namespace yuli::vault::ui {

/// Strong度等级 → Medium文文案。
///   VeryWeak → Very weak、Weak → Weak、Medium → Medium、Strong → Strong、VeryStrong → Very strong
QString strength_text(core::StrengthLevel level);

/// Strong度等级 → Strong度条 QSS key（与 QSS Medium [strength="..."] 选择器对应）。
///   veryweak / weak / medium / strong / verystrong
QString strength_qss_key(core::StrengthLevel level);

/// Strong度等级 → Strong度条颜色（hex 字符串，用于 inline 样式或图标着色）。
QString strength_color(core::StrengthLevel level);

/// Strong度等级 → 文本Tags cssClass（与 QSS Medium QLabel[cssClass="..."] 对应）。
///   error / warning / caption / success / success
QString strength_label_class(core::StrengthLevel level);

/// Strong度等级 → badge cssClass（与 QSS Medium QLabel[cssClass="badge*"] 对应）。
///   badgeDanger / badgeWarning / badgeInfo / badgeSuccess / badgeVeryStrong
QString strength_badge_class(core::StrengthLevel level);

/// Strong度等级 → 进度条点亮段数（0..4）。
int strength_segments(core::StrengthLevel level);

/// Map a stable SDK warning key (repeat:N, uneven, sequential:N, keyboard:N)
/// to a translated sentence for the UI.
QString translate_strength_warning(const QString& key);

}  // namespace yuli::vault::ui

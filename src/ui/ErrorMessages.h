// coding: utf-8
// =============================================================================
// ErrorMessages.h
//
// 把 core::Error 转换为面向用户的Medium文友好文案。
// 技术细节（error.what()）由本函数记入 qDebug 日志，不展示给用户。
//
// 设计要点：
//   - 调用方传入失败状态的 core::Error，得到适合直接显示在 QLabel /
//     QMessageBox Medium的Medium文文案。
//   - 技术细节（"code: message" 字符串）通过 qDebug 输出，便于排障但
//     不污染用户界面。
//   - 仅做错误码 → 文案映射，不做参数替换；如需带上下文（如Title），
//     由调用方自行拼接。
// =============================================================================
#pragma once

#include <QString>

#include "Error.h"

namespace yuli::vault::ui {

/// 把 core::Error 转为用户可读的Medium文文案。
/// \param error 引擎或 IPC 返回的错误对象（必须为失败状态）
/// \return 友好文案（不含技术细节）
QString friendly_message(const core::Error& error);

}  // namespace yuli::vault::ui

// coding: utf-8
// =============================================================================
// ErrorMessages.cpp
//
// core::Error → Medium文友好文案 实现。技术细节进 qDebug 日志。
// =============================================================================
#include "ErrorMessages.h"

#include <QDebug>
#include <QCoreApplication>

namespace yuli::vault::ui {

QString friendly_message(const core::Error& error) {
    // 技术细节进日志，便于排障但不污染 UI
    const std::string what = error.what();
    if (!what.empty()) {
        qDebug() << "[Yuli Vault][Error]" << QString::fromStdString(what);
    }

    switch (error.code) {
        case core::ErrorCode::None:
            return QCoreApplication::translate("ErrorMessages", "Success");
        case core::ErrorCode::InvalidArgument:
            return QCoreApplication::translate("ErrorMessages", "Invalid input. Check the fields.");
        case core::ErrorCode::NotFound:
            return QCoreApplication::translate("ErrorMessages", "Item not found. It may have been deleted.");
        case core::ErrorCode::AlreadyExists:
            return QCoreApplication::translate("ErrorMessages", "That name is already in use.");
        case core::ErrorCode::Unauthorized:
            return QCoreApplication::translate("ErrorMessages", "Wrong password or not authorized");
        case core::ErrorCode::CryptoError:
            return QCoreApplication::translate("ErrorMessages", "Encryption failed. Data may be damaged.");
        case core::ErrorCode::StorageError:
            return QCoreApplication::translate("ErrorMessages", "Local storage failed. Try again.");
        case core::ErrorCode::IpcError:
            return QCoreApplication::translate("ErrorMessages", "Could not talk to the service. Try again.");
        case core::ErrorCode::InternalError:
            return QCoreApplication::translate("ErrorMessages", "Internal error. Try again.");
    }
    return QCoreApplication::translate("ErrorMessages", "Unknown error");
}

}  // namespace yuli::vault::ui

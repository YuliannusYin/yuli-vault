// coding: utf-8
// =============================================================================
// VaultItemUi.h
//
// Qt labels for VaultItemType. Login is the only editable type in this release.
// =============================================================================
#pragma once

#include <QCoreApplication>
#include <QString>

#include "Types.h"

namespace yuli::vault::ui {

inline QString vault_item_type_label(core::VaultItemType type) {
    switch (type) {
        case core::VaultItemType::Login:
            return QCoreApplication::translate("VaultItemType", "Login");
        case core::VaultItemType::SecureNote:
            return QCoreApplication::translate("VaultItemType", "Secure note");
        case core::VaultItemType::Card:
            return QCoreApplication::translate("VaultItemType", "Card");
        case core::VaultItemType::Identity:
            return QCoreApplication::translate("VaultItemType", "Identity");
        case core::VaultItemType::Custom:
            return QCoreApplication::translate("VaultItemType", "Custom");
    }
    return QCoreApplication::translate("VaultItemType", "Unknown");
}

inline bool vault_item_type_has_login_editor(core::VaultItemType type) {
    return type == core::VaultItemType::Login;
}

}  // namespace yuli::vault::ui

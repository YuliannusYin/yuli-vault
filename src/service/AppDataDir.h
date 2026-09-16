// coding: utf-8
// =============================================================================
// AppDataDir.h
//
// Resolve %APPDATA%\YuliVault. If that folder is missing and a legacy
// %APPDATA%\PwdVault folder exists, copy it across and keep the original.
// =============================================================================
#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace yuli::vault::service {

/// Resolve the vault data directory under `appdata_root`.
/// When `appdata_root` is empty, uses relative "YuliVault" / "PwdVault".
inline std::filesystem::path resolve_vault_data_dir(
    const std::filesystem::path& appdata_root,
    std::string* migrated_message = nullptr) {
    const std::filesystem::path neu =
        appdata_root.empty() ? std::filesystem::path(L"YuliVault")
                             : appdata_root / L"YuliVault";
    const std::filesystem::path legacy =
        appdata_root.empty() ? std::filesystem::path(L"PwdVault")
                             : appdata_root / L"PwdVault";

    std::error_code ec;
    if (std::filesystem::exists(neu, ec)) {
        return neu;
    }
    if (std::filesystem::exists(legacy, ec)) {
        std::filesystem::create_directories(neu, ec);
        std::filesystem::copy(
            legacy, neu,
            std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing,
            ec);
        std::ofstream marker(neu / "migrated_from_pwdvault");
        marker << "Copied from %APPDATA%\\PwdVault. The original folder was kept.\n";
        if (migrated_message != nullptr) {
            *migrated_message = "Migrated data directory from PwdVault to YuliVault";
        }
    }
    return neu;
}

}  // namespace yuli::vault::service

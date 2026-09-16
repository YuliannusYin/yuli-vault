# Yuli Vault build guide

Windows-only. Product overview: [README.md](../README.md). Architecture: [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Requirements

| Tool | Version | Notes |
| --- | --- | --- |
| Visual Studio | 2022 (MSVC v143) | Desktop development with C++, C++20 |
| Qt | 6.5+ LTS | Widgets, Network, Concurrent. LinguistTools comes from vcpkg `qttools[linguist]` (needed to embed `yuli-vault_zh_CN.qm`) |
| vcpkg | latest | Manifest mode |
| CMake | 3.20+ | |
| Git | 2.x | |
| Inno Setup | 6.x (optional) | Installer |

## 2. Configure

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
$env:CMAKE_PREFIX_PATH = "C:\Qt\6.5.3\msvc2022_64"

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Outputs: `build/bin/Release/yuli-vault-ui.exe` and `yuli-vault-service.exe`.

The UI looks for `yuli-vault-service.exe` in the same folder.

## 3. Tests

```powershell
ctest --test-dir build --output-on-failure
ctest --test-dir build -R "CryptoEngine" --output-on-failure
```

Schema / folder migration tests live in `tests/storage` and `tests/integration` (`test_migration.cpp`).

## 4. Installer

```powershell
cmake --install build --config Release --prefix build/install
cmake --build build --target package_inno
```

`package_inno` runs `windeployqt` on `yuli-vault-ui.exe` (needs vcpkg `qtbase[windeployqt]`), then `packaging/yuli-vault.iss`. Output: `build/package/yuli-vault-4.0.0-setup.exe`.

The installer AppId is new so it can coexist with PwdVault. Languages: English + Simplified Chinese.

## 5. Debugging

- Run `yuli-vault-service.exe --foreground` then the UI, or let the UI start the service.
- Override the pipe with `--pipe-name=\\.\pipe\YourName` on the service (and match the UI client if you test that path).
- Vault files: `%APPDATA%\YuliVault\`

## 6. Translations

`src/ui/translations/yuli-vault_zh_CN.ts` is compiled with `lrelease` when Qt LinguistTools is available and embedded as `:/translations/yuli-vault_zh_CN.qm`. English needs no `.qm` (source strings are English). Missing tools skip `.qm`; the UI still starts in English.

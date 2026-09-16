# Yuli Vault development

Workflow for contributors. Environment: [BUILD.md](BUILD.md). Architecture: [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Git

GitHub Flow: branch from `main`, open a PR. Names: `feat/…`, `fix/…`, `docs/…`.

## 2. Commits

Conventional, English, imperative. Examples:

- `feat: encrypt whole vault item payloads`
- `fix: migrate v2 rows after unlock`
- `docs: describe YuliVault data directory`

Do not commit secrets, local `build/`, or `.user` files.

## 3. Versioning

Product version is **4.0.0** (breaking identity + schema). Set it in:

| File | Field |
| --- | --- |
| `CMakeLists.txt` | `project(YuliVault VERSION …)` |
| `vcpkg.json` | `version-string` |
| `packaging/yuli-vault.iss` | `MyAppVersion` |

UI reads `YULI_VAULT_VERSION` from generated `Version.h`.

Installer AppId `{B3E1A9C4-7D52-4F18-9A6B-2C8E4F1D0A77}` is new; PwdVault installs are left alone.

GitHub: https://github.com/YuliannusYin/yuli-vault

## 4. Dependencies

vcpkg manifest: OpenSSL, libsodium, SQLite3, gtest, qtbase (`windeployqt`), qtsvg, qttools (`linguist`; also pulls `designer` via the port). Prefer no new packages. License and binary size must be reviewed first.

## 5. Releases

1. Bump version in the three files above
2. Update README if user-visible behavior changed
3. Tag `v4.x.x`
4. Build Release + `package_inno`
5. Attach `yuli-vault-4.x.x-setup.exe`

## 6. Do not

- Edit `legacy-python/`
- Reuse IPC command IDs
- Drop user tables on schema upgrade
- Localize SDK warning strings (use keys; UI `tr()` maps them)

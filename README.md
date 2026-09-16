# Yuli Vault

A local encrypted vault for logins. Data stays on your computer. Nothing is sent to the cloud.

English is the default language of the app. If Windows is set to Chinese, the UI loads Simplified Chinese. You can also pick **System / English / 简体中文** in Settings.

中文说明见 [README.zh-CN.md](README.zh-CN.md).

---

## What it does

- **Local first**: vault files live in `%APPDATA%\YuliVault\`
- **Optional encryption**: a program password derives an AES-256-GCM key with Argon2id
- **Two processes**: the Qt UI talks to `yuli-vault-service.exe` over a named pipe
- **No telemetry**

This 4.x release stores a unified `VaultItem` model. The editor is **Login only** (title, account, username, password, website, tags, Markdown notes). Secure note, card, identity, and custom types are reserved in the database for later versions.

---

## Features

### Vault (logins)

- Add, edit, delete login items
- Search title, account, username, website, and notes
- Tags, timestamps, copy-to-clipboard (clears after 30 seconds)
- Markdown notes in the detail pane

### Password generator

- Length 4–128, charset toggles, exclude similar characters
- Strength estimate (five bands) with pattern warnings
- History with a configurable retention limit

### Security

- Program password: enable / change / disable
- Five failed unlocks lock the vault for 5 minutes
- Auto-lock after idle time
- Lock from the sidebar, tray, or Settings

When a program password is on, **the whole login payload is encrypted** (account, username, password, website, notes). Title, type, and tags stay plaintext so the list can still filter. Plaintext mode stores the payload unencrypted.

---

## Install

Download a release installer (`yuli-vault-4.x.x-setup.exe`) or build from source ([docs/BUILD.md](docs/BUILD.md)).

Yuli Vault can sit next to an older PwdVault install. It uses a new Inno Setup AppId. Data is **copied**, not moved:

1. If `%APPDATA%\YuliVault\` already exists, that folder is used.
2. Else if `%APPDATA%\PwdVault\` exists, it is copied to `YuliVault\` and a `migrated_from_pwdvault` marker is written. The old folder is kept as a backup.
3. Otherwise a new `YuliVault\` folder is created.

A v2 PwdVault database (`passwords` table, password-only ciphertext) is upgraded to schema v3 (`vault_items`, payload ciphertext) on first plaintext start, or on the first successful unlock if a program password is set.

---

## Usage

1. Start **Yuli Vault** (`yuli-vault-ui.exe`). The UI starts the service if needed.
2. Optionally enable a program password under Settings.
3. Create logins from **New Item**. Browse them in **Vault**.
4. Generate passwords in **Generator**.

---

## Data locations

| File | Path |
| --- | --- |
| Database | `%APPDATA%\YuliVault\vault.db` |
| Wrapped encryption key | `%APPDATA%\YuliVault\vault.meta` |
| Named pipe | `\\.\pipe\YuliVaultService` |

---

## Build from source

See [docs/BUILD.md](docs/BUILD.md). Short version:

```powershell
$env:VCPKG_ROOT = "<vcpkg>"
$env:CMAKE_PREFIX_PATH = "<Qt 6>\msvc2022_64"
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

---

## License

[MIT](LICENSE). Source: [github.com/YuliannusYin/yuli-vault](https://github.com/YuliannusYin/yuli-vault).

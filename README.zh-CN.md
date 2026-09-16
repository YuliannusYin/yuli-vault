# Yuli Vault

本地加密保险库。数据只保存在你的电脑上，不上传云端。

应用默认语言是英语。若系统语言为中文，会加载简体中文界面。也可在设置中选择 **跟随系统 / English / 简体中文**。

English docs: [README.md](README.md).

---

## 它做什么

- **本地优先**：数据目录 `%APPDATA%\YuliVault\`
- **可选加密**：程序密码经 Argon2id 派生 AES-256-GCM 密钥
- **双进程**：Qt 界面通过命名管道调用 `yuli-vault-service.exe`
- **无遥测**

4.x 使用统一的 `VaultItem` 模型。当前编辑器**只支持登录条目**（标题、账号、用户名、密码、网站、标签、Markdown 备注）。安全笔记、卡片、身份、自定义类型已在库中预留，后续版本再提供编辑界面。

---

## 功能

### 保险库（登录）

- 添加、编辑、删除登录条目
- 按标题、账号、用户名、网站、备注搜索
- 标签、时间戳、复制到剪贴板（30 秒后清空）
- 详情页渲染 Markdown 备注

### 密码生成器

- 长度 4–128，字符集开关，排除易混字符
- 五级强度评估与模式警告
- 可配置上限的生成历史

### 安全

- 程序密码：启用 / 修改 / 禁用
- 连续 5 次解锁失败锁定 5 分钟
- 空闲自动锁定
- 侧边栏、托盘或设置中均可锁定

启用程序密码后，**整份登录载荷都会加密**（账号、用户名、密码、网站、备注）。标题、类型和标签保持明文，便于列表筛选。明文模式不加密载荷。

---

## 安装

下载安装包（`yuli-vault-4.x.x-setup.exe`），或按 [docs/BUILD.md](docs/BUILD.md) 自行编译。

Yuli Vault 可与旧版 PwdVault 同时安装（新的 Inno Setup AppId）。数据是**复制迁移**，不会删除旧目录：

1. 若 `%APPDATA%\YuliVault\` 已存在，直接使用。
2. 否则若存在 `%APPDATA%\PwdVault\`，则递归复制到 `YuliVault\`，并写入 `migrated_from_pwdvault` 标记；旧目录保留作备份。
3. 否则新建 `YuliVault\`。

PwdVault 的 v2 库（`passwords` 表，仅加密 password 列）会升级到 schema v3（`vault_items`，整包载荷加密）：无程序密码时在启动时升级；有程序密码时在**首次成功解锁**后升级。

---

## 使用

1. 启动 **Yuli Vault**（`yuli-vault-ui.exe`）。界面会按需拉起服务进程。
2. 可在设置中启用程序密码。
3. 在 **新建条目** 中创建登录，在 **保险库** 中浏览。
4. 在 **生成器** 中生成密码。

---

## 数据位置

| 文件 | 路径 |
| --- | --- |
| 数据库 | `%APPDATA%\YuliVault\vault.db` |
| 包装后的加密密钥 | `%APPDATA%\YuliVault\vault.meta` |
| 命名管道 | `\\.\pipe\YuliVaultService` |

---

## 从源码构建

见 [docs/BUILD.md](docs/BUILD.md)。

```powershell
$env:VCPKG_ROOT = "<vcpkg>"
$env:CMAKE_PREFIX_PATH = "<Qt 6>\msvc2022_64"
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

---

## 许可证

[MIT](LICENSE)。源码：[github.com/YuliannusYin/yuli-vault](https://github.com/YuliannusYin/yuli-vault)。

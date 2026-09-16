# Yuli Vault IPC protocol

Binary protocol between `yuli-vault-ui.exe` and `yuli-vault-service.exe`. See [ARCHITECTURE.md](ARCHITECTURE.md) for process layout.

## 1. Transport

| Property | Value |
| --- | --- |
| Pipe | `\\.\pipe\YuliVaultService` |
| Type | `PIPE_TYPE_BYTE` / `PIPE_READMODE_BYTE` |
| I/O | OVERLAPPED |
| Instances | `PIPE_UNLIMITED_INSTANCES` |
| Client timeout | 10 s per request, 3 connect retries at 500 ms |

## 2. Frame

16-byte `MessageHeader` + payload:

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 4 | magic | `0x50564456` ("PDVV" little-endian). Unchanged from PwdVault. |
| 4 | 2 | version | **2** (VaultItem type byte). Ship UI and service together. |
| 6 | 2 | command | `CommandId` |
| 8 | 4 | request_id | Matches response |
| 12 | 4 | payload_size | Bytes after the header |

Little-endian. Strings and blobs are `uint32` length + bytes. Compound structs serialize fields in declaration order. Empty requests have `payload_size = 0`.

## 3. Commands

Defined in `src/sdk/protocol/Commands.h`.

### System `0x00xx`

| Id | Value | Request | Response |
| --- | --- | --- | --- |
| Ping | 0x0001 | empty | `PingResponse` |
| Shutdown | 0x0002 | empty | empty |

### Session `0x01xx`

| Id | Value | Request | Response |
| --- | --- | --- | --- |
| Unlock | 0x0101 | password | success + `error_message` |
| Lock | 0x0102 | empty | empty |
| EnableProgramPassword | 0x0103 | password | success + message |
| DisableProgramPassword | 0x0104 | password | success + message |
| ChangeProgramPassword | 0x0105 | old + new | success + message |
| GetVaultStatus | 0x0106 | empty | enabled / locked |

Unlock `error_message` (English, parsed by the UI):

- `Incorrect password, N attempts remaining`
- `Too many incorrect passwords, locked. Retry in N seconds`
- `Locked, retry in N seconds`

### Items `0x02xx`

Command IDs are unchanged. Payloads are `VaultItem` / `PasswordEntry`:

```
id i64 | type u8 | title | account | username | password | website | note
| tags | created_at i64 | updated_at i64 | iv | tag
```

`type` is `VaultItemType` (Login = 1, …). Title/type/tags are what the list shows; secret fields travel in the clear over the pipe after the service decrypts (the pipe is local-only).

| Id | Value |
| --- | --- |
| AddEntry | 0x0200 |
| UpdateEntry | 0x0201 |
| RemoveEntry | 0x0202 |
| GetEntry | 0x0203 |
| SearchEntries | 0x0204 |
| ListEntries | 0x0205 |

Search `fields` accept `title` (alias `entry_name`), `account`, `username`, `website`, `note`. Encrypted login fields are matched after decrypt in ServiceCore.

### Generator `0x03xx`

GeneratePassword, EstimateStrength, List/Remove/Clear generated records, GetGeneratorSettings, SetGeneratorLimit.

Strength `warnings` are stable keys: `repeat:<n>`, `uneven`, `sequential:<n>`, `keyboard:<n>`. The UI translates them.

### Tags `0x04xx`

Add/Update/Remove/List/Get/FindTagByName, GetEntryTags, SetEntryTags.

## 4. On-disk payload (not the IPC frame)

Item type fields are encoded separately for SQLite (`src/sdk/core/VaultPayload.h`):

```
u8 payload_version (1) | u8 item_type | account | username | password | website | note
```

With a program password, ServiceCore AES-GCM-encrypts that blob into `vault_items.payload` + `iv` + `tag`. Without a program password the blob is stored as-is.

## 5. Errors

Failures may return `ErrorResponse` (`ErrorCode` + message) instead of the success struct. UI maps codes through `ErrorMessages.cpp` (English source, zh_CN translation).

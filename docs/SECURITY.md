# Yuli Vault security

Threat model, algorithms, key hierarchy, and known limits. See [ARCHITECTURE.md](ARCHITECTURE.md) and [IPC_PROTOCOL.md](IPC_PROTOCOL.md).

## 1. Threat model

### Local user

Encrypted mode: item **payloads** (account, username, password, website, notes) are AES-256-GCM. Title, type, and tags are plaintext for listing. `encryption_key` in `vault.meta` is wrapped with an Argon2id KEK from the program password. Files sit under `%APPDATA%\YuliVault\` with the user's NTFS ACL.

**Plaintext mode**: payloads are stored unencrypted. Anyone who can read `vault.db` can read secrets. Use a program password for anything sensitive.

### Memory capture

Keys and secrets are wiped with `sodium_memzero`. KEK lives only on the stack in `ProgramPasswordStore`. `encryption_key` lives in the service until lock or exit. The UI never holds the key.

### Stolen files

Offline attack needs `vault.db` + `vault.meta` + the program password. Argon2id INTERACTIVE parameters slow guesses. GCM tags detect tampering.

### Online guessing

Five failed unlocks lock the vault for five minutes. Messages stay generic aside from remaining attempts / cooldown seconds for the UI.

## 2. Algorithms

| Use | Algorithm | Library |
| --- | --- | --- |
| Item payload | AES-256-GCM (96-bit IV, 128-bit tag) | OpenSSL EVP |
| KDF | Argon2id INTERACTIVE | libsodium |
| RNG | `randombytes_buf` / `BCryptGenRandom` | libsodium / Windows |
| Wipe | `sodium_memzero` | libsodium |

v2 PwdVault databases encrypted **only the password column**. After migrate, the whole payload is encrypted. Notes and accounts are no longer stored in plaintext once a program password is enabled.

## 3. Key hierarchy

```
program password
    Argon2id(+salt in vault.meta) → KEK
        unwraps encryption_key
            AES-GCM per item payload and generated-password records
```

Changing the program password rewraps `encryption_key` (items stay). Disabling it decrypts everything and deletes `vault.meta`.

## 4. Limits (honest)

- Windows only; named pipe is local but not a full sandbox
- Title, type, and tags remain plaintext
- UI clipboard holds secrets until the 30-second clear
- Dual-process isolation is not a kernel boundary
- No remote sync, attachments, TOTP, or import in this release
- `legacy-python/` is archival and unmaintained

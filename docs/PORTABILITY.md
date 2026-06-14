# Portability Notes

LLM Usage Tray uses SDL3 for the tray/window UI and has platform backends for
Windows, macOS, and Linux.

## Windows Backend

Implemented:

- secure credential storage via DPAPI
- HTTP via WinHTTP
- browser launch via `ShellExecuteW`
- OAuth localhost callback server via Winsock
- PKCE random bytes and SHA-256 via BCrypt
- JWT payload base64 decode via Crypt32

## macOS Backend

Implemented:

- credential storage through Keychain's `security` command
- HTTP through libcurl
- browser launch through `open`
- localhost callback server through BSD sockets
- PKCE crypto through portable SHA-256 plus OS random
- tray/window UI through SDL3

## Linux Backend

Implemented:

- credential storage through Secret Service's `secret-tool`
- HTTP through libcurl
- browser launch through `xdg-open`
- localhost callback server through BSD sockets
- PKCE crypto through portable SHA-256 plus `getrandom`/`/dev/urandom`
- tray/window UI through SDL3

## Current CI Policy

CI builds Windows, macOS, and Linux release artifacts. Linux CI installs
`libcurl4-openssl-dev` and `libsecret-tools`.

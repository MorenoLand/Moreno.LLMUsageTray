# Cross-provider quota monitor

Small C++/SDL tray app for watching LLM quota windows without opening each
provider's settings page.

## Current Support

| Provider | Auth | Usage windows |
| --- | --- | --- |
| GPT/Codex | Browser OAuth via ChatGPT | 5 hour, weekly |
| Claude | Browser OAuth via Anthropic | 5 hour, weekly |
| GLM/Z.ai | Saved API key | 5 hour quota, MCP/tool requests |
| Gemini/Antigravity | Google OAuth with hosted verification code | 5 hour, weekly |
| Grok | Browser OAuth via auth.x.ai (Grok CLI) | Weekly Grok CLI, weekly Grok Bot |

## Platform Status

The app builds on Windows, macOS, and Linux.

Backend notes:

- Credentials are saved through the platform's local credential store.
- Linux users need a working tray/status notifier environment.

## Security

The tray UI never prints access tokens. GPT, Claude, Gemini, and Grok use OAuth
tokens; GLM stores only the API key you enter.

## Build

Requirements:

- CMake 3.20+
- Git, for fetching pinned SDL dependencies
- Windows: Visual Studio 2022 or newer with C++ tools
- macOS: Xcode command line tools and curl
- Linux: C++ compiler, curl development package, and `secret-tool`

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target LLMUsageTray
```

macOS/Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target LLMUsageTray
```

The executable is written to:

```text
build\windows-release\Release\LLMUsageTray.exe
```

or, with a Visual Studio generator in `build`:

```text
build\Release\LLMUsageTray.exe
```

On macOS/Linux it is written to:

```text
build/LLMUsageTray
```

## Use

Run `LLMUsageTray.exe`. It starts in the system tray.

The popup is a compact ring dock plus a detail card:

- one ring per enabled provider, showing that provider's primary window as percent remaining by default (toggleable to used)
- click a ring for the callout (session/CLI and weekly/Bot bars, reset times)
- gear opens Settings to enable providers, sign in or out, and refresh
- pin keeps the popup open; drag the cards to reposition
- right-click the dock to toggle Settings

GLM uses:

```text
https://api.z.ai/api/monitor/usage/quota/limit
```

with:

```text
Authorization: Bearer <key>
```

## OAuth Notes


- OAuth client id: `app_EMoamEEZ73f0CkXaXp7hrann`
- Callback: `http://localhost:1455/auth/callback`
- Token endpoint: `https://auth.openai.com/oauth/token`
- Usage endpoint: `https://chatgpt.com/backend-api/wham/usage`


- OAuth client id: `9d1c250a-e61b-44d9-88ed-5944d1962f5e`
- Callback: `http://localhost:53692/callback`
- Token endpoint: `https://platform.claude.com/v1/oauth/token`
- Usage endpoint: `https://api.anthropic.com/api/oauth/usage`
- Usage headers include `anthropic-beta: oauth-2025-04-20` and
  `User-Agent: claude/1.0`

Gemini mirrors the Antigravity OAuth flow:

- Google authorization uses the hosted `https://antigravity.google/oauth-callback` page
- Copy the one-time code shown by that page into the tray's Gemini verification field
- Quota reads AGY's local `RetrieveUserQuotaSummary` endpoint when AGY is running
- Environment overrides are `LLM_USAGE_TRAY_GEMINI_CLIENT_ID` and
  `LLM_USAGE_TRAY_GEMINI_CLIENT_SECRET`; otherwise the installed AGY binary is inspected

Grok uses the public Grok CLI OAuth client against `auth.x.ai`:

- OAuth client id: `b1a00492-073a-47ea-816f-4c329264a828`
- Callback: `http://127.0.0.1:56121/callback`
- Token endpoint: `https://auth.x.ai/oauth2/token`
- Usage endpoint: `https://cli-chat-proxy.grok.com/v1/billing?format=credits`
- Usage headers include `X-XAI-Token-Auth: xai-grok-cli`
- Bars are Grok CLI (`GrokBuild`) and Grok Bot (`GrokChat`) weekly product windows from that payload

## Debug

Run `LLMUsageTray.exe --debug` from a terminal. Redacted diagnostics are mirrored to
the terminal and written to `app.log` beside the executable. Normal launches do not
emit diagnostics.

## License

MIT. See `LICENSE`.

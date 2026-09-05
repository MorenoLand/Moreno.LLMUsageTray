#include "oauth.h"

#include "base64url.h"
#include "credential_store.h"
#include "diagnostics.h"
#include "http_client.h"
#include "json_util.h"
#include "platform.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

static constexpr const char* kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";
static constexpr const char* kAuthorizeUrl = "https://auth.openai.com/oauth/authorize";
static constexpr const char* kTokenUrl = "https://auth.openai.com/oauth/token";
static constexpr const char* kRedirectUri = "http://localhost:1455/auth/callback";
static constexpr const char* kScope = "openid profile email offline_access";

static constexpr const char* kAnthropicClientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static constexpr const char* kAnthropicAuthorizeUrl = "https://claude.ai/oauth/authorize";
static constexpr const char* kAnthropicTokenUrl = "https://platform.claude.com/v1/oauth/token";
static constexpr const char* kAnthropicRedirectUri = "http://localhost:53692/callback";
static constexpr const char* kAnthropicScope = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";

static constexpr const char* kGeminiAuthorizeUrl = "https://accounts.google.com/o/oauth2/auth";
static constexpr const char* kGeminiTokenUrl = "https://oauth2.googleapis.com/token";
static constexpr const char* kGeminiRedirectUri = "https://antigravity.google/oauth-callback";
static constexpr const char* kGeminiScope = "email profile https://www.googleapis.com/auth/cloud-platform https://www.googleapis.com/auth/userinfo.email https://www.googleapis.com/auth/userinfo.profile https://www.googleapis.com/auth/cclog https://www.googleapis.com/auth/experimentsandconfigs https://www.googleapis.com/auth/aicode openid";
static constexpr const char* kGeminiCodeAssistUrls[] = {
    "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist",
    "https://daily-cloudcode-pa.googleapis.com/v1internal:loadCodeAssist",
};

static constexpr const char* kGrokClientId = "b1a00492-073a-47ea-816f-4c329264a828";
static constexpr const char* kGrokAuthorizeUrl = "https://auth.x.ai/oauth2/authorize";
static constexpr const char* kGrokTokenUrl = "https://auth.x.ai/oauth2/token";
static constexpr const char* kGrokRedirectUri = "http://127.0.0.1:56121/callback";
static constexpr const char* kGrokScope = "openid profile email offline_access grok-cli:access api:access";

struct GeminiOAuthConfig {
    std::string client_id;
    std::string client_secret;
};

static std::optional<std::filesystem::path> find_agy_binary() {
    std::vector<std::filesystem::path> candidates;
    const char* path_value = std::getenv("PATH");
    if (path_value) {
        std::string paths = path_value;
        std::size_t start = 0;
        while (start <= paths.size()) {
            std::size_t end = paths.find(
#if defined(_WIN32)
                ';'
#else
                ':'
#endif
                , start);
            if (end == std::string::npos) end = paths.size();
            if (end > start) {
                std::filesystem::path directory = paths.substr(start, end - start);
                candidates.push_back(directory / "agy.exe");
                candidates.push_back(directory / "agy");
            }
            if (end == paths.size()) break;
            start = end + 1;
        }
    }
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data) candidates.push_back(std::filesystem::path(local_app_data) / "agy" / "bin" / "agy.exe");
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile) candidates.push_back(std::filesystem::path(user_profile) / ".local" / "bin" / "agy.exe");
    const char* home = std::getenv("HOME");
    if (home) candidates.push_back(std::filesystem::path(home) / ".local" / "bin" / "agy");
    std::error_code error;
    for (const auto& candidate : candidates) if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    return std::nullopt;
}

static std::string read_binary_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static bool token_character(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.';
}

static std::vector<std::pair<std::size_t, std::string>> agy_oauth_client_ids(const std::string& text) {
    std::vector<std::pair<std::size_t, std::string>> values;
    const std::string suffix = ".apps.googleusercontent.com";
    std::size_t search = 0;
    while ((search = text.find(suffix, search)) != std::string::npos) {
        std::size_t dash_search = search;
        while (dash_search > 0 && search - dash_search <= 256) {
            std::size_t dash = text.rfind('-', dash_search - 1);
            if (dash == std::string::npos || search - dash > 256) break;
            std::size_t start = dash;
            while (start > 0 && std::isdigit(static_cast<unsigned char>(text[start - 1]))) --start;
            if (dash - start >= 6) {
                std::string value = text.substr(start, search + suffix.size() - start);
                if (std::all_of(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(dash - start), [](unsigned char c) { return std::isdigit(c); }) && std::all_of(value.begin() + static_cast<std::ptrdiff_t>(dash - start), value.end(), [](unsigned char c) { return token_character(c); })) {
                    values.emplace_back(start, std::move(value));
                    break;
                }
            }
            dash_search = dash;
        }
        search += suffix.size();
    }
    return values;
}

static std::vector<std::pair<std::size_t, std::string>> agy_oauth_client_secrets(const std::string& text) {
    std::vector<std::pair<std::size_t, std::string>> values;
    const std::string prefix = "GOCSPX-";
    std::size_t search = 0;
    while ((search = text.find(prefix, search)) != std::string::npos) {
        std::size_t end = search + prefix.size();
        while (end < text.size() && token_character(static_cast<unsigned char>(text[end]))) {
            if (end > search + prefix.size() && text.compare(end, prefix.size(), prefix) == 0) break;
            ++end;
        }
        values.emplace_back(search, text.substr(search, end - search));
        search = end;
    }
    return values;
}

static int agy_oauth_context_score(const std::string& text, std::size_t offset) {
    std::size_t start = offset > 4096 ? offset - 4096 : 0;
    std::size_t length = std::min<std::size_t>(8192, text.size() - start);
    std::string context = text.substr(start, length);
    int score = 0;
    for (const char* marker : {"CLOUD_CODE_URL", "CloudCodeServerURL", "cloudcode-pa.googleapis.com", "oauth", "OAuth"}) if (context.find(marker) != std::string::npos) ++score;
    return score;
}

static std::optional<GeminiOAuthConfig> discover_agy_oauth_config() {
    auto path = find_agy_binary();
    if (!path) {
        diagnostics_log("gemini oauth config source=none agy_not_found");
        return std::nullopt;
    }
    std::string text = read_binary_text(*path);
    if (text.empty()) {
        diagnostics_log("gemini oauth config source=none agy_binary_unreadable");
        return std::nullopt;
    }
    auto ids = agy_oauth_client_ids(text);
    auto secrets = agy_oauth_client_secrets(text);
    if (ids.empty() || secrets.empty()) {
        diagnostics_log("gemini oauth config source=none agy_values_not_found");
        return std::nullopt;
    }
    auto best_id = std::max_element(ids.begin(), ids.end(), [&](const auto& left, const auto& right) { return agy_oauth_context_score(text, left.first) < agy_oauth_context_score(text, right.first); });
    auto best_secret = std::max_element(secrets.begin(), secrets.end(), [&](const auto& left, const auto& right) { return agy_oauth_context_score(text, left.first) < agy_oauth_context_score(text, right.first); });
    diagnostics_log("gemini oauth config source=agy path=" + path->string() + " client_candidates=" + std::to_string(ids.size()) + " secret_candidates=" + std::to_string(secrets.size()) + " selected_secret_length=" + std::to_string(best_secret->second.size()));
    return GeminiOAuthConfig{best_id->second, best_secret->second};
}

static GeminiOAuthConfig gemini_oauth_config() {
    const char* client_id = std::getenv("LLM_USAGE_TRAY_GEMINI_CLIENT_ID");
    const char* client_secret = std::getenv("LLM_USAGE_TRAY_GEMINI_CLIENT_SECRET");
    std::optional<GeminiOAuthConfig> discovered = (client_id && *client_id && client_secret && *client_secret) ? std::nullopt : discover_agy_oauth_config();
    std::string resolved_id = client_id && *client_id ? client_id : (discovered ? discovered->client_id : "");
    std::string resolved_secret = client_secret && *client_secret ? client_secret : (discovered ? discovered->client_secret : "");
    if (resolved_id.empty() || resolved_secret.empty()) throw std::runtime_error("Install AGY or set LLM_USAGE_TRAY_GEMINI_CLIENT_ID and LLM_USAGE_TRAY_GEMINI_CLIENT_SECRET before Gemini login");
    diagnostics_log("gemini oauth config resolved client_id_length=" + std::to_string(resolved_id.size()) + " client_secret_length=" + std::to_string(resolved_secret.size()) + " env_override=" + std::string(client_id && *client_id && client_secret && *client_secret ? "true" : "false"));
    return {resolved_id, resolved_secret};
}

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string url_escape(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::string create_authorize_url(const std::string& challenge, const std::string& state) {
    std::string url = kAuthorizeUrl;
    url += "?response_type=code";
    url += "&client_id=" + url_escape(kClientId);
    url += "&redirect_uri=" + url_escape(kRedirectUri);
    url += "&scope=" + url_escape(kScope);
    url += "&code_challenge=" + url_escape(challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + url_escape(state);
    url += "&id_token_add_organizations=true";
    url += "&codex_cli_simplified_flow=true";
    url += "&originator=" + url_escape("codex-usage-tray");
    return url;
}

static std::string create_anthropic_authorize_url(const std::string& challenge, const std::string& state) {
    std::string url = kAnthropicAuthorizeUrl;
    url += "?code=true";
    url += "&client_id=" + url_escape(kAnthropicClientId);
    url += "&response_type=code";
    url += "&redirect_uri=" + url_escape(kAnthropicRedirectUri);
    url += "&scope=" + url_escape(kAnthropicScope);
    url += "&code_challenge=" + url_escape(challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + url_escape(state);
    return url;
}

static std::string create_gemini_authorize_url(const std::string& client_id, const std::string& challenge, const std::string& state) {
    std::string url = kGeminiAuthorizeUrl;
    url += "?access_type=offline";
    url += "&client_id=" + url_escape(client_id);
    url += "&code_challenge=" + url_escape(challenge);
    url += "&code_challenge_method=S256";
    url += "&prompt=consent";
    url += "&redirect_uri=" + url_escape(kGeminiRedirectUri);
    url += "&response_type=code";
    url += "&scope=" + url_escape(kGeminiScope);
    url += "&state=" + url_escape(state);
    return url;
}

static std::string create_grok_authorize_url(const std::string& challenge, const std::string& state) {
    std::string url = kGrokAuthorizeUrl;
    url += "?response_type=code";
    url += "&client_id=" + url_escape(kGrokClientId);
    url += "&redirect_uri=" + url_escape(kGrokRedirectUri);
    url += "&scope=" + url_escape(kGrokScope);
    url += "&code_challenge=" + url_escape(challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + url_escape(state);
    return url;
}

static std::string jwt_payload(const std::string& access_token) {
    std::size_t first = access_token.find('.');
    if (first == std::string::npos) return "";
    std::size_t second = access_token.find('.', first + 1);
    if (second == std::string::npos) return "";
    auto payload = base64url_decode(access_token.substr(first + 1, second - first - 1));
    if (payload.empty()) return "";
    return std::string(payload.begin(), payload.end());
}

static std::string jwt_claim(const std::string& access_token, const std::string& key) {
    std::string decoded = jwt_payload(access_token);
    return decoded.empty() ? "" : json_string(decoded, key).value_or("");
}

static std::string jwt_account_id(const std::string& access_token) {
    return jwt_claim(access_token, "chatgpt_account_id");
}

static std::string grok_account_id(const std::string& access_token) {
    std::string email = jwt_claim(access_token, "email");
    if (!email.empty()) return email;
    email = jwt_claim(access_token, "preferred_username");
    if (!email.empty()) return email;
    return jwt_claim(access_token, "sub");
}

static OAuthCredentials exchange_code(const std::string& code, const std::string& verifier) {
    HttpResponse res = http_post_form(kTokenUrl, {
        {"grant_type", "authorization_code"},
        {"client_id", kClientId},
        {"code", code},
        {"code_verifier", verifier},
        {"redirect_uri", kRedirectUri},
    });
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Token exchange failed: HTTP " + std::to_string(res.status) + " " + res.body);
    }
    std::string access = json_string(res.body, "access_token").value_or("");
    std::string refresh = json_string(res.body, "refresh_token").value_or("");
    double expires_in = json_number(res.body, "expires_in").value_or(0);
    if (access.empty() || refresh.empty() || expires_in <= 0) {
        throw std::runtime_error("Token response missing access_token, refresh_token, or expires_in");
    }
    OAuthCredentials credentials;
    credentials.access = access;
    credentials.refresh = refresh;
    credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0);
    credentials.account_id = jwt_account_id(access);
    return credentials;
}

static OAuthCredentials exchange_anthropic_code(const std::string& code, const std::string& verifier) {
    std::string body = "{";
    body += "\"grant_type\":\"authorization_code\",";
    body += "\"client_id\":\"" + json_escape(kAnthropicClientId) + "\",";
    body += "\"code\":\"" + json_escape(code) + "\",";
    body += "\"state\":\"" + json_escape(verifier) + "\",";
    body += "\"redirect_uri\":\"" + json_escape(kAnthropicRedirectUri) + "\",";
    body += "\"code_verifier\":\"" + json_escape(verifier) + "\"";
    body += "}";
    HttpResponse res = http_post_json(kAnthropicTokenUrl, body);
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Claude token exchange failed: HTTP " + std::to_string(res.status) + " " + res.body);
    }
    std::string access = json_string(res.body, "access_token").value_or("");
    std::string refresh = json_string(res.body, "refresh_token").value_or("");
    double expires_in = json_number(res.body, "expires_in").value_or(0);
    if (access.empty() || refresh.empty() || expires_in <= 0) {
        throw std::runtime_error("Claude token response missing access_token, refresh_token, or expires_in");
    }
    OAuthCredentials credentials;
    credentials.access = access;
    credentials.refresh = refresh;
    credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0) - 5 * 60 * 1000;
    return credentials;
}

static OAuthCredentials exchange_gemini_code(const std::string& code, const std::string& verifier, const GeminiOAuthConfig& config) {
    HttpResponse res = http_post_form(kGeminiTokenUrl, {
        {"grant_type", "authorization_code"},
        {"client_id", config.client_id},
        {"client_secret", config.client_secret},
        {"code", code},
        {"code_verifier", verifier},
        {"redirect_uri", kGeminiRedirectUri},
    });
    diagnostics_log("gemini token exchange status=" + std::to_string(res.status) + " body_length=" + std::to_string(res.body.size()) + " error=" + json_string(res.body, "error").value_or("none"));
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Gemini token exchange failed: HTTP " + std::to_string(res.status));
    }
    std::string access = json_string(res.body, "access_token").value_or("");
    std::string refresh = json_string(res.body, "refresh_token").value_or("");
    double expires_in = json_number(res.body, "expires_in").value_or(0);
    if (access.empty() || refresh.empty() || expires_in <= 0) {
        throw std::runtime_error("Gemini token response missing access_token, refresh_token, or expires_in");
    }
    OAuthCredentials credentials;
    credentials.access = access;
    credentials.refresh = refresh;
    credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0);
    return credentials;
}

static OAuthCredentials exchange_grok_code(const std::string& code, const std::string& verifier) {
    HttpResponse res = http_post_form(kGrokTokenUrl, {
        {"grant_type", "authorization_code"},
        {"client_id", kGrokClientId},
        {"code", code},
        {"code_verifier", verifier},
        {"redirect_uri", kGrokRedirectUri},
    });
    diagnostics_log("grok token exchange status=" + std::to_string(res.status) + " body_length=" + std::to_string(res.body.size()) + " error=" + json_string(res.body, "error").value_or("none"));
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Grok token exchange failed: HTTP " + std::to_string(res.status));
    }
    std::string access = json_string(res.body, "access_token").value_or("");
    std::string refresh = json_string(res.body, "refresh_token").value_or("");
    double expires_in = json_number(res.body, "expires_in").value_or(0);
    if (access.empty() || refresh.empty() || expires_in <= 0) {
        throw std::runtime_error("Grok token response missing access_token, refresh_token, or expires_in");
    }
    OAuthCredentials credentials;
    credentials.access = access;
    credentials.refresh = refresh;
    credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0);
    credentials.account_id = grok_account_id(access);
    return credentials;
}

std::string gemini_discover_project(const std::string& access_token) {
    const std::string bodies[] = {
        "{\"metadata\":{\"ideType\":\"ANTIGRAVITY\"}}",
        "{\"metadata\":{\"ideType\":\"IDE_UNSPECIFIED\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}}",
    };
    int last_status = 0;
    for (const char* url : kGeminiCodeAssistUrls) {
        for (std::size_t body_index = 0; body_index < sizeof(bodies) / sizeof(bodies[0]); ++body_index) {
            const std::string& body = bodies[body_index];
            HttpResponse res = http_post_json(url, body, {
                {"Authorization", "Bearer " + access_token},
                {"User-Agent", "antigravity/cli/1.1.24 windows/amd64"},
            });
            last_status = res.status;
            diagnostics_log("gemini project discovery url=" + std::string(url) + " variant=" + std::to_string(body_index + 1) + " status=" + std::to_string(res.status) + " body_length=" + std::to_string(res.body.size()));
            if (res.status < 200 || res.status >= 300) continue;
            std::string project = json_string(res.body, "cloudaicompanionProject").value_or("");
            if (!project.empty()) {
                diagnostics_log("gemini project discovery result=present project_length=" + std::to_string(project.size()));
                return project;
            }
        }
    }
    if (last_status) throw std::runtime_error("Gemini project discovery failed: HTTP " + std::to_string(last_status));
    throw std::runtime_error("Gemini project discovery failed");
}

std::string oauth_provider_kind(const std::string& name) {
    std::size_t p = name.rfind('_');
    if (p == std::string::npos || p == 0 || p + 1 >= name.size()) return name;
    for (std::size_t i = p + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return name;
    }
    return name.substr(0, p);
}

OAuthCredentials oauth_login_browser() {
    return oauth_login_browser_provider("openai");
}

OAuthCredentials oauth_login_browser_provider(const std::string& provider) {
    std::string kind = oauth_provider_kind(provider);
    if (kind == "glm") {
        throw std::runtime_error("GLM OAuth is not configured yet");
    }
    std::string verifier = base64url_encode(random_bytes(kind == "grok" ? 96 : 32));
    std::string challenge = base64url_encode(sha256_bytes(verifier));
    std::string state = kind == "anthropic" ? verifier : base64url_encode(random_bytes(16));
    std::string url = kind == "anthropic"
        ? create_anthropic_authorize_url(challenge, state)
        : (kind == "grok" ? create_grok_authorize_url(challenge, state) : create_authorize_url(challenge, state));

    auto code_future = std::async(std::launch::async, [kind, state] {
        if (kind == "anthropic") {
            return wait_for_oauth_code_on(53692, "/callback", state, "Claude");
        }
        if (kind == "grok") {
            return wait_for_oauth_code_on(56121, "/callback", state, "Grok");
        }
        return wait_for_oauth_code_on(1455, "/auth/callback", state, "ChatGPT");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    open_browser(url);
    std::string code = code_future.get();
    OAuthCredentials credentials = kind == "anthropic"
        ? exchange_anthropic_code(code, verifier)
        : (kind == "grok" ? exchange_grok_code(code, verifier) : exchange_code(code, verifier));
    save_credentials_provider(provider, credentials);
    return credentials;
}

OAuthLoginSession oauth_begin_manual_login_provider(const std::string& provider) {
    OAuthLoginSession session;
    session.provider = provider;
    std::string kind = oauth_provider_kind(provider);
    if (kind == "grok") {
        session.verifier = base64url_encode(random_bytes(96));
        session.state = base64url_encode(random_bytes(16));
        session.client_id = kGrokClientId;
        session.authorize_url = create_grok_authorize_url(base64url_encode(sha256_bytes(session.verifier)), session.state);
        diagnostics_log("grok oauth browser start");
        open_browser(session.authorize_url);
        return session;
    }
    if (kind != "gemini") throw std::runtime_error("Manual OAuth is only configured for Gemini and Grok");
    GeminiOAuthConfig config = gemini_oauth_config();
    session.verifier = base64url_encode(random_bytes(32));
    session.state = base64url_encode(random_bytes(16));
    session.client_id = config.client_id;
    session.client_secret = config.client_secret;
    session.authorize_url = create_gemini_authorize_url(config.client_id, base64url_encode(sha256_bytes(session.verifier)), session.state);
    diagnostics_log("gemini oauth browser start");
    open_browser(session.authorize_url);
    return session;
}

OAuthCredentials oauth_finish_manual_login_provider(const OAuthLoginSession& session, const std::string& code) {
    std::string kind = oauth_provider_kind(session.provider);
    if (kind == "grok") {
        diagnostics_log("grok oauth code submit length=" + std::to_string(code.size()));
        OAuthCredentials credentials = exchange_grok_code(code, session.verifier);
        save_credentials_provider(session.provider, credentials);
        return credentials;
    }
    if (kind != "gemini") throw std::runtime_error("Manual OAuth is only configured for Gemini and Grok");
    if (session.client_id.empty() || session.client_secret.empty()) throw std::runtime_error("Gemini OAuth session is missing client configuration");
    GeminiOAuthConfig config{session.client_id, session.client_secret};
    diagnostics_log("gemini oauth code submit length=" + std::to_string(code.size()));
    OAuthCredentials credentials = exchange_gemini_code(code, session.verifier, config);
    save_credentials_provider(session.provider, credentials);
    credentials.account_id = gemini_discover_project(credentials.access);
    save_credentials_provider(session.provider, credentials);
    return credentials;
}

OAuthCredentials oauth_refresh(const std::string& refresh_token) {
    return oauth_refresh_provider("openai", refresh_token);
}

OAuthCredentials oauth_refresh_provider(const std::string& provider, const std::string& refresh_token) {
    std::string kind = oauth_provider_kind(provider);
    if (kind == "glm") {
        throw std::runtime_error("GLM OAuth is not configured yet");
    }
    if (kind == "anthropic") {
        std::string body = "{";
        body += "\"grant_type\":\"refresh_token\",";
        body += "\"client_id\":\"" + json_escape(kAnthropicClientId) + "\",";
        body += "\"refresh_token\":\"" + json_escape(refresh_token) + "\"";
        body += "}";
        HttpResponse res = http_post_json(kAnthropicTokenUrl, body);
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Claude token refresh failed: HTTP " + std::to_string(res.status));
        }
        std::string access = json_string(res.body, "access_token").value_or("");
        std::string refresh = json_string(res.body, "refresh_token").value_or("");
        double expires_in = json_number(res.body, "expires_in").value_or(0);
        if (access.empty() || refresh.empty() || expires_in <= 0) {
            throw std::runtime_error("Claude refresh response missing fields");
        }
        OAuthCredentials credentials;
        credentials.access = access;
        credentials.refresh = refresh;
        credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0) - 5 * 60 * 1000;
        save_credentials_provider(provider, credentials);
        return credentials;
    }
    if (kind == "gemini") {
        GeminiOAuthConfig config = gemini_oauth_config();
        HttpResponse res = http_post_form(kGeminiTokenUrl, {
            {"grant_type", "refresh_token"},
            {"client_id", config.client_id},
            {"client_secret", config.client_secret},
            {"refresh_token", refresh_token},
        });
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Gemini token refresh failed: HTTP " + std::to_string(res.status));
        }
        std::string access = json_string(res.body, "access_token").value_or("");
        std::string refresh = json_string(res.body, "refresh_token").value_or(refresh_token);
        double expires_in = json_number(res.body, "expires_in").value_or(0);
        if (access.empty() || refresh.empty() || expires_in <= 0) {
            throw std::runtime_error("Gemini refresh response missing fields");
        }
        OAuthCredentials credentials;
        credentials.access = access;
        credentials.refresh = refresh;
        credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0);
        save_credentials_provider(provider, credentials);
        return credentials;
    }
    if (kind == "grok") {
        HttpResponse res = http_post_form(kGrokTokenUrl, {
            {"grant_type", "refresh_token"},
            {"refresh_token", refresh_token},
            {"client_id", kGrokClientId},
        });
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Grok token refresh failed: HTTP " + std::to_string(res.status));
        }
        std::string access = json_string(res.body, "access_token").value_or("");
        std::string refresh = json_string(res.body, "refresh_token").value_or(refresh_token);
        double expires_in = json_number(res.body, "expires_in").value_or(0);
        if (access.empty() || refresh.empty() || expires_in <= 0) {
            throw std::runtime_error("Grok refresh response missing fields");
        }
        OAuthCredentials credentials;
        credentials.access = access;
        credentials.refresh = refresh;
        credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0);
        credentials.account_id = grok_account_id(access);
        save_credentials_provider(provider, credentials);
        return credentials;
    }

    HttpResponse res = http_post_form(kTokenUrl, {
        {"grant_type", "refresh_token"},
        {"refresh_token", refresh_token},
        {"client_id", kClientId},
    });
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Token refresh failed: HTTP " + std::to_string(res.status));
    }
    std::string access = json_string(res.body, "access_token").value_or("");
    std::string refresh = json_string(res.body, "refresh_token").value_or("");
    double expires_in = json_number(res.body, "expires_in").value_or(0);
    if (access.empty() || refresh.empty() || expires_in <= 0) {
        throw std::runtime_error("Refresh response missing fields");
    }
    OAuthCredentials credentials;
    credentials.access = access;
    credentials.refresh = refresh;
    credentials.expires_ms = now_ms() + static_cast<long long>(expires_in * 1000.0);
    credentials.account_id = jwt_account_id(access);
    save_credentials_provider(provider, credentials);
    return credentials;
}

std::optional<OAuthCredentials> load_credentials() {
    return load_credentials_provider("openai");
}

std::optional<OAuthCredentials> load_credentials_provider(const std::string& provider) {
    if (oauth_provider_kind(provider) == "glm") return std::nullopt;
    auto raw = credential_load_named(provider);
    if (!raw) return std::nullopt;
    OAuthCredentials credentials;
    credentials.access = json_string(*raw, "access").value_or("");
    credentials.refresh = json_string(*raw, "refresh").value_or("");
    credentials.expires_ms = static_cast<long long>(json_number(*raw, "expires_ms").value_or(0));
    credentials.account_id = json_string(*raw, "account_id").value_or("");
    if (credentials.access.empty() || credentials.refresh.empty() || credentials.expires_ms <= 0) {
        return std::nullopt;
    }
    return credentials;
}

void save_credentials(const OAuthCredentials& credentials) {
    save_credentials_provider("openai", credentials);
}

void save_credentials_provider(const std::string& provider, const OAuthCredentials& credentials) {
    std::string json = "{";
    json += "\"access\":\"" + json_escape(credentials.access) + "\",";
    json += "\"refresh\":\"" + json_escape(credentials.refresh) + "\",";
    json += "\"expires_ms\":" + std::to_string(credentials.expires_ms) + ",";
    json += "\"account_id\":\"" + json_escape(credentials.account_id) + "\"";
    json += "}";
    credential_save_named(provider, json);
}

void clear_credentials() {
    clear_credentials_provider("openai");
}

void clear_credentials_provider(const std::string& provider) {
    credential_delete_named(provider);
}

bool credentials_expired(const OAuthCredentials& credentials) {
    return now_ms() + 60000 >= credentials.expires_ms;
}

std::optional<std::string> load_api_key_provider(const std::string& provider) {
    auto raw = credential_load_named(provider);
    if (!raw) return std::nullopt;
    std::string key = json_string(*raw, "api_key").value_or("");
    return key.empty() ? std::nullopt : std::optional<std::string>(key);
}

void save_api_key_provider(const std::string& provider, const std::string& api_key) {
    std::string json = "{";
    json += "\"api_key\":\"" + json_escape(api_key) + "\"";
    json += "}";
    credential_save_named(provider, json);
}

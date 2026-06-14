#include "oauth.h"

#include "base64url.h"
#include "credential_store.h"
#include "http_client.h"
#include "json_util.h"
#include "platform.h"

#include <chrono>
#include <future>
#include <map>
#include <stdexcept>
#include <thread>

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

static std::string jwt_account_id(const std::string& access_token) {
    std::size_t first = access_token.find('.');
    if (first == std::string::npos) return "";
    std::size_t second = access_token.find('.', first + 1);
    if (second == std::string::npos) return "";
    auto payload = base64url_decode(access_token.substr(first + 1, second - first - 1));
    if (payload.empty()) return "";
    std::string decoded(payload.begin(), payload.end());
    return json_string(decoded, "chatgpt_account_id").value_or("");
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

OAuthCredentials oauth_login_browser() {
    return oauth_login_browser_provider("openai");
}

OAuthCredentials oauth_login_browser_provider(const std::string& provider) {
    if (provider == "glm") {
        throw std::runtime_error("GLM OAuth is not configured yet");
    }
    std::string verifier = base64url_encode(random_bytes(32));
    std::string challenge = base64url_encode(sha256_bytes(verifier));
    std::string state = provider == "anthropic" ? verifier : base64url_encode(random_bytes(16));
    std::string url = provider == "anthropic"
        ? create_anthropic_authorize_url(challenge, state)
        : create_authorize_url(challenge, state);

    auto code_future = std::async(std::launch::async, [provider, state] {
        if (provider == "anthropic") {
            return wait_for_oauth_code_on(53692, "/callback", state, "Claude");
        }
        return wait_for_oauth_code_on(1455, "/auth/callback", state, "ChatGPT");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    open_browser(url);
    std::string code = code_future.get();
    OAuthCredentials credentials = provider == "anthropic"
        ? exchange_anthropic_code(code, verifier)
        : exchange_code(code, verifier);
    save_credentials_provider(provider, credentials);
    return credentials;
}

OAuthCredentials oauth_refresh(const std::string& refresh_token) {
    return oauth_refresh_provider("openai", refresh_token);
}

OAuthCredentials oauth_refresh_provider(const std::string& provider, const std::string& refresh_token) {
    if (provider == "glm") {
        throw std::runtime_error("GLM OAuth is not configured yet");
    }
    if (provider == "anthropic") {
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
    if (provider == "glm") return std::nullopt;
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

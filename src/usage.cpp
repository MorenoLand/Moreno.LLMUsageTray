#include "usage.h"

#include "http_client.h"
#include "json_util.h"
#include "oauth.h"
#include "base64url.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

static std::time_t timegm_portable(std::tm* tm) {
#if defined(_WIN32)
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

static std::tm localtime_portable(std::time_t t) {
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

static constexpr const char* kWhamUrl = "https://chatgpt.com/backend-api/wham/usage";
static constexpr const char* kClaudeUsageUrl = "https://api.anthropic.com/api/oauth/usage";
static constexpr const char* kCodexResponsesUrl = "https://chatgpt.com/backend-api/codex/responses";
static constexpr const char* kClaudeMessagesUrl = "https://api.anthropic.com/v1/messages";
static constexpr const char* kGlmQuotaUrl = "https://api.z.ai/api/monitor/usage/quota/limit";
static constexpr const char* kGlmChatUrl = "https://api.z.ai/api/coding/paas/v4/chat/completions";

static long long parse_iso_or_epoch_reset(const std::string& value) {
    if (value.empty()) return 0;
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::stoll(value);
    }
    std::tm tm{};
    std::istringstream in(value.substr(0, 19));
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) return 0;
    return static_cast<long long>(timegm_portable(&tm));
}

static UsageInfo parse_usage(const std::string& body) {
    UsageInfo info;
    info.email = json_string(body, "email").value_or("");
    info.plan_type = json_string(body, "plan_type").value_or("");

    std::size_t ppos = body.find("\"primary_window\"");
    std::size_t spos = body.find("\"secondary_window\"");
    std::string primary = ppos == std::string::npos ? body : body.substr(ppos, spos == std::string::npos ? std::string::npos : spos - ppos);
    std::string secondary = spos == std::string::npos ? body : body.substr(spos);

    info.primary.used_percent = json_number(primary, "used_percent").value_or(0);
    info.primary.reset_at = static_cast<long long>(json_number(primary, "reset_at").value_or(0));
    info.secondary.used_percent = json_number(secondary, "used_percent").value_or(0);
    info.secondary.reset_at = static_cast<long long>(json_number(secondary, "reset_at").value_or(0));
    return info;
}

static std::string request_id() {
    static const char hex[] = "0123456789abcdef";
    auto bytes = random_bytes(16);
    std::string out;
    out.reserve(32);
    for (unsigned char b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

static std::string auth_header_value(const std::string& api_key) {
    std::string key = api_key;
    key.erase(key.begin(), std::find_if(key.begin(), key.end(), [](unsigned char c) { return !std::isspace(c); }));
    key.erase(std::find_if(key.rbegin(), key.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), key.end());
    std::string lower = key.substr(0, std::min<std::size_t>(7, key.size()));
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower == "bearer " ? key : "Bearer " + key;
}

static std::string object_for_type(const std::string& body, const std::string& type) {
    std::size_t type_pos = body.find("\"type\":\"" + type + "\"");
    if (type_pos == std::string::npos) type_pos = body.find("\"type\": \"" + type + "\"");
    if (type_pos == std::string::npos) return "";
    std::size_t start = body.rfind('{', type_pos);
    std::size_t next_type = body.find("\"type\"", type_pos + 6);
    std::size_t end = next_type == std::string::npos ? body.find(']', type_pos) : body.rfind('}', next_type);
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        end = body.find('}', type_pos);
    }
    if (start == std::string::npos || end == std::string::npos || end <= start) return "";
    return body.substr(start, end - start + 1);
}

static RateWindow parse_glm_window(const std::string& body, const std::string& type) {
    std::string obj = object_for_type(body, type);
    if (obj.empty()) return {};
    RateWindow window;
    window.used_percent = json_number(obj, "percentage").value_or(0);
    long long reset_ms = static_cast<long long>(json_number(obj, "nextResetTime").value_or(0));
    window.reset_at = reset_ms > 100000000000LL ? reset_ms / 1000 : reset_ms;
    return window;
}

UsageInfo fetch_usage_with_auth() {
    return fetch_usage_with_auth_provider("openai");
}

UsageInfo fetch_usage_with_auth_provider(const std::string& provider) {
    if (provider == "glm") {
        auto api_key = load_api_key_provider("glm");
        if (!api_key) {
            throw std::runtime_error("No GLM API key saved");
        }
        HttpResponse res = http_get(kGlmQuotaUrl, {
            {"Authorization", auth_header_value(*api_key)},
            {"Accept-Language", "en-US"},
            {"Content-Type", "application/json"},
        });
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("GLM quota request failed: HTTP " + std::to_string(res.status));
        }
        if (static_cast<int>(json_number(res.body, "code").value_or(0)) != 200) {
            throw std::runtime_error("GLM quota API did not return code 200");
        }
        UsageInfo info;
        info.email = "GLM API key";
        info.plan_type = "API";
        info.primary = parse_glm_window(res.body, "TOKENS_LIMIT");
        info.secondary = parse_glm_window(res.body, "TIME_LIMIT");
        return info;
    }
    auto credentials = load_credentials_provider(provider);
    if (!credentials) {
        throw std::runtime_error("Not logged in");
    }
    if (credentials_expired(*credentials)) {
        credentials = oauth_refresh_provider(provider, credentials->refresh);
    }

    if (provider == "anthropic") {
        HttpResponse res = http_get(kClaudeUsageUrl, {
            {"Authorization", "Bearer " + credentials->access},
            {"anthropic-beta", "oauth-2025-04-20"},
            {"User-Agent", "claude/1.0"},
        });
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Claude usage request failed: HTTP " + std::to_string(res.status));
        }
        UsageInfo info;
        info.email = "Claude";
        info.plan_type = "Claude";
        std::size_t fpos = res.body.find("\"five_hour\"");
        std::size_t spos = res.body.find("\"seven_day\"");
        std::string five = fpos == std::string::npos ? res.body : res.body.substr(fpos, spos == std::string::npos ? std::string::npos : spos - fpos);
        std::string seven = spos == std::string::npos ? res.body : res.body.substr(spos);
        info.primary.used_percent = json_number(five, "utilization").value_or(0);
        if (auto n = json_number(five, "resets_at")) info.primary.reset_at = static_cast<long long>(*n);
        else info.primary.reset_at = parse_iso_or_epoch_reset(json_string(five, "resets_at").value_or(""));
        info.secondary.used_percent = json_number(seven, "utilization").value_or(0);
        if (auto n = json_number(seven, "resets_at")) info.secondary.reset_at = static_cast<long long>(*n);
        else info.secondary.reset_at = parse_iso_or_epoch_reset(json_string(seven, "resets_at").value_or(""));
        return info;
    }

    HttpResponse res = http_get(kWhamUrl, {
        {"Authorization", "Bearer " + credentials->access},
    });
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Usage request failed: HTTP " + std::to_string(res.status));
    }
    return parse_usage(res.body);
}

void warm_provider(const std::string& provider) {
    if (provider == "glm") {
        auto api_key = load_api_key_provider("glm");
        if (!api_key) throw std::runtime_error("No GLM API key saved");
        std::string body = R"({"model":"glm-5","messages":[{"role":"user","content":"."}]})";
        HttpResponse res = http_post_json(kGlmChatUrl, body, {
            {"Authorization", auth_header_value(*api_key)},
            {"Accept-Language", "en-US"},
        });
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("GLM warm request failed: HTTP " + std::to_string(res.status));
        }
        return;
    }

    auto credentials = load_credentials_provider(provider);
    if (!credentials) throw std::runtime_error("Not logged in");
    if (credentials_expired(*credentials)) {
        credentials = oauth_refresh_provider(provider, credentials->refresh);
    }

    if (provider == "anthropic") {
        std::string body = "{";
        body += "\"model\":\"claude-sonnet-4-6\",";
        body += "\"max_tokens\":1,";
        body += "\"system\":[{\"type\":\"text\",\"text\":\"You are Claude Code, Anthropic's official CLI for Claude.\"}],";
        body += "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]";
        body += "}";
        HttpResponse res = http_post_json(kClaudeMessagesUrl, body, {
            {"Authorization", "Bearer " + credentials->access},
            {"anthropic-beta", "claude-code-20250219,oauth-2025-04-20"},
            {"User-Agent", "claude-cli/2.1.75"},
            {"x-app", "cli"},
        });
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Claude warm request failed: HTTP " + std::to_string(res.status));
        }
        return;
    }

    std::string id = request_id();
    std::string account_id = credentials->account_id;
    if (account_id.empty()) throw std::runtime_error("OpenAI account id missing; log in again");
    std::string body = "{";
    body += "\"model\":\"gpt-5.5\",";
    body += "\"store\":false,";
    body += "\"stream\":false,";
    body += "\"instructions\":\"You are a helpful assistant.\",";
    body += "\"input\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\".\"}]}],";
    body += "\"text\":{\"verbosity\":\"low\"}";
    body += "}";
    HttpResponse res = http_post_json(kCodexResponsesUrl, body, {
        {"Authorization", "Bearer " + credentials->access},
        {"chatgpt-account-id", account_id},
        {"originator", "pi"},
        {"User-Agent", "LLMUsageTray/" LLM_USAGE_TRAY_VERSION},
        {"OpenAI-Beta", "responses=experimental"},
        {"session-id", id},
        {"x-client-request-id", id},
    });
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("GPT warm request failed: HTTP " + std::to_string(res.status));
    }
}

std::string format_reset(long long reset_at_seconds) {
    if (reset_at_seconds <= 0) return "unknown";
    std::time_t t = static_cast<std::time_t>(reset_at_seconds);
    std::tm local = localtime_portable(t);
    std::ostringstream out;
    out << std::put_time(&local, "%I:%M%p %b %d");
    std::string s = out.str();
    if (!s.empty() && s[0] == '0') s.erase(s.begin());
    auto am = s.find("AM");
    if (am != std::string::npos) s.replace(am, 2, "a");
    auto pm = s.find("PM");
    if (pm != std::string::npos) s.replace(pm, 2, "p");
    return s;
}

std::string format_usage_line(const char* label, const RateWindow& window) {
    int left = static_cast<int>(std::max(0.0, std::min(100.0, 100.0 - window.used_percent)));
    std::ostringstream out;
    out << label << ": " << left << "% left, reset @ " << format_reset(window.reset_at);
    return out.str();
}

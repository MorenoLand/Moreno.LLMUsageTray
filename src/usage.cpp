#include "usage.h"

#include "http_client.h"
#include "diagnostics.h"
#include "json_util.h"
#include "oauth.h"
#include "base64url.h"
#include "platform.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

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
static constexpr const char* kGeminiQuotaUrl = "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota";
static constexpr const char* kGeminiQuotaSummaryUrl = "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary";
static constexpr const char* kGrokBillingUrl = "https://cli-chat-proxy.grok.com/v1/billing?format=credits";
static constexpr const char* kGrokSettingsUrl = "https://cli-chat-proxy.grok.com/v1/settings";
static constexpr const char* kGrokChatUrl = "https://cli-chat-proxy.grok.com/v1/chat/completions";

static long long parse_iso_or_epoch_reset(const std::string& value) {
    if (value.empty()) return 0;
    if (std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return std::stoll(value);
    }
    std::tm tm{};
    std::istringstream in(value.substr(0, 19));
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (!in.fail()) return static_cast<long long>(timegm_portable(&tm));
    tm = {};
    std::istringstream local_in(value.substr(0, 19));
    local_in >> std::get_time(&tm, "%m/%d/%Y %H:%M:%S");
    return local_in.fail() ? 0 : static_cast<long long>(std::mktime(&tm));
}

static std::string object_for_key(const std::string& body, const std::string& key) {
    std::size_t key_pos = body.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return "";
    std::size_t start = body.find(':', key_pos + key.size() + 2);
    if (start == std::string::npos) return "";
    start = body.find_first_not_of(" \t\r\n", start + 1);
    if (start == std::string::npos || body[start] != '{') return "";
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = start; i < body.size(); ++i) {
        char c = body[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) return body.substr(start, i - start + 1);
    }
    return "";
}

static std::string array_for_key(const std::string& body, const std::string& key) {
    std::size_t key_pos = body.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return "";
    std::size_t start = body.find(':', key_pos + key.size() + 2);
    if (start == std::string::npos) return "";
    start = body.find_first_not_of(" \t\r\n", start + 1);
    if (start == std::string::npos || body[start] != '[') return "";
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (std::size_t i = start; i < body.size(); ++i) {
        char c = body[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[') ++depth;
        else if (c == ']' && --depth == 0) return body.substr(start, i - start + 1);
    }
    return "";
}

static std::vector<std::string> objects_in_array(const std::string& array) {
    std::vector<std::string> objects;
    for (std::size_t i = 0; i < array.size(); ++i) {
        if (array[i] != '{') continue;
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (std::size_t end = i; end < array.size(); ++end) {
            char c = array[end];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                objects.push_back(array.substr(i, end - i + 1));
                i = end;
                break;
            }
        }
    }
    return objects;
}

static std::vector<std::pair<std::size_t, std::string>> objects_for_key(const std::string& body, const std::string& key) {
    std::vector<std::pair<std::size_t, std::string>> objects;
    std::size_t search = 0;
    while (search < body.size()) {
        std::size_t key_pos = body.find("\"" + key + "\"", search);
        if (key_pos == std::string::npos) break;
        std::size_t start = body.find(':', key_pos + key.size() + 2);
        if (start == std::string::npos) break;
        start = body.find_first_not_of(" \t\r\n", start + 1);
        if (start == std::string::npos || body[start] != '{') {
            search = key_pos + key.size() + 2;
            continue;
        }
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        bool complete = false;
        for (std::size_t end = start; end < body.size(); ++end) {
            char c = body[end];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                objects.emplace_back(key_pos, body.substr(start, end - start + 1));
                search = end + 1;
                complete = true;
                break;
            }
        }
        if (!complete) break;
    }
    return objects;
}

static RateWindow parse_openai_window(const std::string& body, const std::string& key) {
    std::string object = object_for_key(body, key);
    if (object.empty()) return {};
    RateWindow window;
    window.available = true;
    window.used_percent = json_number(object, "used_percent").value_or(0);
    if (auto reset_at = json_number(object, "reset_at")) window.reset_at = static_cast<long long>(*reset_at);
    else window.reset_at = parse_iso_or_epoch_reset(json_string(object, "resets_at").value_or(""));
    if (auto limit_window_seconds = json_number(object, "limit_window_seconds")) window.limit_window_seconds = static_cast<long long>(*limit_window_seconds);
    else window.limit_window_seconds = static_cast<long long>(json_number(object, "window_minutes").value_or(0) * 60.0);
    return window;
}

static UsageInfo parse_usage(const std::string& body) {
    UsageInfo info;
    info.email = json_string(body, "email").value_or("");
    info.plan_type = json_string(body, "plan_type").value_or("");
    std::string rate_limit = object_for_key(body, "rate_limit");
    info.primary = parse_openai_window(rate_limit, "primary_window");
    if (!info.primary.available) info.primary = parse_openai_window(rate_limit, "primary");
    info.secondary = parse_openai_window(rate_limit, "secondary_window");
    if (!info.secondary.available) info.secondary = parse_openai_window(rate_limit, "secondary");
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

static RateWindow parse_glm_window(const std::string& body, const std::string& type, int unit = 0) {
    std::size_t pos = 0;
    while ((pos = body.find("\"type\":\"" + type + "\"", pos)) != std::string::npos) {
        std::size_t start = body.rfind('{', pos);
        std::size_t end = body.find('}', pos);
        if (start == std::string::npos || end == std::string::npos) break;
        std::string obj = body.substr(start, end - start + 1);
        pos = end + 1;
        if (unit && static_cast<int>(json_number(obj, "unit").value_or(0)) != unit) continue;
        RateWindow window;
        window.available = true;
        window.used_percent = json_number(obj, "percentage").value_or(0);
        long long reset_ms = static_cast<long long>(json_number(obj, "nextResetTime").value_or(0));
        window.reset_at = reset_ms > 100000000000LL ? reset_ms / 1000 : reset_ms;
        return window;
    }
    return {};
}

static RateWindow parse_gemini_window(const std::string& object) {
    std::optional<double> remaining = json_number(object, "remainingFraction");
    if (!remaining) remaining = json_number(object_for_key(object, "remaining"), "remainingFraction");
    if (!remaining) {
        std::string value = json_string(object, "remainingFraction").value_or("");
        if (value.empty()) value = json_string(object_for_key(object, "remaining"), "remainingFraction").value_or("");
        if (!value.empty()) {
            char* end = nullptr;
            double parsed = std::strtod(value.c_str(), &end);
            if (end != value.c_str()) remaining = parsed;
        }
    }
    if (!remaining) return {};
    RateWindow window;
    window.available = true;
    window.used_percent = (1.0 - std::clamp(*remaining, 0.0, 1.0)) * 100.0;
    if (auto reset_at = json_number(object, "resetTime")) window.reset_at = static_cast<long long>(*reset_at);
    else window.reset_at = parse_iso_or_epoch_reset(json_string(object, "resetTime").value_or(""));
    return window;
}

static void add_gemini_window(UsageInfo& info, const std::string& model, const RateWindow& window) {
    if (!window.available) return;
    std::string lower = model;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool pro = lower.find("pro") != std::string::npos;
    bool flash = lower.find("flash") != std::string::npos;
    if ((pro || (!flash && !info.primary.available)) && !info.primary.available) info.primary = window;
    else if ((flash || !info.secondary.available) && !info.secondary.available) info.secondary = window;
}

static std::string gemini_model_name_before(const std::string& body, std::size_t key_pos) {
    std::size_t parent_start = body.rfind('{', key_pos);
    if (parent_start == std::string::npos || parent_start == 0) return "";
    std::size_t quote_end = body.rfind('"', parent_start - 1);
    if (quote_end == std::string::npos || quote_end == 0) return "";
    std::size_t quote_start = body.rfind('"', quote_end - 1);
    if (quote_start == std::string::npos || quote_start >= quote_end) return "";
    return body.substr(quote_start + 1, quote_end - quote_start - 1);
}

static UsageInfo parse_gemini_usage(const std::string& body) {
    UsageInfo info;
    info.plan_type = "Gemini";
    for (const std::string& object : objects_in_array(array_for_key(body, "buckets"))) {
        add_gemini_window(info, json_string(object, "modelId").value_or(""), parse_gemini_window(object));
    }
    for (const auto& match : objects_for_key(body, "quotaInfo")) {
        add_gemini_window(info, gemini_model_name_before(body, match.first), parse_gemini_window(match.second));
    }
    if (!info.primary.available && !info.secondary.available) throw std::runtime_error("Gemini quota response contained no model windows");
    return info;
}

static UsageInfo parse_gemini_summary(const std::string& body) {
    UsageInfo info;
    info.plan_type = "Gemini";
    std::vector<std::string> groups = objects_in_array(array_for_key(body, "groups"));
    for (const std::string& group : groups) {
        std::string group_name = json_string(group, "displayName").value_or("");
        std::string lower_group = group_name;
        std::transform(lower_group.begin(), lower_group.end(), lower_group.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool group_is_gemini = lower_group.find("gemini") != std::string::npos;
        diagnostics_log("gemini summary group=" + (group_name.empty() ? std::string("<unnamed>") : group_name));
        for (const std::string& bucket : objects_in_array(array_for_key(group, "buckets"))) {
            RateWindow window = parse_gemini_window(bucket);
            if (!window.available) continue;
            std::string bucket_id = json_string(bucket, "bucketId").value_or("");
            std::string kind = json_string(bucket, "window").value_or("") + " " + json_string(bucket, "displayName").value_or("") + " " + bucket_id;
            std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            bool bucket_is_gemini = kind.find("gemini") != std::string::npos;
            diagnostics_log("gemini summary bucket=" + (bucket_id.empty() ? std::string("<unnamed>") : bucket_id) + " kind=" + kind + " selected=" + std::string(group_is_gemini || bucket_is_gemini ? "true" : "false"));
            if (!group_is_gemini && !bucket_is_gemini) continue;
            if (kind.find("pro") != std::string::npos) {
                if (!info.primary.available) info.primary = window;
            } else if (kind.find("flash") != std::string::npos) {
                if (!info.secondary.available) info.secondary = window;
            } else if ((kind.find("5") != std::string::npos || kind.find("hour") != std::string::npos) && !info.primary.available) {
                window.limit_window_seconds = 5 * 60 * 60;
                info.primary = window;
            } else if ((kind.find("week") != std::string::npos || kind.find("seven") != std::string::npos || kind.find("day") != std::string::npos) && !info.secondary.available) {
                window.limit_window_seconds = 7 * 24 * 60 * 60;
                info.secondary = window;
            } else {
                add_gemini_window(info, kind, window);
            }
        }
    }
    diagnostics_log("gemini summary parsed primary=" + std::string(info.primary.available ? "available" : "missing") + " secondary=" + std::string(info.secondary.available ? "available" : "missing") + " primary_used=" + std::to_string(info.primary.used_percent) + " secondary_used=" + std::to_string(info.secondary.used_percent) + " primary_reset=" + std::to_string(info.primary.reset_at) + " secondary_reset=" + std::to_string(info.secondary.reset_at));
    if (!info.primary.available && !info.secondary.available) throw std::runtime_error("Gemini quota summary contained no model windows");
    return info;
}

static std::string gemini_platform() {
#if defined(_WIN32)
    return "WINDOWS_AMD64";
#elif defined(__APPLE__)
    return "DARWIN_AMD64";
#else
    return "LINUX_AMD64";
#endif
}

static std::map<std::string, std::string> grok_headers(const std::string& access) {
    return {
        {"Authorization", "Bearer " + access},
        {"X-XAI-Token-Auth", "xai-grok-cli"},
        {"Accept", "application/json"},
    };
}

static std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static bool grok_product_is_cli(const std::string& product) {
    std::string lower = lower_ascii(product);
    return lower.find("build") != std::string::npos || lower == "cli";
}

static bool grok_product_is_bot(const std::string& product) {
    std::string lower = lower_ascii(product);
    return lower.find("chat") != std::string::npos || lower.find("bot") != std::string::npos;
}

static RateWindow grok_window(double used_percent, long long reset_at, long long window_seconds) {
    RateWindow window;
    window.available = true;
    window.used_percent = used_percent;
    window.reset_at = reset_at;
    window.limit_window_seconds = window_seconds;
    return window;
}

static std::string grok_array(const std::string& config, const std::string& body, const char* key) {
    std::string value = array_for_key(config, key);
    if (value.empty()) value = array_for_key(body, key);
    return value;
}

static UsageInfo parse_grok_usage(const std::string& body, const std::string& plan, const std::string& email) {
    std::string config = object_for_key(body, "config");
    if (config.empty()) config = body;
    std::string period = object_for_key(config, "currentPeriod");
    if (period.empty()) period = object_for_key(body, "currentPeriod");
    std::string end = json_string(period, "end").value_or(json_string(config, "billingPeriodEnd").value_or(json_string(body, "billingPeriodEnd").value_or("")));
    long long reset_at = parse_iso_or_epoch_reset(end);
    std::string period_type = json_string(period, "type").value_or(json_string(body, "period").value_or(""));
    long long window_seconds = period_type.find("MONTH") != std::string::npos ? 30LL * 24 * 60 * 60 : 7LL * 24 * 60 * 60;
    bool has_period = !period.empty() || !end.empty();
    double overall = json_number(config, "creditUsagePercent").value_or(json_number(body, "creditUsagePercent").value_or(has_period ? 0.0 : -1.0));
    std::string products = grok_array(config, body, "productUsage");
    if (products.empty()) products = grok_array(config, body, "products");
    bool cli_found = false;
    bool bot_found = false;
    double cli_used = 0;
    double bot_used = 0;
    for (const std::string& object : objects_in_array(products)) {
        std::string product = json_string(object, "product").value_or("");
        double used = json_number(object, "usagePercent").value_or(json_number(object, "usage_percent").value_or(0));
        if (grok_product_is_cli(product)) {
            cli_found = true;
            cli_used = used;
        } else if (grok_product_is_bot(product)) {
            bot_found = true;
            bot_used = used;
        }
    }
    if (!cli_found && overall < 0) {
        auto used = json_number(object_for_key(config, "used"), "val");
        auto limit = json_number(object_for_key(config, "monthlyLimit"), "val");
        if (used && limit && *limit > 0) overall = (*used / *limit) * 100.0;
    }
    if (!has_period && overall < 0 && products.empty()) throw std::runtime_error("Grok billing response contained no usage windows");
    UsageInfo info;
    info.email = email;
    info.plan_type = plan.empty() ? "Grok" : plan;
    info.primary = grok_window(cli_found ? cli_used : (overall >= 0 ? overall : 0), reset_at, window_seconds);
    info.secondary = grok_window(bot_found ? bot_used : 0, reset_at, window_seconds);
    return info;
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
        info.primary = parse_glm_window(res.body, "TOKENS_LIMIT", 3);
        info.secondary = parse_glm_window(res.body, "TOKENS_LIMIT", 6);
        info.tertiary = parse_glm_window(res.body, "TIME_LIMIT");
        return info;
    }
    auto credentials = load_credentials_provider(provider);
    if (!credentials) {
        throw std::runtime_error("Not logged in");
    }
    if (provider == "gemini") {
        if (auto local = fetch_agy_local_quota_summary()) {
            diagnostics_log_raw("agy local quota summary raw_body", *local);
            return parse_gemini_summary(*local);
        }
        diagnostics_log("agy local quota summary unavailable; using remote Cloud Code");
    }
    if (credentials_expired(*credentials)) {
        credentials = oauth_refresh_provider(provider, credentials->refresh);
    }

    if (provider == "grok") {
        auto headers = grok_headers(credentials->access);
        HttpResponse res = http_get(kGrokBillingUrl, headers);
        diagnostics_log("grok billing status=" + std::to_string(res.status) + " body_length=" + std::to_string(res.body.size()));
        diagnostics_log_raw("grok billing raw_body", res.body);
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Grok billing request failed: HTTP " + std::to_string(res.status));
        }
        std::string plan = "Grok";
        HttpResponse settings = http_get(kGrokSettingsUrl, headers);
        diagnostics_log("grok settings status=" + std::to_string(settings.status) + " body_length=" + std::to_string(settings.body.size()));
        if (settings.status >= 200 && settings.status < 300) {
            plan = json_string(settings.body, "subscription_tier_display").value_or(json_string(settings.body, "subscription_tier").value_or(plan));
        }
        return parse_grok_usage(res.body, plan, credentials->account_id);
    }

    if (provider == "gemini") {
        if (credentials->account_id.empty()) {
            credentials->account_id = gemini_discover_project(credentials->access);
            save_credentials_provider(provider, *credentials);
        }
        std::string body = "{\"project\":\"" + json_escape(credentials->account_id) + "\"}";
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + credentials->access},
            {"User-Agent", "antigravity/cli/1.1.24 windows/amd64"},
            {"Client-Metadata", "{\"ideType\":\"ANTIGRAVITY\",\"platform\":\"" + gemini_platform() + "\",\"pluginType\":\"GEMINI\"}"},
        };
        HttpResponse summary = http_post_json(kGeminiQuotaSummaryUrl, body, headers);
        diagnostics_log("gemini quota summary status=" + std::to_string(summary.status) + " body_length=" + std::to_string(summary.body.size()));
        diagnostics_log_raw("gemini quota summary raw_body", summary.body);
        if (summary.status >= 200 && summary.status < 300) {
            try {
                return parse_gemini_summary(summary.body);
            } catch (const std::exception& error) {
                diagnostics_log("gemini quota summary parse_error=" + std::string(error.what()));
            }
        }
        HttpResponse res = http_post_json(kGeminiQuotaUrl, body, headers);
        diagnostics_log("gemini quota legacy status=" + std::to_string(res.status) + " body_length=" + std::to_string(res.body.size()));
        diagnostics_log_raw("gemini quota legacy raw_body", res.body);
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Gemini quota request failed: HTTP " + std::to_string(res.status));
        }
        return parse_gemini_usage(res.body);
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
        info.primary.available = fpos != std::string::npos;
        info.primary.used_percent = json_number(five, "utilization").value_or(0);
        if (auto n = json_number(five, "resets_at")) info.primary.reset_at = static_cast<long long>(*n);
        else info.primary.reset_at = parse_iso_or_epoch_reset(json_string(five, "resets_at").value_or(""));
        info.secondary.available = spos != std::string::npos;
        info.secondary.used_percent = json_number(seven, "utilization").value_or(0);
        if (auto n = json_number(seven, "resets_at")) info.secondary.reset_at = static_cast<long long>(*n);
        else info.secondary.reset_at = parse_iso_or_epoch_reset(json_string(seven, "resets_at").value_or(""));
        return info;
    }

    std::map<std::string, std::string> headers = {
        {"Authorization", "Bearer " + credentials->access},
    };
    if (!credentials->account_id.empty()) headers["ChatGPT-Account-Id"] = credentials->account_id;
    HttpResponse res = http_get(kWhamUrl, headers);
    if (res.status < 200 || res.status >= 300) {
        throw std::runtime_error("Usage request failed: HTTP " + std::to_string(res.status));
    }
    UsageInfo info = parse_usage(res.body);
    return info;
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

    if (provider == "grok") {
        std::string body = R"({"model":"grok-build-0.1","max_tokens":1,"messages":[{"role":"user","content":"."}]})";
        HttpResponse res = http_post_json(kGrokChatUrl, body, grok_headers(credentials->access));
        if (res.status < 200 || res.status >= 300) {
            throw std::runtime_error("Grok warm request failed: HTTP " + std::to_string(res.status));
        }
        return;
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

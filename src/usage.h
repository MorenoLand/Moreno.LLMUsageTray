#pragma once

#include <optional>
#include <string>

struct RateWindow {
    bool available = false;
    double used_percent = 0;
    long long reset_at = 0;
    long long limit_window_seconds = 0;
};

struct UsageInfo {
    std::string email;
    std::string plan_type;
    RateWindow primary;
    RateWindow secondary;
    RateWindow tertiary;
    long long local_codex_tokens_today = 0;
};

UsageInfo fetch_usage_with_auth();
UsageInfo fetch_usage_with_auth_provider(const std::string& provider);
void warm_provider(const std::string& provider);
std::string format_usage_line(const char* label, const RateWindow& window);
std::string format_reset(long long reset_at_seconds);

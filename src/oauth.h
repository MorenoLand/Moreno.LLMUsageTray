#pragma once

#include <optional>
#include <string>

struct OAuthCredentials {
    std::string access;
    std::string refresh;
    long long expires_ms = 0;
    std::string account_id;
};

OAuthCredentials oauth_login_browser();
OAuthCredentials oauth_refresh(const std::string& refresh_token);
std::optional<OAuthCredentials> load_credentials();
void save_credentials(const OAuthCredentials& credentials);
void clear_credentials();
bool credentials_expired(const OAuthCredentials& credentials);

OAuthCredentials oauth_login_browser_provider(const std::string& provider);
OAuthCredentials oauth_refresh_provider(const std::string& provider, const std::string& refresh_token);
std::optional<OAuthCredentials> load_credentials_provider(const std::string& provider);
void save_credentials_provider(const std::string& provider, const OAuthCredentials& credentials);
void clear_credentials_provider(const std::string& provider);

std::optional<std::string> load_api_key_provider(const std::string& provider);
void save_api_key_provider(const std::string& provider, const std::string& api_key);

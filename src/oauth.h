#pragma once

#include <optional>
#include <string>

struct OAuthCredentials {
    std::string access;
    std::string refresh;
    long long expires_ms = 0;
    std::string account_id;
};

struct OAuthLoginSession {
    std::string provider;
    std::string verifier;
    std::string state;
    std::string authorize_url;
    std::string client_id;
    std::string client_secret;
};

OAuthCredentials oauth_login_browser();
OAuthCredentials oauth_refresh(const std::string& refresh_token);
std::optional<OAuthCredentials> load_credentials();
void save_credentials(const OAuthCredentials& credentials);
void clear_credentials();
bool credentials_expired(const OAuthCredentials& credentials);

OAuthCredentials oauth_login_browser_provider(const std::string& provider);
OAuthLoginSession oauth_begin_manual_login_provider(const std::string& provider);
OAuthCredentials oauth_finish_manual_login_provider(const OAuthLoginSession& session, const std::string& code);
OAuthCredentials oauth_refresh_provider(const std::string& provider, const std::string& refresh_token);
std::string gemini_discover_project(const std::string& access_token);
std::optional<OAuthCredentials> load_credentials_provider(const std::string& provider);
void save_credentials_provider(const std::string& provider, const OAuthCredentials& credentials);
void clear_credentials_provider(const std::string& provider);

std::optional<std::string> load_api_key_provider(const std::string& provider);
void save_api_key_provider(const std::string& provider, const std::string& api_key);

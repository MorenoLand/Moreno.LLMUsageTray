#include "http_client.h"

#include <curl/curl.h>

#include <sstream>
#include <stdexcept>

namespace {

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpResponse request(const std::string& method, const std::string& url, const std::map<std::string, std::string>& headers, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LLMUsageTray/" LLM_USAGE_TRAY_VERSION);

    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) throw std::runtime_error(std::string("curl request failed: ") + curl_easy_strerror(code));

    return {static_cast<int>(status), response_body};
}

std::string form_encode(const std::map<std::string, std::string>& fields) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    std::ostringstream out;
    bool first = true;
    for (const auto& [key, value] : fields) {
        char* escaped_key = curl_easy_escape(curl, key.c_str(), static_cast<int>(key.size()));
        char* escaped_value = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.size()));
        if (!first) out << '&';
        first = false;
        out << (escaped_key ? escaped_key : "") << '=' << (escaped_value ? escaped_value : "");
        if (escaped_key) curl_free(escaped_key);
        if (escaped_value) curl_free(escaped_value);
    }
    curl_easy_cleanup(curl);
    return out.str();
}

} // namespace

HttpResponse http_get(const std::string& url, const std::map<std::string, std::string>& headers) {
    return request("GET", url, headers, "");
}

HttpResponse http_post_form(const std::string& url, const std::map<std::string, std::string>& fields) {
    return request("POST", url, {{"Content-Type", "application/x-www-form-urlencoded"}}, form_encode(fields));
}

HttpResponse http_post_json(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
    auto merged = headers;
    merged.emplace("Content-Type", "application/json");
    return request("POST", url, merged, body);
}

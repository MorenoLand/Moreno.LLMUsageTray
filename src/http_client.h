#pragma once

#include <map>
#include <string>

struct HttpResponse {
    int status = 0;
    std::string body;
};

HttpResponse http_get(const std::string& url, const std::map<std::string, std::string>& headers = {});
HttpResponse http_post_form(const std::string& url, const std::map<std::string, std::string>& fields);
HttpResponse http_post_json(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers = {});

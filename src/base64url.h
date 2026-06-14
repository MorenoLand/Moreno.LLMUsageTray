#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::string base64url_encode(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> base64url_decode(const std::string& text);
std::vector<std::uint8_t> random_bytes(std::size_t count);
std::vector<std::uint8_t> sha256_bytes(const std::string& text);

#include "base64url.h"

#include <array>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace {

constexpr std::array<std::uint32_t, 64> kSha256Round = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

std::uint32_t rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

#if !defined(_WIN32)
void fill_random_from_device(std::vector<std::uint8_t>& bytes) {
    std::size_t done = 0;
#if defined(__linux__)
    while (done < bytes.size()) {
        ssize_t n = getrandom(bytes.data() + done, bytes.size() - done, 0);
        if (n > 0) done += static_cast<std::size_t>(n);
        else break;
    }
    if (done == bytes.size()) return;
#endif
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) throw std::runtime_error("Could not open /dev/urandom");
    while (done < bytes.size()) {
        ssize_t n = read(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("Could not read random bytes");
        }
        done += static_cast<std::size_t>(n);
    }
    close(fd);
}
#endif

} // namespace

std::string base64url_encode(const std::vector<std::uint8_t>& bytes) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    std::uint32_t val = 0;
    int valb = -6;
    for (std::uint8_t c : bytes) {
        val = (val << 8) | c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(table[(val >> valb) & 0x3f]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(table[((val << 8) >> (valb + 8)) & 0x3f]);
    return out;
}

std::vector<std::uint8_t> base64url_decode(const std::string& text) {
    static const std::string table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string in = text;
    for (char& c : in) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (in.size() % 4) in.push_back('=');

    std::vector<std::uint8_t> out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        auto pos = table.find(static_cast<char>(c));
        if (pos == std::string::npos) return {};
        val = (val << 6) | static_cast<int>(pos);
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

std::vector<std::uint8_t> random_bytes(std::size_t count) {
    std::vector<std::uint8_t> bytes(count);
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
#else
    fill_random_from_device(bytes);
#endif
    return bytes;
}

std::vector<std::uint8_t> sha256_bytes(const std::string& text) {
    std::vector<std::uint8_t> data(text.begin(), text.end());
    std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xff));

    std::uint32_t h0 = 0x6a09e667;
    std::uint32_t h1 = 0xbb67ae85;
    std::uint32_t h2 = 0x3c6ef372;
    std::uint32_t h3 = 0xa54ff53a;
    std::uint32_t h4 = 0x510e527f;
    std::uint32_t h5 = 0x9b05688c;
    std::uint32_t h6 = 0x1f83d9ab;
    std::uint32_t h7 = 0x5be0cd19;

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::uint32_t w[64]{};
        for (int i = 0; i < 16; ++i) {
            std::size_t j = offset + i * 4;
            w[i] = (static_cast<std::uint32_t>(data[j]) << 24) |
                (static_cast<std::uint32_t>(data[j + 1]) << 16) |
                (static_cast<std::uint32_t>(data[j + 2]) << 8) |
                static_cast<std::uint32_t>(data[j + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t temp1 = h + s1 + ch + kSha256Round[i] + w[i];
            std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e; h5 += f; h6 += g; h7 += h;
    }

    std::vector<std::uint8_t> digest(32);
    std::uint32_t parts[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(parts[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(parts[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(parts[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(parts[i]);
    }
    return digest;
}

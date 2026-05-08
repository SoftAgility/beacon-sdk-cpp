#include "Base64.hpp"

#include <cstdint>
#include <string>

namespace beacon {
namespace internal {

namespace {

const char BASE64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint8_t decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
}

} // anonymous namespace

std::string base64_encode(const std::string& input) {
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    const auto* data = reinterpret_cast<const uint8_t*>(input.data());
    size_t len = input.size();

    while (i < len) {
        size_t start_i = i;
        uint32_t octet_a = (i < len) ? data[i++] : 0;
        uint32_t octet_b = (i < len) ? data[i++] : 0;
        uint32_t octet_c = (i < len) ? data[i++] : 0;
        size_t bytes_read = i - start_i;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        output += BASE64_CHARS[(triple >> 18) & 0x3F];
        output += BASE64_CHARS[(triple >> 12) & 0x3F];
        output += bytes_read > 1 ? BASE64_CHARS[(triple >> 6) & 0x3F] : '=';
        output += bytes_read > 2 ? BASE64_CHARS[triple & 0x3F] : '=';
    }

    return output;
}

std::string base64_decode(const std::string& input) {
    std::string output;
    if (input.empty()) return output;

    output.reserve((input.size() / 4) * 3);

    for (size_t i = 0; i < input.size(); i += 4) {
        uint8_t a = decode_char(input[i]);
        uint8_t b = (i + 1 < input.size()) ? decode_char(input[i + 1]) : 0;
        uint8_t c = (i + 2 < input.size()) ? decode_char(input[i + 2]) : 0;
        uint8_t d = (i + 3 < input.size()) ? decode_char(input[i + 3]) : 0;

        uint32_t triple = (static_cast<uint32_t>(a) << 18)
                        | (static_cast<uint32_t>(b) << 12)
                        | (static_cast<uint32_t>(c) << 6)
                        | static_cast<uint32_t>(d);

        output += static_cast<char>((triple >> 16) & 0xFF);
        if (i + 2 < input.size() && input[i + 2] != '=')
            output += static_cast<char>((triple >> 8) & 0xFF);
        if (i + 3 < input.size() && input[i + 3] != '=')
            output += static_cast<char>(triple & 0xFF);
    }

    return output;
}

} // namespace internal
} // namespace beacon

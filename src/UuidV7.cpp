#include "UuidV7.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

namespace beacon {
namespace internal {

namespace {

std::mutex g_rng_mutex;
bool g_rng_initialized = false;
std::mt19937_64 g_rng;

void ensure_rng_initialized() {
    if (!g_rng_initialized) {
        std::random_device rd;
        g_rng.seed(rd());
        g_rng_initialized = true;
    }
}

} // anonymous namespace

std::string new_uuid_v7() {
    // Get milliseconds since Unix epoch
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    uint64_t timestamp = static_cast<uint64_t>(ms);

    uint64_t rand_a, rand_b;
    {
        std::lock_guard<std::mutex> lock(g_rng_mutex);
        ensure_rng_initialized();
        rand_a = g_rng();
        rand_b = g_rng();
    }

    // UUID v7 layout:
    // Bytes 0-5 (48 bits): Unix timestamp in milliseconds
    // Bytes 6-7 (16 bits): version (4 bits = 0111) + 12 random bits
    // Bytes 8-9 (16 bits): variant (2 bits = 10) + 14 random bits
    // Bytes 10-15 (48 bits): random

    uint8_t uuid[16] = {0};

    // Timestamp: big-endian 48-bit ms
    uuid[0] = static_cast<uint8_t>((timestamp >> 40) & 0xFF);
    uuid[1] = static_cast<uint8_t>((timestamp >> 32) & 0xFF);
    uuid[2] = static_cast<uint8_t>((timestamp >> 24) & 0xFF);
    uuid[3] = static_cast<uint8_t>((timestamp >> 16) & 0xFF);
    uuid[4] = static_cast<uint8_t>((timestamp >> 8) & 0xFF);
    uuid[5] = static_cast<uint8_t>(timestamp & 0xFF);

    // Version 7 + 12 random bits
    uuid[6] = static_cast<uint8_t>(0x70 | ((rand_a >> 8) & 0x0F));
    uuid[7] = static_cast<uint8_t>(rand_a & 0xFF);

    // Variant 10 + 62 random bits
    uuid[8] = static_cast<uint8_t>(0x80 | ((rand_a >> 16) & 0x3F));
    uuid[9] = static_cast<uint8_t>((rand_a >> 24) & 0xFF);
    uuid[10] = static_cast<uint8_t>((rand_b >> 0) & 0xFF);
    uuid[11] = static_cast<uint8_t>((rand_b >> 8) & 0xFF);
    uuid[12] = static_cast<uint8_t>((rand_b >> 16) & 0xFF);
    uuid[13] = static_cast<uint8_t>((rand_b >> 24) & 0xFF);
    uuid[14] = static_cast<uint8_t>((rand_b >> 32) & 0xFF);
    uuid[15] = static_cast<uint8_t>((rand_b >> 40) & 0xFF);

    // Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5],
        uuid[6], uuid[7],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);

    return std::string(buf, 36);
}

} // namespace internal
} // namespace beacon

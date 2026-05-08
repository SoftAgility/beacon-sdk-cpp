#include "EnvironmentCollector.hpp"

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <bcrypt.h>
    #pragma comment(lib, "bcrypt.lib")
#else
    #include <openssl/sha.h>
    #include <sys/utsname.h>
    #include <unistd.h>
#endif

#if defined(__linux__)
    #include <sys/sysinfo.h>
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
    #include <sys/types.h>
#endif

#include <clocale>
#include <nlohmann/json.hpp>

namespace beacon {
namespace internal {

std::string ram_bucket(uint64_t total_ram_mb) {
    if (total_ram_mb < 2048) return "< 2 GB";
    if (total_ram_mb < 4096) return "2-4 GB";
    if (total_ram_mb < 8192) return "4-8 GB";
    if (total_ram_mb < 16384) return "8-16 GB";
    if (total_ram_mb < 32768) return "16-32 GB";
    return "> 32 GB";
}

std::string sha256_hex(const std::string& input) {
    unsigned char hash[32] = {};

#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash_handle = nullptr;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) return {};

    status = BCryptCreateHash(alg, &hash_handle, nullptr, 0, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    status = BCryptHashData(hash_handle,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hash_handle);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    status = BCryptFinishHash(hash_handle, hash, sizeof(hash), 0);
    BCryptDestroyHash(hash_handle);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (!BCRYPT_SUCCESS(status)) return {};
#else
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), hash);
#endif

    char hex[65] = {};
    for (int i = 0; i < 32; ++i) {
        std::snprintf(hex + (i * 2), 3, "%02x", hash[i]);
    }
    return std::string(hex, 64);
}

std::string collect_environment_json() {
    nlohmann::json env;

    try {
#if defined(_WIN32)
        // OS name
        env["os_name"] = "Windows";

        // OS version via RtlGetVersion
        {
            using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            auto ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                auto fn = reinterpret_cast<RtlGetVersionFn>(
                    GetProcAddress(ntdll, "RtlGetVersion"));
                if (fn) {
                    RTL_OSVERSIONINFOW vi = {};
                    vi.dwOSVersionInfoSize = sizeof(vi);
                    if (fn(&vi) == 0) {
                        env["os_version"] = std::to_string(vi.dwMajorVersion) + "." +
                                            std::to_string(vi.dwMinorVersion) + "." +
                                            std::to_string(vi.dwBuildNumber);
                    }
                }
            }
        }

        // OS architecture
        {
            SYSTEM_INFO si = {};
            GetNativeSystemInfo(&si);
            switch (si.wProcessorArchitecture) {
                case PROCESSOR_ARCHITECTURE_AMD64: env["os_architecture"] = "x86_64"; break;
                case PROCESSOR_ARCHITECTURE_ARM64: env["os_architecture"] = "ARM64"; break;
                case PROCESSOR_ARCHITECTURE_INTEL: env["os_architecture"] = "x86"; break;
                default: env["os_architecture"] = "unknown"; break;
            }

            // CPU core count
            env["cpu_core_count"] = static_cast<int>(si.dwNumberOfProcessors);
        }

        // Total RAM
        {
            MEMORYSTATUSEX mem = {};
            mem.dwLength = sizeof(mem);
            if (GlobalMemoryStatusEx(&mem)) {
                uint64_t total_mb = mem.ullTotalPhys / (1024 * 1024);
                env["total_ram_mb_bucket"] = ram_bucket(total_mb);
            }
        }

        // Display (Windows only)
        {
            int w = GetSystemMetrics(SM_CXSCREEN);
            int h = GetSystemMetrics(SM_CYSCREEN);
            if (w > 0 && h > 0) {
                env["display_width"] = w;
                env["display_height"] = h;
            }
        }

        // Machine name hash
        {
            char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD size = sizeof(name);
            if (GetComputerNameA(name, &size)) {
                std::string hash = sha256_hex(std::string(name, size));
                if (!hash.empty()) {
                    env["machine_name_hash"] = hash;
                }
            }
        }

        // Locale
        {
            wchar_t locale_name[LOCALE_NAME_MAX_LENGTH] = {};
            if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
                // Convert wchar_t to string
                char narrow[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, locale_name, -1, narrow, sizeof(narrow), nullptr, nullptr);
                env["locale"] = std::string(narrow);
            }
        }

#elif defined(__linux__) || defined(__APPLE__)
        // OS name and version via uname
        {
            struct utsname uts = {};
            if (uname(&uts) == 0) {
                env["os_name"] = std::string(uts.sysname);
                env["os_version"] = std::string(uts.release);
                env["os_architecture"] = std::string(uts.machine);
            }
        }

        // CPU core count
#if defined(__linux__)
        {
            long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
            if (nprocs > 0) {
                env["cpu_core_count"] = static_cast<int>(nprocs);
            }
        }
#elif defined(__APPLE__)
        {
            int ncpu = 0;
            size_t len = sizeof(ncpu);
            if (sysctlbyname("hw.logicalcpu", &ncpu, &len, nullptr, 0) == 0) {
                env["cpu_core_count"] = ncpu;
            }
        }
#endif

        // Total RAM
#if defined(__linux__)
        {
            struct sysinfo si = {};
            if (sysinfo(&si) == 0) {
                uint64_t total_mb = (static_cast<uint64_t>(si.totalram) * si.mem_unit) / (1024 * 1024);
                env["total_ram_mb_bucket"] = ram_bucket(total_mb);
            }
        }
#elif defined(__APPLE__)
        {
            uint64_t memsize = 0;
            size_t len = sizeof(memsize);
            if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
                uint64_t total_mb = memsize / (1024 * 1024);
                env["total_ram_mb_bucket"] = ram_bucket(total_mb);
            }
        }
#endif

        // Machine name hash
        {
            char hostname[256] = {};
            if (gethostname(hostname, sizeof(hostname)) == 0) {
                std::string hash = sha256_hex(std::string(hostname));
                if (!hash.empty()) {
                    env["machine_name_hash"] = hash;
                }
            }
        }

        // Locale
        {
            const char* loc = std::setlocale(LC_ALL, nullptr);
            if (loc) {
                env["locale"] = std::string(loc);
            }
        }

        // Display: omitted on Linux/macOS per PRD
#endif

        // Common fields
        env["runtime_name"] = "C++ (Beacon SDK)";
        env["runtime_version"] = std::to_string(__cplusplus);

    } catch (...) {
        // If entire collection fails, return empty
        if (env.empty()) return {};
    }

    return env.dump();
}

} // namespace internal
} // namespace beacon

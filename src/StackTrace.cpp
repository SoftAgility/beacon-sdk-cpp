#include "StackTrace.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #if defined(BEACON_STACK_TRACES_ENABLED)
        #include <dbghelp.h>
        #pragma comment(lib, "dbghelp.lib")
    #endif
    #include <mutex>
#elif defined(__linux__) || defined(__APPLE__)
    #include <execinfo.h>
    #include <cxxabi.h>
#endif

namespace beacon {
namespace internal {

#if defined(_WIN32) && defined(BEACON_STACK_TRACES_ENABLED)

namespace {
    std::once_flag sym_init_flag;

    void ensure_sym_initialized() {
        std::call_once(sym_init_flag, []() {
            HANDLE process = GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
            SymInitialize(process, nullptr, TRUE);
        });
    }
} // anonymous namespace

std::string capture_stack_trace() {
    try {
        ensure_sym_initialized();

        void* frames[62] = {};
        USHORT count = CaptureStackBackTrace(1, 62, frames, nullptr);
        if (count == 0) return {};

        HANDLE process = GetCurrentProcess();
        std::string result;
        result.reserve(4096);

        char symbol_buffer[sizeof(SYMBOL_INFO) + 256 * sizeof(char)];

        for (USHORT i = 0; i < count; ++i) {
            if (!frames[i]) continue;

            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;

            DWORD64 displacement = 0;
            std::string frame_str = "[" + std::to_string(i) + "] ";

            if (SymFromAddr(process, reinterpret_cast<DWORD64>(frames[i]), &displacement, symbol)) {
                frame_str += symbol->Name;
            } else {
                frame_str += "???";
            }

            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, reinterpret_cast<DWORD64>(frames[i]), &line_displacement, &line)) {
                frame_str += " (";
                frame_str += line.FileName;
                frame_str += ":";
                frame_str += std::to_string(line.LineNumber);
                frame_str += ")";
            }

            result += frame_str;
            result += "\n";

            if (result.size() >= 32768) {
                result.resize(32768);
                break;
            }
        }

        return result;
    } catch (...) {
        return {};
    }
}

#elif defined(__linux__) || defined(__APPLE__)

std::string capture_stack_trace() {
    try {
        void* frames[64] = {};
        int count = backtrace(frames, 64);
        if (count <= 0) return {};

        char** symbols = backtrace_symbols(frames, count);
        if (!symbols) return {};

        std::string result;
        result.reserve(4096);

        for (int i = 1; i < count; ++i) {
            result += symbols[i];
            result += "\n";
            if (result.size() >= 32768) {
                result.resize(32768);
                break;
            }
        }

        free(symbols);
        return result;
    } catch (...) {
        return {};
    }
}

#else

std::string capture_stack_trace() {
    return {};
}

#endif

std::string demangle_type_name(const char* mangled_name) {
    if (!mangled_name) return "unknown";

#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        free(demangled);
        return result;
    }
    return std::string(mangled_name);
#else
    return std::string(mangled_name);
#endif
}

} // namespace internal
} // namespace beacon

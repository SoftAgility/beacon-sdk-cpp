#include "DeviceId.hpp"
#include "UuidV7.hpp"

#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <shlobj.h>
    #include <direct.h>
#else
    #include <pwd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace beacon {
namespace internal {

std::string sanitize_path_component(const std::string& input) {
    std::string result = input;
    for (auto& c : result) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    return result;
}

namespace {

#if defined(_WIN32)

void create_directories(const std::string& path) {
    // Simple recursive directory creation for Windows
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if ((path[i] == '/' || path[i] == '\\') && current.size() > 1) {
            CreateDirectoryA(current.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

std::string get_device_id_path(const std::string& safe_app_name) {
    char appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::string dir = std::string(appdata) + "\\SoftAgility\\Beacon\\" + safe_app_name;
        return dir + "\\device_id.txt";
    }
    return {};
}

#elif defined(__APPLE__)

void create_directories(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' && current.size() > 1) {
            mkdir(current.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string get_device_id_path(const std::string& safe_app_name) {
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (home) {
        std::string dir = std::string(home) + "/Library/Application Support/SoftAgility/Beacon/" + safe_app_name;
        return dir + "/device_id.txt";
    }
    return {};
}

#else // Linux

void create_directories(const std::string& path) {
    std::string current;
    for (size_t i = 0; i < path.size(); ++i) {
        current += path[i];
        if (path[i] == '/' && current.size() > 1) {
            mkdir(current.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

std::string get_device_id_path(const std::string& safe_app_name) {
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }

    if (home) {
        std::string dir = std::string(home) + "/.local/share/SoftAgility/Beacon/" + safe_app_name;
        return dir + "/device_id.txt";
    }

    // Fallback
    std::string dir = "/var/lib/SoftAgility/Beacon/" + safe_app_name;
    return dir + "/device_id.txt";
}

#endif

} // anonymous namespace

std::string get_data_directory(const std::string& app_name) {
    std::string safe_name = sanitize_path_component(app_name);
    std::string path = get_device_id_path(safe_name);
    if (path.empty()) return {};
    // Return directory portion (strip filename)
    auto pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        return path.substr(0, pos);
    }
    return {};
}

void write_device_id(const std::string& app_name, const std::string& uuid) {
    std::string safe_name = sanitize_path_component(app_name);
    std::string path = get_device_id_path(safe_name);
    if (path.empty()) {
        throw std::runtime_error("beacon: cannot determine device ID path.");
    }

    // Ensure directory exists
    std::string dir = path.substr(0, path.find_last_of("/\\"));
    create_directories(dir);

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        throw std::runtime_error("beacon: cannot open device ID file for writing.");
    }
    ofs << uuid;
    ofs.close();
    if (ofs.fail()) {
        throw std::runtime_error("beacon: failed to write device ID file.");
    }
}

std::string get_or_create_device_id(const std::string& app_name) {
    std::string safe_name = sanitize_path_component(app_name);
    std::string path = get_device_id_path(safe_name);

    if (path.empty()) {
        return new_uuid_v7();
    }

    // Try to read existing
    try {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            std::string id;
            std::getline(ifs, id);
            if (!id.empty()) {
                return id;
            }
        }
    } catch (...) {
        // Fall through to create
    }

    // Generate new device ID
    std::string new_id = new_uuid_v7();

    // Create directory and write file
    try {
        std::string dir = path.substr(0, path.find_last_of("/\\"));
        create_directories(dir);

        std::ofstream ofs(path);
        if (ofs.is_open()) {
            ofs << new_id;
            ofs.close();
        }
    } catch (...) {
        // Could not persist - return transient ID
    }

    return new_id;
}

} // namespace internal
} // namespace beacon

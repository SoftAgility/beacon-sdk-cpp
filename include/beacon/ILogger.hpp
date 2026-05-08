#pragma once

#include <string>

namespace beacon {

enum class LogLevel { Debug, Info, Warning, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, const std::string& message) = 0;
};

} // namespace beacon

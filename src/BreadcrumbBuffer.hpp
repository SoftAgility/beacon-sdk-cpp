#pragma once

#include "beacon/internal/Types.hpp"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace beacon {
namespace internal {

class BreadcrumbBuffer {
public:
    explicit BreadcrumbBuffer(int max_breadcrumbs);

    void add(const std::string& category, const std::string& name,
             const std::string& timestamp,
             const std::unordered_map<std::string, std::string>& properties);

    std::vector<BreadcrumbEntry> snapshot() const;

    size_t size() const;

private:
    int max_breadcrumbs_;
    mutable std::mutex mutex_;
    std::deque<BreadcrumbEntry> buffer_;
};

} // namespace internal
} // namespace beacon

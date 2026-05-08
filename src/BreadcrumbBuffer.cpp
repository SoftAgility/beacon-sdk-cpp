#include "BreadcrumbBuffer.hpp"

namespace beacon {
namespace internal {

BreadcrumbBuffer::BreadcrumbBuffer(int max_breadcrumbs)
    : max_breadcrumbs_(max_breadcrumbs) {}

void BreadcrumbBuffer::add(const std::string& category, const std::string& name,
                           const std::string& timestamp,
                           const std::unordered_map<std::string, std::string>& properties) {
    if (max_breadcrumbs_ <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    BreadcrumbEntry entry;
    entry.category = category;
    entry.name = name;
    entry.timestamp = timestamp;
    entry.properties = properties;

    buffer_.push_back(std::move(entry));

    while (static_cast<int>(buffer_.size()) > max_breadcrumbs_) {
        buffer_.pop_front();
    }
}

std::vector<BreadcrumbEntry> BreadcrumbBuffer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<BreadcrumbEntry>(buffer_.begin(), buffer_.end());
}

size_t BreadcrumbBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffer_.size();
}

} // namespace internal
} // namespace beacon

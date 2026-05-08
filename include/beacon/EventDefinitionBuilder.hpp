#pragma once

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace beacon {

class EventDefinitionBuilder {
public:
    EventDefinitionBuilder() = default;

    EventDefinitionBuilder& define(std::string category, std::string name) {
        if (category.empty()) {
            throw std::invalid_argument("category must not be empty.");
        }
        if (name.empty()) {
            throw std::invalid_argument("name must not be empty.");
        }
        definitions_.emplace(std::move(category), std::move(name));
        return *this;
    }

    std::vector<std::pair<std::string, std::string>> build() const {
        std::vector<std::pair<std::string, std::string>> result(definitions_.begin(), definitions_.end());
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });
        return result;
    }

private:
    std::set<std::pair<std::string, std::string>> definitions_;
};

} // namespace beacon

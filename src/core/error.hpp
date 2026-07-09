// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace pu {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& msg) : std::runtime_error(msg) {}
};

class HttpError : public std::runtime_error {
public:
    explicit HttpError(const std::string& msg) : std::runtime_error(msg) {}
};

class StoreError : public std::runtime_error {
public:
    explicit StoreError(const std::string& msg) : std::runtime_error(msg) {}
};

}  // namespace pu

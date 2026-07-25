// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace pu {


class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class HttpError : public Error {
public:
    explicit HttpError(const std::string& msg) : Error(msg), detail_(msg) {}
    explicit HttpError(const std::string& msg, const std::string& detail)
        : Error(msg), detail_(detail) {}
    const std::string& detail() const { return detail_; }

private:
    std::string detail_;
};

class StoreError : public Error {
public:
    explicit StoreError(const std::string& msg) : Error(msg) {}
};

}  // namespace pu
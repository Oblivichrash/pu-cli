// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace pu {

// 新增：项目统一异常基类
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 令 HttpError 继承自 pu::Error 而非直接继承 std::runtime_error
class HttpError : public Error {
public:
    explicit HttpError(const std::string& msg) : Error(msg), detail_(msg) {}
    explicit HttpError(const std::string& msg, const std::string& detail)
        : Error(msg), detail_(detail) {}
    const std::string& detail() const { return detail_; }

private:
    std::string detail_;
};

// 令 StoreError 继承自 pu::Error
class StoreError : public Error {
public:
    explicit StoreError(const std::string& msg) : Error(msg) {}
};

}  // namespace pu
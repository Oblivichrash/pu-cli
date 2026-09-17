// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdexcept>
#include <string>

namespace pu {

// Base class for all non-recoverable runtime errors.
// Catchers at the top level can catch this (or std::exception) to
// produce a friendly error message without crashing.
class RuntimeError : public std::runtime_error {
public:
    explicit RuntimeError(const std::string& msg) : std::runtime_error(msg) {}
};

// General-purpose error (e.g. configuration parsing)
class Error : public RuntimeError {
public:
    using RuntimeError::RuntimeError;
};

// HTTP / network errors (from HttpClient)
class HttpError : public Error {
public:
    explicit HttpError(const std::string& msg) : Error(msg) {}
};

}  // namespace pu

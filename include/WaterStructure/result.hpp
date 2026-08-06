#pragma once

#include <string>
#include <utility>

namespace water_structure {

template <typename T>
class Result {
public:
    static Result success(T value) {
        Result result;
        result.mOk = true;
        result.mValue = std::move(value);
        return result;
    }

    static Result failure(std::string error) {
        Result result;
        result.mError = std::move(error);
        return result;
    }

    explicit operator bool() const noexcept { return mOk; }
    bool ok() const noexcept { return mOk; }
    const std::string& error() const noexcept { return mError; }
    T& value() & { return mValue; }
    const T& value() const& { return mValue; }
    T&& value() && { return std::move(mValue); }

private:
    bool mOk = false;
    T mValue{};
    std::string mError;
};

template <>
class Result<void> {
public:
    static Result success() {
        Result result;
        result.mOk = true;
        return result;
    }

    static Result failure(std::string error) {
        Result result;
        result.mError = std::move(error);
        return result;
    }

    explicit operator bool() const noexcept { return mOk; }
    bool ok() const noexcept { return mOk; }
    const std::string& error() const noexcept { return mError; }

private:
    bool mOk = false;
    std::string mError;
};

} // namespace water_structure

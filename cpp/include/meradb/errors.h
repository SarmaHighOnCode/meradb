// cpp/include/meradb/errors.h
#pragma once
#include <stdexcept>
#include <string>

namespace meradb {

class MeraDBError : public std::exception {
public:
    explicit MeraDBError(std::string message) : message_(std::move(message)) {
        refreshFormatted();
    }
    virtual ~MeraDBError() = default;
    virtual std::string stage() const { return "MeraDB"; }
    const std::string& message() const { return message_; }
    const char* what() const noexcept override { return formatted_.c_str(); }
    // The same text as what(), as a std::string: embedded NUL bytes (a tokenizer error can quote one) survive.
    const std::string& formatted() const noexcept { return formatted_; }

protected:
    // Subclass constructors call this after their own stage() becomes
    // callable via the vtable (i.e. from the subclass's own constructor
    // body), so `formatted_` reflects the correct stage name.
    void refreshFormatted() { formatted_ = "[" + stage() + " Galti] " + message_; }

private:
    std::string message_;
    std::string formatted_;
};

class TokenizerError : public MeraDBError {
public:
    explicit TokenizerError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Tokenizer"; }
};

class ParseError : public MeraDBError {
public:
    explicit ParseError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Parser"; }
};

class ExecutionError : public MeraDBError {
public:
    explicit ExecutionError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Execution"; }
};

class StorageError : public MeraDBError {
public:
    explicit StorageError(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Storage"; }
};

class ConnectionFailed : public MeraDBError {
public:
    explicit ConnectionFailed(std::string message) : MeraDBError(std::move(message)) { refreshFormatted(); }
    std::string stage() const override { return "Connection"; }
};

class ServerUnavailable : public ConnectionFailed {
public:
    explicit ServerUnavailable(std::string message) : ConnectionFailed(std::move(message)) { refreshFormatted(); }
    // stage() inherited from ConnectionFailed — matches Python's
    // ServerUnavailable(ConnectionFailed), which doesn't override stage either.
};

}  // namespace meradb

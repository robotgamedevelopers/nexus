#pragma once
#include <string>
#include <utility>

namespace nexus::store {

// The result of a store call. Ok, or one of:
//   Refused   the call broke a rule of the state (a full queue, a grow with no open entry, a chunk past the size); the
//             state did not change and the store is fine
//   NotFound  a file or item that is not there (the filesystem seam uses it; the store turns it into a meaning)
//   Failed    the disk failed or what is on it is not what the store wrote. After a failed write or fsync the store
//             refuses every later change (what reached the disk is unknown); restarting the server reopens it from
//             what is durable
// The message says what happened and how to fix it.
class Status {
public:
    enum class Code { Ok, Refused, NotFound, Failed };

    Status() = default;
    static Status ok() { return Status(); }
    static Status refused(std::string why) { return Status(Code::Refused, std::move(why)); }
    static Status not_found(std::string why) { return Status(Code::NotFound, std::move(why)); }
    static Status failed(std::string why) { return Status(Code::Failed, std::move(why)); }

    bool is_ok() const { return code_ == Code::Ok; }
    Code code() const { return code_; }
    const std::string& message() const { return message_; }

private:
    Status(Code code, std::string message) : code_(code), message_(std::move(message)) {}
    Code code_ = Code::Ok;
    std::string message_;
};

} // namespace nexus::store

#ifndef ATOMDB_DB_ERROR_HPP
#define ATOMDB_DB_ERROR_HPP

#include <string>
#include <utility>

namespace atomdb {

enum class DbErrorCode {
    NotFound,
    Deadlock,
    LockBusy,
    ParseError,
    NotSupported,
    Internal,
};

// DbError: structured error crossing every interface boundary (spec §3.2).
// Named constructors for the common cases — preferred over ad-hoc string codes
// because named factories prevent typos and centralize code mapping.
class DbError {
public:
    DbError(DbErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    DbErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

    static DbError deadlock(std::string msg = "") {
        return DbError(DbErrorCode::Deadlock, std::move(msg));
    }
    static DbError notFound(std::string msg = "") {
        return DbError(DbErrorCode::NotFound, std::move(msg));
    }
    static DbError lockBusy(std::string msg = "") {
        return DbError(DbErrorCode::LockBusy, std::move(msg));
    }
    static DbError parseError(std::string msg = "") {
        return DbError(DbErrorCode::ParseError, std::move(msg));
    }
    static DbError notSupported(std::string msg = "") {
        return DbError(DbErrorCode::NotSupported, std::move(msg));
    }
    static DbError internal(std::string msg = "") {
        return DbError(DbErrorCode::Internal, std::move(msg));
    }
    // Sentinel success: returned by storage engine methods that have no
    // failure path to express. Code() == Internal but message() == ""
    // by convention; callers check message().empty() rather than code().
    static DbError sentinel() { return DbError(DbErrorCode::Internal, ""); }
    bool isSentinel() const noexcept { return message_.empty(); }

    std::string toString() const {
        const char* name = "Unknown";
        switch (code_) {
            case DbErrorCode::NotFound:     name = "NotFound";     break;
            case DbErrorCode::Deadlock:     name = "Deadlock";     break;
            case DbErrorCode::LockBusy:     name = "LockBusy";     break;
            case DbErrorCode::ParseError:   name = "ParseError";   break;
            case DbErrorCode::NotSupported: name = "NotSupported"; break;
            case DbErrorCode::Internal:     name = "Internal";     break;
        }
        std::string s = name;
        if (!message_.empty()) {
            s += ": ";
            s += message_;
        }
        return s;
    }

private:
    DbErrorCode code_;
    std::string message_;
};

} // namespace atomdb

#endif // ATOMDB_DB_ERROR_HPP

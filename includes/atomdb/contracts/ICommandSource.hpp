#ifndef ATOMDB_ICOMMAND_SOURCE_HPP
#define ATOMDB_ICOMMAND_SOURCE_HPP

#include <optional>

#include "atomdb/types/Command.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Result.hpp"

namespace atomdb {

// ICommandSource: the front-end plugin contract (spec §4.1).
//
// Any front-end (SQL parser, REPL, HTTP gateway) implements this interface so
// that the core engine can pull commands one at a time without knowing how
// input arrives — interactive line-based or batched. Separating the producer
// from the core loop keeps the loop trivial and the I/O-shaped concerns
// isolated.
//
// The pull model (`nextCommand` returning std::nullopt at end-of-input) is
// simple but synchronous; the spec §4.1 notes an HTTP / gRPC front-end would
// likely want an async/queue model and would require a rethink of this contract.
class ICommandSource {
public:
    virtual ~ICommandSource() = default;

    // Return the next Command to execute, or std::nullopt to signal that input
    // has ended (EOF, closed socket, REPL exit). Returning a Command with
    // CommandType::Select etc. that the EngineLoop will dispatch through the
    // full lock/handler/commit pipeline.
    virtual std::optional<Command> nextCommand() = 0;

    // Present a successful result to whatever this front-end represents. For an
    // interactive REPL this prints an ASCII table; for an HTTP gateway it
    // serializes to the wire format.
    virtual void present(const ResultSet& result) = 0;

    // Present an error to the front-end. No return value is required; the source
    // may continue producing commands (e.g. REPL after a parse error) or may
    // signal end-of-input on the next call.
    virtual void present(const DbError& error) = 0;
};

} // namespace atomdb

#endif // ATOMDB_ICOMMAND_SOURCE_HPP

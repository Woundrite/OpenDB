#ifndef ATOMDB_IACCESS_PLUGIN_HPP
#define ATOMDB_IACCESS_PLUGIN_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "atomdb/contracts/ICommandSource.hpp"
#include "atomdb/types/Command.hpp"
#include "atomdb/types/DbError.hpp"

namespace atomdb {

// Forward declarations to break include cycles.
class IStorageProvider;
class IEngineDispatcher;

// Access mode: tells the dispatcher and the user code what to expect from a
// plugin's run() block. Interactive plugins (stdio REPL) block until one
// session ends. Batch plugins (SQL file) run to EOF in a single session.
// Server plugins (HTTP) enter an accept loop that yields many sessions.
enum class AccessMode : std::uint8_t {
    Interactive,   // REPL, PTY, websockets (one session, blocks)
    Batch,         // file/pipe (one session, blocks to EOF)
    Server,        // TCP listener (accept loop yields multiple sessions)
};

// Capability flags. Like StorageCapability, these describe what the plugin
// advertises so the user / launcher can pick appropriate plugins.
enum class AccessCapability : std::uint32_t {
    JsonWire     = 1u << 0,  // speak JSON natively (HTTP plugin)
    SqlWire      = 1u << 1,  // speaks SQL text (SQL plugin)
    Streaming    = 1u << 2,  // supports backpressure / chunked resultsets
    AsyncCapable = 1u << 3,  // can dispatch concurrent commands
    AuthCapable  = 1u << 4,  // can authenticate clients
};

// ISession: one connection's ICommandSource over its transport handle.
// The dispatcher's worker threads pull sessions off the queue and call
// runSession(session) which loops nextCommand() until std::nullopt.
//
// Inheritance IS-A ICommandSource is the *natural* fit here: a session
// *is* a command source from the dispatcher's perspective. The dispatcher
// holds it by base reference and doesn't need to know it's a session.
class ISession : public ICommandSource {
public:
    // Optional: shut down this session's underlying transport. Called by the
    // dispatcher worker thread once nextCommand() returns nullopt (EOF).
    // Safe to call multiple times.
    virtual void close() = 0;
};

// IAccessPlugin: the front-end plugin contract above ICommandSource.
// Per the design preference, composition shows up where it's clean:
//   - A concrete plugin (ReplAccessPlugin, SqlAccessPlugin, HttpAccessPlugin)
//     implements IAccessPlugin. It does NOT inherit from ICommandSource —
//     it yields ISessions via the dispatcher.
//   - The session itself (ISession) DOES inherit ICommandSource (natural fit).
//
// Threading: per Phase 4 the engine is multi-threaded.
//   - Server plugins' run() blocks on accept; each accepted connection becomes
//     an ISession that is enqueued onto the dispatcher. A worker thread pulls
//     that session and runs it to EOF.
//   - Interactive / batch plugins' run() executes their single session inline
//     on the calling thread (one-shot; no dispatch needed).
//
// Cross-edge coupling is minimal but intentional: open() receives the
// IStorageProvider* so the plugin can probe capabilities and reject DDL that
// names types the provider doesn't accept. The core engine never sees this
// negotiation.
class IAccessPlugin {
public:
    virtual ~IAccessPlugin() = default;

    // ---- Identity / capability probing ---------------------------------------
    virtual std::string  name() const = 0;
    virtual AccessMode  mode() const = 0;
    virtual std::uint32_t capabilities() const = 0;

    // ---- Lifecycle ----------------------------------------------------------
    // `storage` may be queried for TypeVocabulary etc.; `dispatcher` is where
    // server plugins enqueue sessions. Either may be nullptr when the plugin
    // doesn't need it (e.g. an interactive REPL ignores dispatcher).
    virtual DbError open(const std::string& uri,
                         IStorageProvider* storage,
                         IEngineDispatcher* dispatcher) = 0;
    virtual DbError close() = 0;
    virtual bool    isOpen() const noexcept = 0;

    // For interactive / batch plugins, blocks until the single session ends.
    // For server plugins, blocks on the accept loop (returns when close()
    // is called from another thread or the transport is shut down).
    virtual void run() = 0;

    // Optional introspection used by /schema endpoints and REPL \d commands.
    // Format chosen by the plugin (JSON for HTTP; human-readable for REPL).
    virtual std::string describeServer() const = 0;
};

// IEngineDispatcher: the interface the access plugin uses to enqueue a
// session onto the engine's worker pool. The concrete EngineDispatcher lives
// in core/EngineDispatcher.hpp and is constructed by main.cpp / the ProviderRegistry.
// Forward-declared here to break the include cycle; defined in the dispatcher header.
class IEngineDispatcher {
public:
    virtual ~IEngineDispatcher() = default;
    // Ownership of the session transfers to the dispatcher. The dispatcher
    // runs the session to EOF on a worker thread and destroys it.
    virtual void enqueue(std::unique_ptr<ISession> session) = 0;
};

} // namespace atomdb

#endif // ATOMDB_IACCESS_PLUGIN_HPP

// main.cpp — atomdb REPL driver (milestone 5).
//
// Wires the dependencies per spec §2.1: an ICommandSource (REPL) feeds commands
// into the non-pluggable EngineLoop, which delegates to an IStorageEngine
// (In-Memory for v0.1), TransactionManager (for TxnIds and visibleSeq), and
// LockManager + DeadlockDetector for concurrency control.

#include <iostream>

#include "atomdb/core/DeadlockDetector.hpp"
#include "atomdb/core/EngineLoop.hpp"
#include "atomdb/core/LockManager.hpp"
#include "atomdb/core/TransactionManager.hpp"
#include "atomdb/frontend/ReplSource.hpp"
#include "atomdb/storage/InMemoryStorageEngine.hpp"

int main() {
    atomdb::TransactionManager txn_mgr;
    atomdb::LockManager        lock_mgr;
    atomdb::DeadlockDetector   deadlock_detector(lock_mgr);
    atomdb::InMemoryStorageEngine storage;
    atomdb::ReplSource         repl(std::cin, std::cout);
    atomdb::EngineLoop        engine(repl,
                                      storage,
                                      txn_mgr,
                                      lock_mgr,
                                      deadlock_detector);

    std::cout << "atomdb v0.1 — type EXIT to quit\n";
    engine.run();
    std::cout << "bye\n";
    return 0;
}

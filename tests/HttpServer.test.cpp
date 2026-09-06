#include "opendb/frontend/HttpServer.hpp"
#include "opendb/frontend/HttpSession.hpp"
#include "opendb/frontend/JsonEncoder.hpp"
#include "opendb/frontend/HttpApi.hpp"
#include "opendb/core/EngineDispatcher.hpp"
#include "opendb/core/TransactionManager.hpp"
#include "opendb/core/LockManager.hpp"
#include "opendb/core/DeadlockDetector.hpp"
#include "opendb/storage/InMemoryStorageProvider.hpp"
#include "opendb/types/Schema.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/Value.hpp"
#include "opendb/types/TxnId.hpp"
#include "opendb/contracts/IStorageProvider.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <thread>

using namespace opendb;

TEST(HttpServer_Configuration_Port_Zero_Lets_OS_Choose) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50));

    HttpServer::Config cfg;
    cfg.port = 0;
    cfg.ioThreadCount = 2;

    HttpServer server(provider.get(), &dispatcher, txnm, lockMgr, dd);
    EXPECT(server.listen(cfg));
    EXPECT(server.isListening());
    EXPECT(server.boundPort() > 0);

    server.stop();
    server.join();
    dispatcher.shutdown();
}

TEST(HttpServer_Configuration_Respects_Thread_Count) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(4, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50));

    HttpServer::Config cfg;
    cfg.port = 0;
    cfg.ioThreadCount = 4;

    HttpServer server(provider.get(), &dispatcher, txnm, lockMgr, dd);
    EXPECT(server.listen(cfg));

    server.stop();
    server.join();
    dispatcher.shutdown();
}

TEST(HttpServer_Stats_Default_Zero) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50));

    HttpServer server(provider.get(), &dispatcher, txnm, lockMgr, dd);
    EXPECT_EQ(server.stats().requestsServed.load(), std::uint64_t{0});
    EXPECT_EQ(server.stats().connectionsAccepted.load(), std::uint64_t{0});

    auto snap = server.renderStatsSnapshot();
    EXPECT(snap.find("\"requests_served\":0") != std::string::npos);
    EXPECT(snap.find("\"connections_accepted\":0") != std::string::npos);
}

TEST(HttpServer_RenderStats_Contains_All_Fields) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50));

    HttpServer server(provider.get(), &dispatcher, txnm, lockMgr, dd);
    auto snap = server.renderStatsSnapshot();
    EXPECT(snap.find("connections_accepted") != std::string::npos);
    EXPECT(snap.find("connections_active")  != std::string::npos);
    EXPECT(snap.find("requests_served")     != std::string::npos);
    EXPECT(snap.find("requests_conflicted") != std::string::npos);
    EXPECT(snap.find("requests_rejected")   != std::string::npos);
    EXPECT(snap.find("total_latency_us")    != std::string::npos);
    EXPECT(snap.find("max_latency_us")      != std::string::npos);
}

TEST(HttpServer_Stop_Without_Listen_Is_Safe) {
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50));

    HttpServer server(provider.get(), &dispatcher, txnm, lockMgr, dd);
    // Calling stop/join without listen must be a no-op.
    server.stop();
    server.join();
    EXPECT(!server.isListening());
}

TEST(HttpServer_Multiple_Servers_Can_Coexist) {
    // Two independent servers on the same test process: different ports,
    // independent threads. Both must come up and shut down cleanly.
    auto provider = std::make_unique<InMemoryStorageProvider>();
    EXPECT(provider->open("in-memory://").isSentinel());

    TransactionManager txnm;
    LockManager lockMgr;
    DeadlockDetector dd(lockMgr);
    EngineDispatcher dispatcher(2, provider.get(), txnm, lockMgr, dd, std::chrono::seconds(50));

    HttpServer s1(provider.get(), &dispatcher, txnm, lockMgr, dd);
    HttpServer::Config c1;
    c1.port = 0;
    c1.ioThreadCount = 2;
    EXPECT(s1.listen(c1));

    HttpServer s2(provider.get(), &dispatcher, txnm, lockMgr, dd);
    HttpServer::Config c2;
    c2.port = 0;
    c2.ioThreadCount = 2;
    EXPECT(s2.listen(c2));

    EXPECT(s1.boundPort() != s2.boundPort());

    s1.stop();
    s2.stop();
    s1.join();
    s2.join();
    dispatcher.shutdown();
}

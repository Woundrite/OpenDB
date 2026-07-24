#ifndef ATOMDB_SHARDED_STORAGE_PROVIDER_HPP
#define ATOMDB_SHARDED_STORAGE_PROVIDER_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/storage/ShardedStorageEngine.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Schema.hpp"
#include "atomdb/types/Value.hpp"

namespace atomdb {

// ShardedStorageProvider: IStorageProvider implementation that wraps multiple
// child IStorageProvider instances and routes operations based on hash
// partitioning.
//
// Composition model (per design decision: prefer composition over inheritance):
//   - The provider IMPLEMENTS IStorageProvider and OWNS a ShardedStorageEngine
//     internally. It exposes the engine via engine().
//   - The ShardedStorageEngine wraps the child providers' engines and routes
//     DML by key hash.
//
// Schema model: schemas are propagated to ALL child providers (each shard
// holds the same DDL definition). The provider caches schemas in-core
// (mutex-guarded) for fast describeTable/tables without round-tripping.
class ShardedStorageProvider : public IStorageProvider {
public:
    explicit ShardedStorageProvider(std::vector<std::unique_ptr<IStorageProvider>> shards,
                                    bool children_already_open = false) {
        std::vector<IStorageEngine*> engines;
        for (auto& s : shards) {
            engines.push_back(s->engine());
            shards_.push_back(std::move(s));
        }
        engine_ = std::make_unique<ShardedStorageEngine>(std::move(engines));
        // ponytail: callers that pre-open children (e.g., each shard on its own
        // file path) want open() to be a no-op — re-opening with the parent
        // URI would clobber the per-shard URIs. Default keeps the legacy
        // "reopen-all-with-same-URI" behavior.
        children_already_open_ = children_already_open;
        if (children_already_open) {
            opened_ = true;
        }
    }

    ~ShardedStorageProvider() override = default;

    // ---- IStorageProvider -----------------------------------------------------
    std::string name() const override { return "sharded"; }

    std::uint32_t capabilities() const override {
        if (shards_.empty()) return 0;
        // Union of child capabilities
        std::uint32_t caps = 0;
        for (auto& s : shards_) {
            caps |= s->capabilities();
        }
        return caps;
    }

    TypeVocabulary typeVocabulary() const override {
        if (shards_.empty()) return TypeVocabulary{};
        return shards_[0]->typeVocabulary();
    }

    DbError open(const std::string& uri) override {
        if (shards_.empty()) return DbError::internal("no shards configured");
        // ponytail: children_already_open_ skips per-child re-opens so per-shard
        // URIs (e.g. shard0.db, shard1.db) aren't overwritten by the parent URI.
        if (children_already_open_) {
            opened_ = true;
            return DbError::sentinel();
        }
        // Open all children. Roll back on any failure.
        std::size_t openedCount = 0;
        for (auto& s : shards_) {
            auto r = s->open(uri);
            if (!r.isSentinel()) {
                for (std::size_t i = 0; i < openedCount; ++i) {
                    shards_[i]->close();
                }
                return r;
            }
            ++openedCount;
        }
        opened_ = true;
        return DbError::sentinel();
    }

    DbError close() override {
        for (auto& s : shards_) {
            s->close();
        }
        opened_ = false;
        return DbError::sentinel();
    }

    bool isOpen() const noexcept override { return opened_; }

    DbError createTable(const Schema& schema) override {
        if (!opened_) return DbError::internal("provider not open");

        // Validate types against vocabulary
        auto tv = typeVocabulary();
        auto isAccepted = [&](ValueType t) {
            for (auto a : tv.accepted) if (a == t) return true;
            return false;
        };
        for (const auto& col : schema.columns) {
            if (!isAccepted(col.type)) {
                return DbError::notSupported(
                    "column '" + col.name + "' type not accepted by sharded provider");
            }
        }

        // If partitioned (Hash), ensure shard count matches
        if (schema.partition && schema.partition->kind == PartitionPolicy::Kind::Hash) {
            if (schema.partition->shardCount != shards_.size()) {
                return DbError::notSupported(
                    "partition shardCount (" + std::to_string(schema.partition->shardCount) +
                    ") must match provider shard count (" + std::to_string(shards_.size()) + ")");
            }
            if (schema.partition->column.empty()) {
                return DbError::notSupported("hash partition requires a column name");
            }
        }

        // Create on all shards
        for (auto& s : shards_) {
            auto r = s->createTable(schema);
            if (!r.isSentinel()) return r;
        }
        // Cache schema locally
        {
            const std::lock_guard<std::mutex> lk(schema_mu_);
            schemas_[schema.table] = schema;
        }
        return DbError::sentinel();
    }

    DbError dropTable(const std::string& name) override {
        if (!opened_) return DbError::internal("provider not open");
        for (auto& s : shards_) {
            auto r = s->dropTable(name);
            if (!r.isSentinel()) return r;
        }
        {
            const std::lock_guard<std::mutex> lk(schema_mu_);
            schemas_.erase(name);
        }
        return DbError::sentinel();
    }

    std::optional<Schema> describeTable(const std::string& name) const override {
        const std::lock_guard<std::mutex> lk(schema_mu_);
        auto it = schemas_.find(name);
        if (it == schemas_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<std::string> tables() const override {
        const std::lock_guard<std::mutex> lk(schema_mu_);
        std::vector<std::string> out;
        out.reserve(schemas_.size());
        for (const auto& [name, _] : schemas_) out.push_back(name);
        return out;
    }

    IStorageEngine* engine() override { return engine_.get(); }

    // Diagnostic: number of shards
    std::size_t shardCount() const noexcept { return shards_.size(); }

private:
    std::vector<std::unique_ptr<IStorageProvider>> shards_;
    std::unique_ptr<ShardedStorageEngine> engine_;
    mutable std::mutex schema_mu_;
    std::unordered_map<std::string, Schema> schemas_;
    bool opened_ = false;
    bool children_already_open_ = false;
};

} // namespace atomdb

#endif // ATOMDB_SHARDED_STORAGE_PROVIDER_HPP
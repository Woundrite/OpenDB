#ifndef ATOMDB_IN_MEMORY_STORAGE_PROVIDER_HPP
#define ATOMDB_IN_MEMORY_STORAGE_PROVIDER_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "atomdb/contracts/IStorageProvider.hpp"
#include "atomdb/storage/InMemoryStorageEngine.hpp"
#include "atomdb/types/DbError.hpp"
#include "atomdb/types/Schema.hpp"
#include "atomdb/types/Value.hpp"

namespace atomdb {

// InMemoryStorageProvider: the RAM-only storage provider (spec §4.2 v0.1).
//
// Composition model (per design decision): the provider IMPLEMENTS IStorageProvider
// and OWNS an InMemoryStorageEngine internally. It exposes the engine via engine().
// Capabilities: NOT Durable, NOT CrashDurable. Accepts all 9 ValueType tags.
// Schema: stored in-core alongside the engine's data; createTable registers a
// Schema, dropTable removes it, describeTable/tables query it.
//
// Threading: the inner engine is mutex-guarded; the schema registry is separately
// mutex-guarded. Concurrent flag is declared.
class InMemoryStorageProvider : public IStorageProvider {
public:
    InMemoryStorageProvider()
        : engine_(std::make_unique<InMemoryStorageEngine>()) {}

    // ---- IStorageProvider -----------------------------------------------------
    std::string name() const override { return "in-memory"; }

    std::uint32_t capabilities() const override {
        return static_cast<std::uint32_t>(StorageCapability::RandomAccess)
             | static_cast<std::uint32_t>(StorageCapability::OrderedScan)
             | static_cast<std::uint32_t>(StorageCapability::BlobSupport)
             | static_cast<std::uint32_t>(StorageCapability::TemporalSupport)
             | static_cast<std::uint32_t>(StorageCapability::Concurrent);
    }

    TypeVocabulary typeVocabulary() const override {
        TypeVocabulary tv;
        tv.accepted = {
            ValueType::Null, ValueType::Bool,
            ValueType::Int32, ValueType::Int64, ValueType::Double,
            ValueType::Text,
            ValueType::Blob,
            ValueType::Date, ValueType::Timestamp,
        };
        tv.providerNativeName = {
            {ValueType::Null,      "NULL"},
            {ValueType::Bool,      "BOOLEAN"},
            {ValueType::Int32,     "INTEGER"},
            {ValueType::Int64,     {"BIGINT"}},
            {ValueType::Double,    {"DOUBLE PRECISION"}},
            {ValueType::Text,      {"TEXT"}},
            {ValueType::Blob,      {"BYTEA"}},
            {ValueType::Date,      {"DATE"}},
            {ValueType::Timestamp, {"TIMESTAMP"}},
        };
        return tv;
    }

    DbError open(const std::string& uri) override {
        if (uri != "in-memory://") {
            return DbError::internal("InMemoryStorageProvider: unexpected URI '" + uri + "'");
        }
        opened_ = true;
        return DbError::sentinel();
    }

    DbError close() override {
        const std::lock_guard<std::mutex> lk(schema_mu_);
        schemas_.clear();
        opened_ = false;
        return DbError::sentinel();
    }

    bool isOpen() const noexcept override { return opened_; }

    DbError createTable(const Schema& schema) override {
        const std::lock_guard<std::mutex> lk(schema_mu_);
        if (schemas_.find(schema.table) != schemas_.end()) {
            return DbError::internal("table '" + schema.table + "' already exists");
        }
        // Validate that every declared column type is in our TypeVocabulary.
        auto tv = typeVocabulary();
        auto isAccepted = [&](ValueType t) {
            for (auto a : tv.accepted) if (a == t) return true;
            return false;
        };
        for (const auto& col : schema.columns) {
            if (!isAccepted(col.type)) {
                return DbError::notSupported(
                    "column '" + col.name + "' type not accepted by in-memory provider");
            }
        }
        schemas_[schema.table] = schema;
        return DbError::sentinel();
    }

    DbError dropTable(const std::string& name) override {
        const std::lock_guard<std::mutex> lk(schema_mu_);
        auto it = schemas_.find(name);
        if (it == schemas_.end()) {
            return DbError::notFound("table '" + name + "' not found");
        }
        schemas_.erase(it);
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

    // Diagnostics (used by tests — delegates to the inner engine).
    std::size_t tableCount() const { return engine_->tableCount(); }
    std::size_t rowCount(const std::string& table) const { return engine_->rowCount(table); }

private:
    std::unique_ptr<InMemoryStorageEngine> engine_;
    mutable std::mutex schema_mu_;
    std::unordered_map<std::string, Schema> schemas_;
    bool opened_ = false;
};

} // namespace atomdb

#endif // ATOMDB_IN_MEMORY_STORAGE_PROVIDER_HPP
# ADR-002: FNV-1a for sharding

## Status

Accepted (Phase 4 implementation detail).

## Context

Hash-sharded tables need a deterministic, fast, non-cryptographic hash
function that maps a `Value` (Int64, Text, etc.) to a shard index. The
two main contenders are:

1. **FNV-1a** (Fowler–Noll–Vo) — simple, fast, reasonable distribution.
2. **std::hash** — already in the standard library, type-aware, but
   implementation-defined (different libstdc++ versions can hash
   `std::string` differently, breaking cross-platform data layouts).
3. **Cryptographic hashes** (SHA-256, etc.) — overkill for routing,
   slow, hard to make inline.

## Decision

We use **FNV-1a** with a 64-bit offset basis and prime. Implementation
in `ShardedStorageProvider.hpp`. Why:

- Deterministic across platforms and stdlib versions.
- A handful of bytes per Value — inline in the routing function.
- No dependency on `<functional>` (which varies across libstdc++).
- Distribution quality is good enough for shard routing where we don't
  need adversarial robustness.

## Consequences

- Stable shard placement across runs, machines, and toolchains.
- Routing is O(len-of-key-bytes), no allocation.
- Cannot be used for cryptographic / adversarial workloads; but that
  is not the design goal here.

## Implementation

```cpp
static std::uint64_t fnv1a(const std::uint8_t* data, std::size_t len) {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}
```

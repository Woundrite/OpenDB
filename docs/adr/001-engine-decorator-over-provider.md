# ADR-001: Engine decorator over provider decorator

## Status

Accepted (Phase 4 design decision).

## Context

When wrapping a back-end (e.g. for caching), there are two natural
patterns:
1. Decorate the `IStorageProvider` (whole-provider wrapper).
2. Decorate the `IStorageEngine` (just the data-path wrapper).

Both preserve the universal contract above them, but they have different
behavior in the corner cases:

- A **provider decorator** owns the schema surface AND the data path.
  Useful if you want to filter DDL or do table-level sharding where the
  schema lives on the children.
- An **engine decorator** owns only the data path. Schema lives on the
  inner provider. The decorator is invisible to DDL.

## Decision

We use **engine decorators** for read/write caching
(`CachingStorageEngine`). We use **provider decorators** for sharding
(`ShardedStorageProvider` wraps `IStorageProvider*`, not
`IStorageEngine*`).

Rationale:
- Caching shouldn't participate in schema lookups. It's strictly a
  data-path concern.
- Sharding requires the provider surface because each shard has its own
  schema, and partition policy lives at the provider level.

## Consequences

- A `CachingStorageEngine<IStorageEngine>` is a pure throughput knob.
- A `ShardedStorageProvider` is a structural choice that affects DDL
  fan-out.
- These two concerns can compose: a `ShardedStorageProvider` may
  internally wrap `CachingStorageEngine`s per shard.

#pragma once

#include "result.hpp"
#include "structure.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace water_structure {

// A move-only unit travelling through the public conversion pipeline.  The
// block data is deliberately kept in ChunkData (rather than exposing a
// format-specific representation) so a reader can hand ownership to an
// encoder without a second full-structure copy.
struct StreamChunk {
    ChunkPos position{};
    ChunkData blocks{};
    std::vector<BlockEntity> entities{};

    StreamChunk() = default;
    StreamChunk(ChunkPos chunk_position, ChunkData chunk)
        : position(chunk_position), blocks(std::move(chunk)) {}
    StreamChunk(const StreamChunk&) = delete;
    StreamChunk& operator=(const StreamChunk&) = delete;
    StreamChunk(StreamChunk&&) noexcept = default;
    StreamChunk& operator=(StreamChunk&&) noexcept = default;
};

// A sink is the commit boundary of a conversion.  Implementations must not
// retain a reference to the argument after push() returns; ownership is
// transferred to the sink by the move.  Returning an error immediately stops
// the producer and wakes any blocked queue operation.
class ChunkSink {
public:
    virtual ~ChunkSink() = default;
    virtual Result<void> push(StreamChunk&& chunk) = 0;
    virtual Result<void> finish() { return Result<void>::success(); }
    virtual void cancel() noexcept {}
};

struct ChunkStreamOptions {
    // The stream itself has one producer and one ordered sink.  worker_count
    // is reserved for producer implementations which can decode independent
    // chunks; the generic adapter remains deterministic when it is > 1.
    std::size_t worker_count = 1;
    // A zero value is replaced with a budget-derived bounded window.
    std::size_t max_in_flight_chunks = 0;
    std::size_t soft_memory_budget_bytes = 450u * 1024u * 1024u;
};

using ChunkProducer = std::function<Result<void>(ChunkSink&)>;

// Bounded producer/consumer bridge.  The producer is executed on the caller
// thread and the sink on one consumer thread.  A fixed-size queue supplies
// backpressure, so a large source cannot grow an unbounded vector while the
// destination (LevelDB/ZIP/file encoder) is slower.
class ChunkStream final {
public:
    explicit ChunkStream(ChunkProducer producer);

    ChunkStream(const ChunkStream&) = delete;
    ChunkStream& operator=(const ChunkStream&) = delete;
    ChunkStream(ChunkStream&&) noexcept = default;
    ChunkStream& operator=(ChunkStream&&) noexcept = default;
    ~ChunkStream() = default;

    Result<void> pump(ChunkSink& sink, ChunkStreamOptions options = {}) const;

    // Adapt any IStructure to the common stream.  Positions are consumed in
    // the supplied order; only one chunk and its NBT are materialized at a
    // time by readers that do not provide a native streaming override.
    static ChunkStream from_structure(
        const IStructure& structure,
        std::span<const ChunkPos> positions,
        std::size_t source_batch_size = 1);

    // Ownership-taking overload used by callers which already assembled an
    // ordered position list.  It avoids keeping a second complete copy of a
    // large world's chunk coordinates.
    static ChunkStream from_structure(
        const IStructure& structure,
        std::vector<ChunkPos> positions,
        std::size_t source_batch_size = 1);

    // Generate the canonical [0..chunk_x) × [0..chunk_z) order lazily. This
    // is the preferred overload for full-world conversion because the list of
    // all chunk coordinates never has to reside in memory.
    static ChunkStream from_structure_grid(
        const IStructure& structure,
        Size size,
        std::size_t source_batch_size = 1);

private:
    ChunkProducer mProducer;
};

} // namespace water_structure

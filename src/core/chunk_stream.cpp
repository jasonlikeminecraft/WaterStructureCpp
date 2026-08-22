#include <WaterStructure/chunk_stream.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>

namespace water_structure {
namespace {

Result<void> produce_structure_batch(
    const IStructure& structure,
    std::span<const ChunkPos> batch,
    ChunkSink& sink)
{
    auto entities = structure.get_chunk_nbt(batch);
    if (!entities) return Result<void>::failure(entities.error());
    auto visited = structure.visit_chunks(batch,
        [&](ChunkPos position, const ChunkData& chunk) -> Result<void> {
            StreamChunk item(position, chunk);
            if (auto entity_it = entities.value().find(position);
                entity_it != entities.value().end()) {
                item.entities = std::move(entity_it->second);
            }
            return sink.push(std::move(item));
        });
    structure.release_cached_chunks();
    return visited;
}

class QueueSink final : public ChunkSink {
public:
    QueueSink(std::size_t capacity, ChunkSink& downstream)
        : mCapacity(std::max<std::size_t>(capacity, 1)), mDownstream(downstream) {}

    Result<void> push(StreamChunk&& chunk) override
    {
        std::unique_lock lock(mMutex);
        mSpace.wait(lock, [this] {
            return mClosed || mError.has_value() || mQueue.size() < mCapacity;
        });
        if (mError) return Result<void>::failure(*mError);
        if (mClosed) return Result<void>::failure("chunk stream 已取消");
        mQueue.emplace_back(std::move(chunk));
        lock.unlock();
        mReady.notify_one();
        return Result<void>::success();
    }

    void close() noexcept
    {
        {
            const std::scoped_lock lock(mMutex);
            mClosed = true;
        }
        mReady.notify_all();
        mSpace.notify_all();
    }

    void fail(std::string error) noexcept
    {
        {
            const std::scoped_lock lock(mMutex);
            if (!mError) mError = std::move(error);
            mClosed = true;
            mQueue.clear();
        }
        mReady.notify_all();
        mSpace.notify_all();
        mDownstream.cancel();
    }

    Result<void> consume()
    {
        while (true) {
            StreamChunk chunk;
            {
                std::unique_lock lock(mMutex);
                mReady.wait(lock, [this] { return mClosed || !mQueue.empty(); });
                if (mQueue.empty()) {
                    if (mError) return Result<void>::failure(*mError);
                    if (mClosed) return Result<void>::success();
                    continue;
                }
                chunk = std::move(mQueue.front());
                mQueue.pop_front();
            }
            mSpace.notify_one();
            auto result = mDownstream.push(std::move(chunk));
            if (!result) {
                fail(result.error());
                return result;
            }
        }
    }

    std::optional<std::string> error() const
    {
        const std::scoped_lock lock(mMutex);
        return mError;
    }

private:
    const std::size_t mCapacity;
    ChunkSink& mDownstream;
    mutable std::mutex mMutex;
    std::condition_variable mReady;
    std::condition_variable mSpace;
    std::deque<StreamChunk> mQueue;
    std::optional<std::string> mError;
    bool mClosed = false;
};

std::size_t stream_capacity(const ChunkStreamOptions& options)
{
    // A ChunkData can contain two 16^3 layers plus container overhead.  Keep
    // the estimate conservative; this is a soft bound and never replaces a
    // process-level hard limit.
    constexpr std::size_t kBytesPerChunk = 2u * 1024u * 1024u;
    const auto budget = std::max<std::size_t>(options.soft_memory_budget_bytes, kBytesPerChunk);
    const auto budget_capacity = std::clamp<std::size_t>(
        budget / kBytesPerChunk, 1, 128);
    if (options.max_in_flight_chunks == 0) return budget_capacity;
    // An explicit window is still subordinate to the memory budget.  This is
    // important for very wide Schem rows where the natural source batch may
    // contain hundreds or thousands of chunks.
    return std::min(
        std::clamp<std::size_t>(options.max_in_flight_chunks, 1, 4096),
        budget_capacity);
}

} // namespace

ChunkStream::ChunkStream(ChunkProducer producer)
    : mProducer(std::move(producer)) {}

Result<void> ChunkStream::pump(ChunkSink& sink, ChunkStreamOptions options) const
{
    if (!mProducer) return Result<void>::failure("chunk stream producer 为空");
    QueueSink queue(stream_capacity(options), sink);
    Result<void> consumer_result = Result<void>::success();
    std::thread consumer([&] {
        consumer_result = queue.consume();
    });

    Result<void> producer_result = Result<void>::success();
    try {
        producer_result = mProducer(queue);
    } catch (const std::exception& error) {
        producer_result = Result<void>::failure(
            std::string("chunk stream producer exception: ") + error.what());
    } catch (...) {
        producer_result = Result<void>::failure("chunk stream producer exception");
    }
    if (!producer_result) queue.fail(producer_result.error());
    queue.close();
    consumer.join();

    if (!producer_result) return producer_result;
    if (!consumer_result) return consumer_result;
    if (const auto error = queue.error()) return Result<void>::failure(*error);
    return sink.finish();
}

ChunkStream ChunkStream::from_structure(
    const IStructure& structure,
    std::span<const ChunkPos> positions,
    std::size_t source_batch_size)
{
    // Copy positions into the producer capture.  This makes the returned
    // stream safe even when the caller's temporary span goes out of scope.
    std::vector<ChunkPos> owned(positions.begin(), positions.end());
    return from_structure(structure, std::move(owned), source_batch_size);
}

ChunkStream ChunkStream::from_structure(
    const IStructure& structure,
    std::vector<ChunkPos> positions,
    std::size_t source_batch_size)
{
    source_batch_size = std::max<std::size_t>(source_batch_size, 1);
    return ChunkStream([
        &structure,
        positions = std::move(positions),
        source_batch_size](ChunkSink& sink) {
        for (std::size_t begin = 0; begin < positions.size(); begin += source_batch_size) {
            const auto count = std::min(source_batch_size, positions.size() - begin);
            const auto batch = std::span<const ChunkPos>(positions).subspan(begin, count);

            auto visited = produce_structure_batch(structure, batch, sink);
            if (!visited) return visited;
        }
        return Result<void>::success();
    });
}

ChunkStream ChunkStream::from_structure_grid(
    const IStructure& structure,
    Size size,
    std::size_t source_batch_size)
{
    source_batch_size = std::max<std::size_t>(source_batch_size, 1);
    return ChunkStream([&structure, size, source_batch_size](ChunkSink& sink) {
        std::vector<ChunkPos> batch;
        batch.reserve(source_batch_size);
        const auto x_count = std::max<std::int32_t>(0, size.chunk_x_count());
        const auto z_count = std::max<std::int32_t>(0, size.chunk_z_count());
        for (std::int32_t z = 0; z < z_count; ++z) {
            for (std::int32_t x = 0; x < x_count; ++x) {
                batch.push_back({ x, z });
                if (batch.size() < source_batch_size) continue;
                auto visited = produce_structure_batch(structure, batch, sink);
                batch.clear();
                if (!visited) return visited;
            }
        }
        if (!batch.empty()) return produce_structure_batch(structure, batch, sink);
        return Result<void>::success();
    });
}

} // namespace water_structure

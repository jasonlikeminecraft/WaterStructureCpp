#define NOMINMAX
#include <Windows.h>

#include <BedrockWorldOperator/BedrockWorldOperator.hpp>

#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

constexpr uint32_t kFnvInit = 0x811C9DC5u;
constexpr uint32_t kFnvPrime = 0x01000193u;
constexpr uint32_t kCurrentBlockVersion = 18168865u;
constexpr uint8_t kChunkVersion = 40;
constexpr uint32_t kFinalisationGenerated = 2;
constexpr std::size_t kDiskStateRuntimeCacheLimit = 8 * 1024 * 1024;
constexpr std::size_t kDiskStateRuntimeCacheEntryLimit = 16 * 1024;
constexpr std::size_t kDiskStateRuntimeCacheEntrySizeLimit = 64 * 1024;
constexpr std::size_t kEncodedDiskStateCacheLimit = 8 * 1024 * 1024;
constexpr std::size_t kEncodedDiskStateCacheEntryLimit = 16 * 1024;

struct DecodeProfileState {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> sampledCalls{0};
    std::atomic<std::uint64_t> sampledLayers{0};
    std::atomic<std::uint64_t> sampledPaletteEntries{0};
    std::atomic<std::uint64_t> payloadCopyNs{0};
    std::atomic<std::uint64_t> nativeInitNs{0};
    std::atomic<std::uint64_t> packedReadNs{0};
    std::atomic<std::uint64_t> paletteResolveNs{0};
    std::atomic<std::uint64_t> blockExpandNs{0};
    std::atomic<std::uint64_t> setBlocksNs{0};
    std::atomic<std::uint64_t> wrapperNs{0};
};

DecodeProfileState gDecodeProfile;

struct EncodeProfileState {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> sampledCalls{0};
    std::atomic<std::uint64_t> sampledLayers{0};
    std::atomic<std::uint64_t> sampledPaletteEntries{0};
    std::atomic<std::uint64_t> paletteBuildNs{0};
    std::atomic<std::uint64_t> indexPackNs{0};
    std::atomic<std::uint64_t> packedWriteNs{0};
    std::atomic<std::uint64_t> paletteWriteNs{0};
};

EncodeProfileState gEncodeProfile;

bool decodeProfileEnabled()
{
    static const bool enabled = std::getenv("WATER_STRUCTURE_PROFILE_DETAIL") != nullptr;
    return enabled;
}

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point start)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
}

enum class TagType : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    String = 8,
    Compound = 10,
};

struct StateValue {
    TagType type = TagType::End;
    std::string stringValue;
    int64_t intValue = 0;
    int8_t byteValue = 0;
};

using StateMap = std::map<std::string, StateValue>;

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

void writeLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}

void writeLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void writeLe64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
    }
}

char* allocCString(const std::string& s) {
    auto* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (!p) {
        return nullptr;
    }
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

char* allocSlice(const std::vector<uint8_t>& payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    auto* p = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(len) + 4));
    if (!p) {
        return nullptr;
    }
    p[0] = static_cast<uint8_t>(len & 0xff);
    p[1] = static_cast<uint8_t>((len >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((len >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((len >> 24) & 0xff);
    if (len != 0) {
        std::memcpy(p + 4, payload.data(), len);
    }
    return reinterpret_cast<char*>(p);
}

char* allocSlice(std::initializer_list<uint8_t> payload) {
    return allocSlice(std::vector<uint8_t>(payload));
}

char* allocSlice(const std::string& payload) {
    return allocSlice(std::vector<uint8_t>(payload.begin(), payload.end()));
}

std::vector<uint8_t> readSlice(const char* p) {
    if (!p) {
        return {};
    }
    const auto* b = reinterpret_cast<const uint8_t*>(p);
    const uint32_t len = readLe32(b);
    return std::vector<uint8_t>(b + 4, b + 4 + len);
}

std::string readSliceString(const char* p) {
    auto v = readSlice(p);
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : mData(data), mSize(size) {}
    explicit ByteReader(const std::vector<uint8_t>& data) : ByteReader(data.data(), data.size()) {}

    bool canRead(size_t n) const { return mPos <= mSize && n <= mSize - mPos; }
    bool empty() const { return mPos >= mSize; }
    size_t pos() const { return mPos; }
    const uint8_t* current() const { return mData + mPos; }

    uint8_t u8() {
        if (!canRead(1)) {
            throw std::runtime_error("unexpected end of buffer");
        }
        return mData[mPos++];
    }

    uint16_t le16() {
        if (!canRead(2)) {
            throw std::runtime_error("unexpected end of buffer");
        }
        uint16_t v = readLe16(mData + mPos);
        mPos += 2;
        return v;
    }

    uint32_t le32() {
        if (!canRead(4)) {
            throw std::runtime_error("unexpected end of buffer");
        }
        uint32_t v = readLe32(mData + mPos);
        mPos += 4;
        return v;
    }

    uint64_t le64() {
        if (!canRead(8)) {
            throw std::runtime_error("unexpected end of buffer");
        }
        uint64_t v = readLe64(mData + mPos);
        mPos += 8;
        return v;
    }

    int32_t varInt32() {
        uint32_t result = 0;
        int shift = 0;
        while (shift < 35) {
            const uint8_t byte = u8();
            result |= static_cast<uint32_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                return static_cast<int32_t>(result);
            }
            shift += 7;
        }
        throw std::runtime_error("varint32 too large");
    }

    uint32_t varUInt32() {
        return static_cast<uint32_t>(varInt32());
    }

    std::string string() {
        const uint32_t len = varUInt32();
        if (!canRead(len)) {
            throw std::runtime_error("string out of range");
        }
        std::string s(reinterpret_cast<const char*>(mData + mPos), len);
        mPos += len;
        return s;
    }

    std::vector<uint8_t> bytes(size_t n) {
        if (!canRead(n)) {
            throw std::runtime_error("bytes out of range");
        }
        std::vector<uint8_t> out(mData + mPos, mData + mPos + n);
        mPos += n;
        return out;
    }

    std::vector<uint8_t> byteSlice() {
        return bytes(varUInt32());
    }

    void skip(size_t n) {
        if (!canRead(n)) {
            throw std::runtime_error("skip out of range");
        }
        mPos += n;
    }

private:
    const uint8_t* mData = nullptr;
    size_t mSize = 0;
    size_t mPos = 0;
};

void writeVarUInt32(std::vector<uint8_t>& out, uint32_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

void writeVarInt32(std::vector<uint8_t>& out, int32_t value) {
    uint32_t v = static_cast<uint32_t>(value);
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

void writeNbtName(std::vector<uint8_t>& out, const std::string& name) {
    writeLe16(out, static_cast<uint16_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
}

void writeNbtStringPayload(std::vector<uint8_t>& out, const std::string& value) {
    writeLe16(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void writeNbtEntry(std::vector<uint8_t>& out, TagType type, const std::string& name, const StateValue& value) {
    out.push_back(static_cast<uint8_t>(type));
    writeNbtName(out, name);
    switch (type) {
    case TagType::Byte:
        out.push_back(static_cast<uint8_t>(value.byteValue));
        break;
    case TagType::Short:
        writeLe16(out, static_cast<uint16_t>(value.intValue));
        break;
    case TagType::Int:
        writeLe32(out, static_cast<uint32_t>(value.intValue));
        break;
    case TagType::Long:
        writeLe64(out, static_cast<uint64_t>(value.intValue));
        break;
    case TagType::String:
        writeNbtStringPayload(out, value.stringValue);
        break;
    default:
        break;
    }
}

std::vector<uint8_t> encodeStatePayload(const StateMap& states) {
    std::vector<uint8_t> out;
    for (const auto& [key, value] : states) {
        writeNbtEntry(out, value.type, key, value);
    }
    out.push_back(0);
    return out;
}

std::vector<uint8_t> encodeRootCompound(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(TagType::Compound));
    writeLe16(out, 0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::string readNbtName(ByteReader& r) {
    const uint16_t len = r.le16();
    auto bytes = r.bytes(len);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

StateValue readNbtValue(ByteReader& r, TagType type) {
    StateValue value;
    value.type = type;
    switch (type) {
    case TagType::Byte:
        value.byteValue = static_cast<int8_t>(r.u8());
        break;
    case TagType::Short:
        value.intValue = static_cast<int16_t>(r.le16());
        break;
    case TagType::Int:
        value.intValue = static_cast<int32_t>(r.le32());
        break;
    case TagType::Long:
        value.intValue = static_cast<int64_t>(r.le64());
        break;
    case TagType::String: {
        const uint16_t len = r.le16();
        auto bytes = r.bytes(len);
        value.stringValue.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        break;
    }
    default:
        throw std::runtime_error("unsupported nbt value type");
    }
    return value;
}

void skipNbtValue(ByteReader& r, TagType type) {
    switch (type) {
    case TagType::Byte:
        r.skip(1);
        return;
    case TagType::Short:
        r.skip(2);
        return;
    case TagType::Int:
        r.skip(4);
        return;
    case TagType::Long:
        r.skip(8);
        return;
    case TagType::String:
        r.skip(r.le16());
        return;
    case TagType::Compound:
        while (true) {
            const auto childType = static_cast<TagType>(r.u8());
            if (childType == TagType::End) return;
            r.skip(r.le16());
            skipNbtValue(r, childType);
        }
    default:
        throw std::runtime_error("unsupported nbt value type");
    }
}

void skipDiskBlockState(ByteReader& r) {
    const auto rootType = static_cast<TagType>(r.u8());
    if (rootType != TagType::Compound) {
        throw std::runtime_error("block state root is not compound");
    }
    r.skip(r.le16());
    skipNbtValue(r, TagType::Compound);
}

StateMap parseStateCompoundPayload(ByteReader& r) {
    StateMap states;
    while (!r.empty()) {
        const auto type = static_cast<TagType>(r.u8());
        if (type == TagType::End) {
            break;
        }
        const std::string name = readNbtName(r);
        if (type == TagType::Compound) {
            while (true) {
                const auto childType = static_cast<TagType>(r.u8());
                if (childType == TagType::End) {
                    break;
                }
                (void)readNbtName(r);
                (void)readNbtValue(r, childType);
            }
            continue;
        }
        states[name] = readNbtValue(r, type);
    }
    return states;
}

struct DiskBlockState {
    std::string name;
    StateMap states;
    int32_t version = 0;
};

DiskBlockState parseDiskBlockState(ByteReader& r) {
    DiskBlockState result;
    const auto rootType = static_cast<TagType>(r.u8());
    if (rootType != TagType::Compound) {
        throw std::runtime_error("block state root is not compound");
    }
    (void)readNbtName(r);
    while (true) {
        const auto type = static_cast<TagType>(r.u8());
        if (type == TagType::End) {
            break;
        }
        const std::string key = readNbtName(r);
        if (key == "name" && type == TagType::String) {
            result.name = readNbtValue(r, type).stringValue;
        } else if (key == "version" && (type == TagType::Int || type == TagType::Short || type == TagType::Long)) {
            result.version = readNbtValue(r, type).intValue;
        } else if (key == "states" && type == TagType::Compound) {
            result.states = parseStateCompoundPayload(r);
        } else {
            (void)readNbtValue(r, type);
        }
    }
    return result;
}

std::vector<uint8_t> encodeDiskBlockState(const std::string& name, const StateMap& states, int32_t blockVersion = static_cast<int32_t>(kCurrentBlockVersion)) {
    std::vector<uint8_t> payload;
    StateValue nameValue;
    nameValue.type = TagType::String;
    nameValue.stringValue = name;
    writeNbtEntry(payload, TagType::String, "name", nameValue);

    payload.push_back(static_cast<uint8_t>(TagType::Compound));
    writeNbtName(payload, "states");
    auto statePayload = encodeStatePayload(states);
    payload.insert(payload.end(), statePayload.begin(), statePayload.end());

    StateValue version;
    version.type = TagType::Int;
    version.intValue = blockVersion == 0 ? static_cast<int32_t>(kCurrentBlockVersion) : blockVersion;
    writeNbtEntry(payload, TagType::Int, "version", version);
    payload.push_back(0);
    return encodeRootCompound(payload);
}

uint32_t fnv1a(const std::vector<uint8_t>& data) {
    uint32_t hash = kFnvInit;
    for (uint8_t b : data) {
        hash ^= b;
        hash *= kFnvPrime;
    }
    return hash;
}

std::vector<uint8_t> marshalInternalData(const std::string& key, const StateValue& value) {
    std::vector<uint8_t> out;
    writeNbtEntry(out, value.type, key, value);
    return out;
}

uint32_t computeBlockHash(std::string name, const StateMap& states) {
    if (name == "unknown" || name == "minecraft:unknown") {
        return static_cast<uint32_t>(-2);
    }
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(TagType::Compound));
    writeLe16(out, 0);

    StateValue nameValue;
    nameValue.type = TagType::String;
    nameValue.stringValue = std::move(name);
    auto nameBytes = marshalInternalData("name", nameValue);
    out.insert(out.end(), nameBytes.begin(), nameBytes.end());

    out.push_back(static_cast<uint8_t>(TagType::Compound));
    writeNbtName(out, "states");
    auto statesBytes = encodeStatePayload(states);
    out.insert(out.end(), statesBytes.begin(), statesBytes.end());
    out.push_back(0);
    return fnv1a(out);
}

class BlockStateRegistry {
public:
    struct Entry {
        std::string name;
        StateMap states;
        int32_t version = 0;
        uint32_t runtimeId = 0;
    };

    static BlockStateRegistry& instance() {
        static BlockStateRegistry registry;
        return registry;
    }

    void setResolver(BedrockWorldOperator::BlockRuntimeResolver resolver) {
        std::lock_guard lock(mMutex);
        mResolver = std::move(resolver);
        mRuntimeCache.clear();
        mDiskStateRuntimeCache.clear();
        mDiskStateRuntimeCacheBytes = 0;
        mGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t generation() const noexcept {
        return mGeneration.load(std::memory_order_relaxed);
    }

    uint32_t airRuntimeId() {
        std::lock_guard lock(mMutex);
        if (mResolver.airRuntimeId) {
            const uint32_t id = mResolver.airRuntimeId();
            return id;
        }
        if (mResolver.nameToRuntimeId) {
            auto id = mResolver.nameToRuntimeId("minecraft:air");
            if (id) {
                return *id;
            }
        }
        return 0;
    }

    const Entry* byRuntime(uint32_t runtimeId) {
        std::lock_guard lock(mMutex);
        auto it = mRuntimeCache.find(runtimeId);
        if (it != mRuntimeCache.end()) {
            return &it->second;
        }
        if (!mResolver.runtimeIdToName) {
            return nullptr;
        }

        if (mResolver.runtimeIdToState) {
            auto resolvedState = mResolver.runtimeIdToState(runtimeId);
            if (resolvedState && !resolvedState->name.empty()) {
                Entry entry;
                entry.name = std::move(resolvedState->name);
                if (entry.name.rfind("minecraft:", 0) != 0) {
                    entry.name = "minecraft:" + entry.name;
                }
                entry.version = resolvedState->version == 0
                    ? static_cast<int32_t>(kCurrentBlockVersion)
                    : resolvedState->version;
                entry.runtimeId = runtimeId;
                for (const auto& property : resolvedState->states) {
                    StateValue value;
                    switch (property.type) {
                    case BedrockWorldOperator::BlockStateValueType::Byte:
                        value.type = TagType::Byte;
                        value.byteValue = static_cast<int8_t>(property.intValue);
                        break;
                    case BedrockWorldOperator::BlockStateValueType::Short:
                        value.type = TagType::Short;
                        value.intValue = static_cast<int16_t>(property.intValue);
                        break;
                    case BedrockWorldOperator::BlockStateValueType::Long:
                        value.type = TagType::Long;
                        value.intValue = property.intValue;
                        break;
                    case BedrockWorldOperator::BlockStateValueType::String:
                        value.type = TagType::String;
                        value.stringValue = property.stringValue;
                        break;
                    case BedrockWorldOperator::BlockStateValueType::Int:
                    default:
                        value.type = TagType::Int;
                        value.intValue = static_cast<int32_t>(property.intValue);
                        break;
                    }
                    if (value.type != TagType::End) {
                        entry.states[property.name] = std::move(value);
                    }
                }
                auto [inserted, _] = mRuntimeCache.emplace(runtimeId, std::move(entry));
                return &inserted->second;
            }
        }

        auto resolvedName = mResolver.runtimeIdToName(runtimeId);
        if (!resolvedName || resolvedName->empty()) {
            return nullptr;
        }

        Entry entry;
        entry.name = *resolvedName;
        if (entry.name.rfind("minecraft:", 0) != 0) {
            entry.name = "minecraft:" + entry.name;
        }
        entry.version = static_cast<int32_t>(kCurrentBlockVersion);
        entry.runtimeId = runtimeId;
        auto [inserted, _] = mRuntimeCache.emplace(runtimeId, std::move(entry));
        return &inserted->second;
    }

    std::optional<uint32_t> toRuntime(
        std::string name,
        const StateMap& states,
        int32_t version = 0,
        std::string_view encodedDiskState = {}) {
        std::lock_guard lock(mMutex);
        if (!encodedDiskState.empty()) {
            const auto cached = mDiskStateRuntimeCache.find(encodedDiskState);
            if (cached != mDiskStateRuntimeCache.end()) {
                return cached->second;
            }
        }
        if (name.rfind("minecraft:", 0) != 0) {
            name = "minecraft:" + name;
        }

        std::optional<uint32_t> runtime;
        if (mResolver.stateToRuntimeId) {
            BedrockWorldOperator::BlockState state;
            state.name = name;
            state.version = version;
            state.states.reserve(states.size());
            for (const auto& [propertyName, source] : states) {
                BedrockWorldOperator::BlockStateProperty property;
                property.name = propertyName;
                switch (source.type) {
                case TagType::Byte:
                    property.type = BedrockWorldOperator::BlockStateValueType::Byte;
                    property.intValue = source.byteValue;
                    break;
                case TagType::Short:
                    property.type = BedrockWorldOperator::BlockStateValueType::Short;
                    property.intValue = static_cast<int16_t>(source.intValue);
                    break;
                case TagType::Int:
                    property.type = BedrockWorldOperator::BlockStateValueType::Int;
                    property.intValue = static_cast<int32_t>(source.intValue);
                    break;
                case TagType::Long:
                    property.type = BedrockWorldOperator::BlockStateValueType::Long;
                    property.intValue = source.intValue;
                    break;
                case TagType::String:
                    property.type = BedrockWorldOperator::BlockStateValueType::String;
                    property.stringValue = source.stringValue;
                    break;
                default:
                    continue;
                }
                state.states.push_back(std::move(property));
            }
            runtime = mResolver.stateToRuntimeId(state);
        }

        if (!runtime && mResolver.nameToRuntimeId) {
            runtime = mResolver.nameToRuntimeId(name);
        }
        if (runtime && !encodedDiskState.empty() &&
            mDiskStateRuntimeCache.size() < kDiskStateRuntimeCacheEntryLimit &&
            encodedDiskState.size() <= kDiskStateRuntimeCacheEntrySizeLimit &&
            encodedDiskState.size() <= kDiskStateRuntimeCacheLimit -
                std::min(kDiskStateRuntimeCacheLimit, mDiskStateRuntimeCacheBytes)) {
            auto [inserted, added] = mDiskStateRuntimeCache.emplace(
                std::string(encodedDiskState), *runtime);
            if (added) {
                mDiskStateRuntimeCacheBytes += inserted->first.size();
            }
        }
        return runtime;
    }

    std::optional<uint32_t> cachedDiskRuntime(std::string_view encodedDiskState) {
        std::lock_guard lock(mMutex);
        const auto cached = mDiskStateRuntimeCache.find(encodedDiskState);
        return cached == mDiskStateRuntimeCache.end()
            ? std::nullopt
            : std::optional<uint32_t>(cached->second);
    }

private:
    struct TransparentStringHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }

        std::size_t operator()(const std::string& value) const noexcept {
            return (*this)(std::string_view(value));
        }
    };

    std::mutex mMutex;
    BedrockWorldOperator::BlockRuntimeResolver mResolver;
    std::unordered_map<uint32_t, Entry> mRuntimeCache;
    std::unordered_map<
        std::string,
        uint32_t,
        TransparentStringHash,
        std::equal_to<>> mDiskStateRuntimeCache;
    std::size_t mDiskStateRuntimeCacheBytes = 0;
    std::atomic<std::uint64_t> mGeneration{1};
};

int rangeStartForDim(int dm) {
    return dm == 0 ? -64 : 0;
}

int rangeEndForDim(int dm) {
    if (dm == 1) {
        return 127;
    }
    if (dm == 2) {
        return 255;
    }
    return 319;
}

std::vector<uint8_t> makeChunkKey(int dm, int x, int z, const std::vector<uint8_t>& suffix = {}) {
    std::vector<uint8_t> key;
    writeLe32(key, static_cast<uint32_t>(x));
    writeLe32(key, static_cast<uint32_t>(z));
    if (dm != 0) {
        writeLe32(key, static_cast<uint32_t>(dm));
    }
    key.insert(key.end(), suffix.begin(), suffix.end());
    return key;
}

class NativeSubChunk {
public:
    NativeSubChunk() {
        mLayers.push_back(std::vector<uint32_t>(4096, BlockStateRegistry::instance().airRuntimeId()));
    }

    uint32_t block(int x, int y, int z, int layer) const {
        if (!valid(x, y, z) || layer < 0 || static_cast<size_t>(layer) >= mLayers.size()) {
            return BlockStateRegistry::instance().airRuntimeId();
        }
        return mLayers[static_cast<size_t>(layer)][index(x, y, z)];
    }

    void setBlock(int x, int y, int z, int layer, uint32_t runtimeId) {
        if (!valid(x, y, z) || layer < 0) {
            return;
        }
        ensureLayer(static_cast<size_t>(layer));
        mLayers[static_cast<size_t>(layer)][index(x, y, z)] = runtimeId;
    }

    std::vector<uint32_t> blocks(int layer) const {
        if (layer < 0 || static_cast<size_t>(layer) >= mLayers.size()) {
            return std::vector<uint32_t>(4096, BlockStateRegistry::instance().airRuntimeId());
        }
        return mLayers[static_cast<size_t>(layer)];
    }

    std::span<const uint32_t> blocksView(int layer) const noexcept {
        if (layer < 0 || static_cast<size_t>(layer) >= mLayers.size()) {
            return {};
        }
        return mLayers[static_cast<size_t>(layer)];
    }

    void setBlocks(int layer, std::vector<uint32_t> blocks) {
        if (layer < 0) {
            return;
        }
        ensureLayer(static_cast<size_t>(layer));
        blocks.resize(4096, BlockStateRegistry::instance().airRuntimeId());
        mLayers[static_cast<size_t>(layer)] = std::move(blocks);
    }

    bool empty() const {
        const uint32_t air = BlockStateRegistry::instance().airRuntimeId();
        for (const auto& layer : mLayers) {
            for (uint32_t v : layer) {
                if (v != air) {
                    return false;
                }
            }
        }
        return true;
    }

    size_t layerCount() const {
        size_t count = mLayers.size();
        const uint32_t air = BlockStateRegistry::instance().airRuntimeId();
        while (count > 1) {
            const auto& layer = mLayers[count - 1];
            if (std::all_of(layer.begin(), layer.end(), [air](uint32_t v) { return v == air; })) {
                --count;
            } else {
                break;
            }
        }
        return count;
    }

private:
    static bool valid(int x, int y, int z) {
        return x >= 0 && x < 16 && y >= 0 && y < 16 && z >= 0 && z < 16;
    }

    static size_t index(int x, int y, int z) {
        return static_cast<size_t>(x) * 256 + static_cast<size_t>(y) * 16 + static_cast<size_t>(z);
    }

    void ensureLayer(size_t layer) {
        while (mLayers.size() <= layer) {
            mLayers.push_back(std::vector<uint32_t>(4096, BlockStateRegistry::instance().airRuntimeId()));
        }
    }

    std::vector<std::vector<uint32_t>> mLayers;
};

template <typename T>
class ObjectStore {
public:
    long long add(std::shared_ptr<T> value) {
        std::lock_guard lock(mMutex);
        const long long id = mNext++;
        mObjects[id] = std::move(value);
        return id;
    }

    std::shared_ptr<T> get(long long id) {
        std::lock_guard lock(mMutex);
        auto it = mObjects.find(id);
        return it == mObjects.end() ? nullptr : it->second;
    }

    void release(long long id) {
        std::lock_guard lock(mMutex);
        mObjects.erase(id);
    }

private:
    std::mutex mMutex;
    long long mNext = 1;
    std::unordered_map<long long, std::shared_ptr<T>> mObjects;
};

ObjectStore<NativeSubChunk> gSubChunks;

struct NativeChunk {
    int rangeStart = -64;
    int rangeEnd = 319;
    std::vector<std::shared_ptr<NativeSubChunk>> subChunks;
    std::vector<std::vector<uint32_t>> biomes;

    NativeChunk(int start, int end) : rangeStart(start), rangeEnd(end) {
        const int count = ((rangeEnd - rangeStart + 1) >> 4);
        subChunks.resize(std::max(0, count));
        biomes.resize(std::max(0, count), std::vector<uint32_t>(4096, 0));
    }
};

ObjectStore<NativeChunk> gChunks;

uint8_t paletteSizeFor(size_t count) {
    if (count <= 1) return 0;
    if (count <= 2) return 1;
    if (count <= 4) return 2;
    if (count <= 8) return 3;
    if (count <= 16) return 4;
    if (count <= 32) return 5;
    if (count <= 64) return 6;
    if (count <= 256) return 8;
    return 16;
}

bool isValidPaletteBits(uint8_t bits) {
    switch (bits) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 8:
    case 16:
        return true;
    default:
        return false;
    }
}

size_t packedUint32Count(uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    if (!isValidPaletteBits(bits)) {
        throw std::runtime_error("invalid palette bit width");
    }
    const size_t per = 32 / bits;
    size_t count = 4096 / per;
    if (bits == 3 || bits == 5 || bits == 6) {
        ++count;
    }
    return count;
}

size_t filledBitsPerPackedWord(uint8_t bits) {
    if (bits == 0) {
        return 0;
    }
    return (32 / bits) * bits;
}

void packIndices(
    std::span<const std::uint16_t> blockPaletteIndices,
    uint8_t bits,
    std::vector<std::uint32_t>& packed) {
    packed.assign(packedUint32Count(bits), 0);
    if (bits == 0) {
        return;
    }
    const std::size_t valuesPerWord = 32u / bits;
    std::size_t storageIndex = 0;
    for (auto& word : packed) {
        for (std::size_t slot = 0;
             slot < valuesPerWord && storageIndex < blockPaletteIndices.size();
             ++slot, ++storageIndex) {
            const auto blockIndex =
                (storageIndex & 0xf00u) |
                ((storageIndex & 0x00fu) << 4u) |
                ((storageIndex & 0x0f0u) >> 4u);
            word |= static_cast<std::uint32_t>(blockPaletteIndices[blockIndex]) <<
                (slot * bits);
        }
    }
}

class PaletteIndexTable {
public:
    PaletteIndexTable()
        : mKeys(kCapacity), mValues(kCapacity), mGenerations(kCapacity) {}

    void reset()
    {
        if (++mGeneration == 0) {
            std::fill(mGenerations.begin(), mGenerations.end(), 0);
            mGeneration = 1;
        }
    }

    std::pair<std::uint16_t, bool> findOrInsert(
        std::uint32_t key,
        std::uint16_t value)
    {
        auto slot = hash(key) & (kCapacity - 1);
        while (mGenerations[slot] == mGeneration) {
            if (mKeys[slot] == key) return { mValues[slot], false };
            slot = (slot + 1) & (kCapacity - 1);
        }
        mGenerations[slot] = mGeneration;
        mKeys[slot] = key;
        mValues[slot] = value;
        return { value, true };
    }

private:
    static constexpr std::size_t kCapacity = 8192;

    static std::size_t hash(std::uint32_t value) noexcept
    {
        value ^= value >> 16u;
        value *= 0x7feb352du;
        value ^= value >> 15u;
        value *= 0x846ca68bu;
        value ^= value >> 16u;
        return value;
    }

    std::vector<std::uint32_t> mKeys;
    std::vector<std::uint16_t> mValues;
    std::vector<std::uint32_t> mGenerations;
    std::uint32_t mGeneration = 0;
};

uint32_t unpackIndex(const std::vector<uint32_t>& packed, uint8_t bits, int x, int y, int z) {
    if (bits == 0) {
        return 0;
    }
    if (!isValidPaletteBits(bits)) {
        throw std::runtime_error("invalid palette bit width");
    }
    const size_t storageIndex = static_cast<size_t>(x) * 256 + static_cast<size_t>(z) * 16 + static_cast<size_t>(y);
    const size_t bitOffset = storageIndex * bits;
    const size_t filledBits = filledBitsPerPackedWord(bits);
    const size_t word = bitOffset / filledBits;
    if (word >= packed.size()) {
        throw std::runtime_error("packed block index out of range");
    }
    const uint8_t shift = static_cast<uint8_t>(bitOffset % filledBits);
    const uint32_t mask = (1u << bits) - 1u;
    return (packed[word] >> shift) & mask;
}

BedrockWorldOperator::BlockStateProperty toPublicProperty(const std::string& name, const StateValue& value) {
    BedrockWorldOperator::BlockStateProperty property;
    property.name = name;
    switch (value.type) {
    case TagType::Byte:
        property.type = BedrockWorldOperator::BlockStateValueType::Byte;
        property.intValue = value.byteValue;
        break;
    case TagType::Short:
        property.type = BedrockWorldOperator::BlockStateValueType::Short;
        property.intValue = value.intValue;
        break;
    case TagType::Int:
        property.type = BedrockWorldOperator::BlockStateValueType::Int;
        property.intValue = value.intValue;
        break;
    case TagType::Long:
        property.type = BedrockWorldOperator::BlockStateValueType::Long;
        property.intValue = value.intValue;
        break;
    case TagType::String:
        property.type = BedrockWorldOperator::BlockStateValueType::String;
        property.stringValue = value.stringValue;
        break;
    default:
        property.type = BedrockWorldOperator::BlockStateValueType::Int;
        property.intValue = value.intValue;
        break;
    }
    return property;
}

BedrockWorldOperator::BlockState toPublicBlockState(const DiskBlockState& state) {
    BedrockWorldOperator::BlockState out;
    out.name = state.name.empty() ? "minecraft:air" : state.name;
    out.version = state.version;
    out.states.reserve(state.states.size());
    for (const auto& [key, value] : state.states) {
        out.states.push_back(toPublicProperty(key, value));
    }
    return out;
}

BedrockWorldOperator::BlockStateList decodeDiskBlockStates(ByteReader& r) {
    uint8_t bits = r.u8();
    bits >>= 1;
    if (bits == 0x7f) {
        return {};
    }
    if (!isValidPaletteBits(bits)) {
        throw std::runtime_error("invalid palette bit width");
    }
    std::vector<uint32_t> packed;
    packed.reserve(packedUint32Count(bits));
    for (size_t i = 0; i < packedUint32Count(bits); ++i) {
        packed.push_back(r.le32());
    }

    uint32_t paletteCount = 1;
    if (bits != 0) {
        const int32_t decodedPaletteCount = static_cast<int32_t>(r.le32());
        if (decodedPaletteCount <= 0) {
            throw std::runtime_error("empty palette");
        }
        paletteCount = static_cast<uint32_t>(decodedPaletteCount);
    }
    if (paletteCount == 0 || paletteCount > 4096) {
        throw std::runtime_error("invalid palette size");
    }

    std::vector<BedrockWorldOperator::BlockState> palette;
    palette.reserve(paletteCount);
    for (uint32_t i = 0; i < paletteCount; ++i) {
        auto entry = toPublicBlockState(parseDiskBlockState(r));
        entry.paletteIndex = static_cast<std::uint16_t>(i);
        palette.push_back(std::move(entry));
    }

    BedrockWorldOperator::BlockStateList blocks(4096, palette.front());
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                const uint32_t pi = unpackIndex(packed, bits, x, y, z);
                const size_t blockIndex = static_cast<size_t>(x) * 256 + static_cast<size_t>(y) * 16 + static_cast<size_t>(z);
                blocks[blockIndex] = pi < palette.size() ? palette[pi] : palette.front();
            }
        }
    }
    return blocks;
}

enum class EncodingKind {
    Network,
    Disk,
};

void encodePaletteEntry(std::vector<uint8_t>& out, uint32_t runtimeId, EncodingKind kind) {
    if (kind == EncodingKind::Network) {
        writeVarInt32(out, static_cast<int32_t>(runtimeId));
        return;
    }
    struct EncodedDiskStateCache {
        std::uint64_t generation = 0;
        std::size_t bytes = 0;
        std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> entries;
    };
    thread_local EncodedDiskStateCache cache;
    const auto registryGeneration = BlockStateRegistry::instance().generation();
    if (cache.generation != registryGeneration) {
        cache.entries.clear();
        cache.bytes = 0;
        cache.generation = registryGeneration;
    }
    if (const auto cached = cache.entries.find(runtimeId);
        cached != cache.entries.end()) {
        out.insert(out.end(), cached->second.begin(), cached->second.end());
        return;
    }
    const auto* entry = BlockStateRegistry::instance().byRuntime(runtimeId);
    if (!entry) {
        entry = BlockStateRegistry::instance().byRuntime(BlockStateRegistry::instance().airRuntimeId());
    }
    auto nbt = encodeDiskBlockState(
        entry ? entry->name : "minecraft:air",
        entry ? entry->states : StateMap{},
        entry ? entry->version : static_cast<int32_t>(kCurrentBlockVersion)
    );
    out.insert(out.end(), nbt.begin(), nbt.end());
    if (cache.entries.size() < kEncodedDiskStateCacheEntryLimit &&
        nbt.size() <= kEncodedDiskStateCacheLimit -
            std::min(kEncodedDiskStateCacheLimit, cache.bytes)) {
        cache.bytes += nbt.size();
        cache.entries.emplace(runtimeId, std::move(nbt));
    }
}

std::vector<uint8_t> encodeSubChunk(const NativeSubChunk& subChunk, int rangeStart, int ind, EncodingKind kind) {
    const bool profileEnabled = decodeProfileEnabled();
    const auto call = profileEnabled
        ? gEncodeProfile.calls.fetch_add(1, std::memory_order_relaxed)
        : 0;
    const bool sampleProfile = profileEnabled && ((call & 63u) == 0);
    if (sampleProfile) {
        gEncodeProfile.sampledCalls.fetch_add(1, std::memory_order_relaxed);
    }
    std::vector<uint8_t> out;
    const uint8_t storageCount = static_cast<uint8_t>(std::max<size_t>(1, subChunk.layerCount()));
    out.push_back(9);
    out.push_back(storageCount);
    out.push_back(static_cast<uint8_t>(static_cast<int8_t>(ind + (rangeStart >> 4))));

    for (uint8_t layer = 0; layer < storageCount; ++layer) {
        if (sampleProfile) {
            gEncodeProfile.sampledLayers.fetch_add(1, std::memory_order_relaxed);
        }
        const auto blocks = subChunk.blocksView(layer);
        const auto paletteStart = sampleProfile
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        struct EncodeWorkspace {
            std::vector<std::uint32_t> palette;
            std::vector<std::uint32_t> packed;
            std::array<std::uint16_t, 4096> blockPaletteIndices{};
            PaletteIndexTable paletteIndex;

            EncodeWorkspace()
            {
                palette.reserve(64);
                packed.reserve(packedUint32Count(16));
            }
        };
        thread_local EncodeWorkspace workspace;
        auto& palette = workspace.palette;
        auto& packed = workspace.packed;
        auto& blockPaletteIndices = workspace.blockPaletteIndices;
        auto& paletteIndex = workspace.paletteIndex;
        palette.clear();
        paletteIndex.reset();
        std::uint32_t previousBlock = 0;
        std::uint16_t previousPaletteEntry = 0;
        bool havePreviousBlock = false;
        for (std::size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
            const auto block = blocks[blockIndex];
            if (havePreviousBlock && block == previousBlock) {
                blockPaletteIndices[blockIndex] = previousPaletteEntry;
                continue;
            }
            const auto [paletteEntry, inserted] = paletteIndex.findOrInsert(
                block, static_cast<std::uint16_t>(palette.size()));
            if (inserted) {
                palette.push_back(block);
            }
            blockPaletteIndices[blockIndex] = paletteEntry;
            previousBlock = block;
            previousPaletteEntry = paletteEntry;
            havePreviousBlock = true;
        }
        if (sampleProfile) {
            gEncodeProfile.paletteBuildNs.fetch_add(
                elapsedNs(paletteStart), std::memory_order_relaxed);
            gEncodeProfile.sampledPaletteEntries.fetch_add(
                palette.size(), std::memory_order_relaxed);
        }

        const uint8_t bits = paletteSizeFor(palette.size());
        out.push_back(static_cast<uint8_t>((bits << 1) | (kind == EncodingKind::Network ? 1 : 0)));
        const auto packStart = sampleProfile
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        packIndices(blockPaletteIndices, bits, packed);
        if (sampleProfile) {
            gEncodeProfile.indexPackNs.fetch_add(
                elapsedNs(packStart), std::memory_order_relaxed);
        }
        const auto packedWriteStart = sampleProfile
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        for (uint32_t word : packed) {
            writeLe32(out, word);
        }

        if (bits != 0) {
            if (kind == EncodingKind::Network) {
                writeVarInt32(out, static_cast<int32_t>(palette.size()));
            } else {
                writeLe32(out, static_cast<uint32_t>(palette.size()));
            }
        }
        if (sampleProfile) {
            gEncodeProfile.packedWriteNs.fetch_add(
                elapsedNs(packedWriteStart), std::memory_order_relaxed);
        }
        const auto paletteWriteStart = sampleProfile
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        for (uint32_t runtimeId : palette) {
            encodePaletteEntry(out, runtimeId, kind);
        }
        if (sampleProfile) {
            gEncodeProfile.paletteWriteNs.fetch_add(
                elapsedNs(paletteWriteStart), std::memory_order_relaxed);
        }
    }
    return out;
}

std::vector<uint32_t> decodePalettedStorage(
    ByteReader& r,
    EncodingKind kind,
    bool sampleProfile) {
    const auto packedStart = sampleProfile
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    uint8_t bits = r.u8();
    bits >>= 1;
    if (bits == 0x7f) {
        return {};
    }
    if (!isValidPaletteBits(bits)) {
        throw std::runtime_error("invalid palette bit width");
    }
    std::vector<uint32_t> packed;
    packed.reserve(packedUint32Count(bits));
    for (size_t i = 0; i < packedUint32Count(bits); ++i) {
        packed.push_back(r.le32());
    }

    uint32_t paletteCount = 1;
    if (bits != 0) {
        const int32_t decodedPaletteCount = kind == EncodingKind::Network ? r.varInt32() : static_cast<int32_t>(r.le32());
        if (decodedPaletteCount <= 0) {
            throw std::runtime_error("empty palette");
        }
        paletteCount = static_cast<uint32_t>(decodedPaletteCount);
    }
    if (paletteCount == 0 || paletteCount > 4096) {
        throw std::runtime_error("invalid palette size");
    }
    if (sampleProfile) {
        gDecodeProfile.packedReadNs.fetch_add(
            elapsedNs(packedStart), std::memory_order_relaxed);
        gDecodeProfile.sampledPaletteEntries.fetch_add(
            paletteCount, std::memory_order_relaxed);
    }

    const auto paletteStart = sampleProfile
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    std::vector<uint32_t> palette;
    palette.reserve(paletteCount);
    for (uint32_t i = 0; i < paletteCount; ++i) {
        if (kind == EncodingKind::Network) {
            palette.push_back(static_cast<uint32_t>(r.varInt32()));
        } else {
            const auto* encodedBegin = r.current();
            auto probe = r;
            skipDiskBlockState(probe);
            const auto encodedSize = static_cast<std::size_t>(probe.current() - encodedBegin);
            const std::string_view encodedState(
                reinterpret_cast<const char*>(encodedBegin), encodedSize);
            if (const auto cached =
                    BlockStateRegistry::instance().cachedDiskRuntime(encodedState)) {
                r = probe;
                palette.push_back(*cached);
                continue;
            }
            auto state = parseDiskBlockState(r);
            auto runtime = BlockStateRegistry::instance().toRuntime(
                state.name, state.states, state.version, encodedState);
            palette.push_back(runtime.value_or(BlockStateRegistry::instance().airRuntimeId()));
        }
    }
    if (sampleProfile) {
        gDecodeProfile.paletteResolveNs.fetch_add(
            elapsedNs(paletteStart), std::memory_order_relaxed);
    }

    const auto expandStart = sampleProfile
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    std::vector<uint32_t> blocks(4096, palette.front());
    if (bits != 0) {
        const std::uint32_t mask = (1u << bits) - 1u;
        const std::size_t valuesPerWord = 32u / bits;
        std::size_t storageIndex = 0;
        for (const auto word : packed) {
            for (std::size_t slot = 0;
                 slot < valuesPerWord && storageIndex < blocks.size();
                 ++slot, ++storageIndex) {
                const auto paletteIndex = (word >> (slot * bits)) & mask;
                // Disk order is (x,z,y); NativeSubChunk stores (x,y,z).
                const auto blockIndex =
                    (storageIndex & 0xf00u) |
                    ((storageIndex & 0x00fu) << 4u) |
                    ((storageIndex & 0x0f0u) >> 4u);
                blocks[blockIndex] = paletteIndex < palette.size()
                    ? palette[paletteIndex]
                    : palette.front();
            }
        }
    }
    if (sampleProfile) {
        gDecodeProfile.blockExpandNs.fetch_add(
            elapsedNs(expandStart), std::memory_order_relaxed);
    }
    return blocks;
}

std::pair<std::shared_ptr<NativeSubChunk>, int> decodeSubChunk(
    const std::vector<uint8_t>& payload,
    int rangeStart,
    EncodingKind kind,
    bool sampleProfile = false) {
    ByteReader r(payload);
    const uint8_t version = r.u8();
    const auto nativeStart = sampleProfile
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    auto sub = std::make_shared<NativeSubChunk>();
    if (sampleProfile) {
        gDecodeProfile.nativeInitNs.fetch_add(
            elapsedNs(nativeStart), std::memory_order_relaxed);
    }
    int index = 0;
    uint8_t storageCount = 1;
    if (version == 1) {
        storageCount = 1;
    } else if (version == 8 || version == 9) {
        storageCount = r.u8();
        if (version == 9) {
            index = static_cast<int>(static_cast<int8_t>(r.u8())) - (rangeStart >> 4);
        }
    } else {
        throw std::runtime_error("unsupported subchunk version");
    }

    for (uint8_t layer = 0; layer < storageCount; ++layer) {
        if (sampleProfile) {
            gDecodeProfile.sampledLayers.fetch_add(1, std::memory_order_relaxed);
        }
        auto blocks = decodePalettedStorage(r, kind, sampleProfile);
        if (!blocks.empty()) {
            const auto setBlocksStart = sampleProfile
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            sub->setBlocks(layer, std::move(blocks));
            if (sampleProfile) {
                gDecodeProfile.setBlocksNs.fetch_add(
                    elapsedNs(setBlocksStart), std::memory_order_relaxed);
            }
        }
    }
    return {sub, index};
}

BedrockWorldOperator::BlockStateList decodeDiskSubChunkBlockStates(const std::vector<uint8_t>& payload, int rangeStart) {
    ByteReader r(payload);
    const uint8_t version = r.u8();
    uint8_t storageCount = 1;
    if (version == 1) {
        storageCount = 1;
    } else if (version == 8 || version == 9) {
        storageCount = r.u8();
        if (version == 9) {
            (void)r.u8();
        }
    } else {
        throw std::runtime_error("unsupported subchunk version");
    }

    (void)rangeStart;
    BedrockWorldOperator::BlockStateList result;
    for (uint8_t layer = 0; layer < storageCount; ++layer) {
        auto blocks = decodeDiskBlockStates(r);
        if (layer == 0) {
            result = std::move(blocks);
        }
    }
    if (result.empty()) {
        BedrockWorldOperator::BlockState air;
        air.name = "minecraft:air";
        result.assign(4096, std::move(air));
    }
    return result;
}

class NativeBedrockWorld {
public:
    static std::shared_ptr<NativeBedrockWorld> open(const std::string& dirName) {
        auto world = std::shared_ptr<NativeBedrockWorld>(new NativeBedrockWorld());
        world->mDir = fs::u8path(dirName);
        std::error_code ec;
        fs::create_directories(world->mDir / "db", ec);
        if (ec) {
            return nullptr;
        }

        leveldb::Options options;
        options.create_if_missing = true;
        options.block_size = 16 * 1024;
        leveldb::DB* db = nullptr;
        auto status = leveldb::DB::Open(options, (world->mDir / "db").string(), &db);
        if (!status.ok() || !db) {
            return nullptr;
        }
        world->mDb.reset(db);
        world->loadLevelDat();
        return world;
    }

    std::string get(const std::vector<uint8_t>& key) const {
        std::string value;
        auto status = mDb->Get(leveldb::ReadOptions(), slice(key), &value);
        if (status.IsNotFound()) {
            return {};
        }
        return status.ok() ? value : std::string{};
    }

    bool has(const std::vector<uint8_t>& key) {
        std::string value;
        auto status = mDb->Get(leveldb::ReadOptions(), slice(key), &value);
        return status.ok();
    }

    std::vector<BedrockWorldOperator::SubChunkPos> listSubChunks(int dm) const {
        std::vector<BedrockWorldOperator::SubChunkPos> positions;
        if (!mDb) {
            return positions;
        }

        const std::size_t prefixSize = dm == 0 ? 8 : 12;
        std::unique_ptr<leveldb::Iterator> iterator(
            mDb->NewIterator(leveldb::ReadOptions()));
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
            const auto key = iterator->key();
            if (key.size() != prefixSize + 2) {
                continue;
            }
            const auto* data = reinterpret_cast<const uint8_t*>(key.data());
            if (dm != 0 && static_cast<int32_t>(readLe32(data + 8)) != dm) {
                continue;
            }
            if (data[prefixSize] != static_cast<uint8_t>('/')) {
                continue;
            }
            positions.push_back({
                static_cast<int32_t>(readLe32(data)),
                static_cast<int8_t>(data[prefixSize + 1]),
                static_cast<int32_t>(readLe32(data + 4)),
            });
        }
        if (!iterator->status().ok()) {
            throw std::runtime_error(iterator->status().ToString());
        }
        std::sort(positions.begin(), positions.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.x != rhs.x) return lhs.x < rhs.x;
            if (lhs.z != rhs.z) return lhs.z < rhs.z;
            return lhs.y < rhs.y;
        });
        return positions;
    }

    std::vector<BedrockWorldOperator::ChunkPos> listNbtChunks(int dm) const {
        std::vector<BedrockWorldOperator::ChunkPos> positions;
        if (!mDb) {
            return positions;
        }

        const std::size_t prefixSize = dm == 0 ? 8 : 12;
        std::unique_ptr<leveldb::Iterator> iterator(
            mDb->NewIterator(leveldb::ReadOptions()));
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
            const auto key = iterator->key();
            if (key.size() != prefixSize + 1) {
                continue;
            }
            const auto* data = reinterpret_cast<const uint8_t*>(key.data());
            if (dm != 0 && static_cast<int32_t>(readLe32(data + 8)) != dm) {
                continue;
            }
            if (data[prefixSize] != static_cast<uint8_t>('1')) {
                continue;
            }
            positions.push_back({
                static_cast<int32_t>(readLe32(data)),
                static_cast<int32_t>(readLe32(data + 4)),
            });
        }
        if (!iterator->status().ok()) {
            throw std::runtime_error(iterator->status().ToString());
        }
        std::sort(positions.begin(), positions.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.x != rhs.x) return lhs.x < rhs.x;
            return lhs.z < rhs.z;
        });
        return positions;
    }

    std::string put(const std::vector<uint8_t>& key, const std::string& value) {
        auto status = mDb->Put(leveldb::WriteOptions(), slice(key), leveldb::Slice(value));
        return status.ok() ? "" : status.ToString();
    }

    std::string del(const std::vector<uint8_t>& key) {
        auto status = mDb->Delete(leveldb::WriteOptions(), slice(key));
        return status.ok() ? "" : status.ToString();
    }

    std::shared_ptr<NativeSubChunk> loadSubChunk(int dm, int x, int y, int z) {
        if (y < (rangeStartForDim(dm) >> 4) || y > (rangeEndForDim(dm) >> 4)) {
            return nullptr;
        }
        const auto key = subChunkKey(dm, x, y, z);
        const std::string data = get(key);
        if (data.empty()) {
            if (has(makeChunkKey(dm, x, z, {','})) || has(makeChunkKey(dm, x, z, {'v'}))) {
                return std::make_shared<NativeSubChunk>();
            }
            return nullptr;
        }
        std::vector<uint8_t> payload(data.begin(), data.end());
        try {
            return decodeSubChunk(payload, rangeStartForDim(dm), EncodingKind::Disk).first;
        } catch (...) {
            return nullptr;
        }
    }

    BedrockWorldOperator::BlockStateList loadSubChunkBlockStates(int dm, int x, int y, int z) {
        if (y < (rangeStartForDim(dm) >> 4) || y > (rangeEndForDim(dm) >> 4)) {
            return {};
        }
        const auto key = subChunkKey(dm, x, y, z);
        const std::string data = get(key);
        if (data.empty()) {
            if (has(makeChunkKey(dm, x, z, {','})) || has(makeChunkKey(dm, x, z, {'v'}))) {
                BedrockWorldOperator::BlockState air;
                air.name = "minecraft:air";
                return BedrockWorldOperator::BlockStateList(4096, std::move(air));
            }
            return {};
        }
        std::vector<uint8_t> payload(data.begin(), data.end());
        return decodeDiskSubChunkBlockStates(payload, rangeStartForDim(dm));
    }

    std::string saveSubChunk(int dm, int x, int y, int z, const NativeSubChunk& sub) {
        writeChunkMeta(dm, x, z);
        const int fixed = ((y << 4) - rangeStartForDim(dm)) >> 4;
        auto payload = encodeSubChunk(sub, rangeStartForDim(dm), fixed, EncodingKind::Disk);
        return put(subChunkKey(dm, x, y, z), std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    }

    std::string saveSubChunksBatch(int dm, std::span<const BedrockWorldOperator::PositionedSubChunkWrite> writes) {
        if (!mDb) {
            return "database is not open";
        }
        leveldb::WriteBatch batch;
        std::set<std::pair<int, int>> metadata;
        for (const auto& write : writes) {
            const auto& pos = write.position;
            if (!metadata.emplace(pos.x, pos.z).second) {
                continue;
            }
            const auto metaKey = makeChunkKey(dm, pos.x, pos.z, {','});
            batch.Put(slice(metaKey), leveldb::Slice(std::string(1, static_cast<char>(kChunkVersion))));
            std::vector<uint8_t> finalisation;
            writeLe32(finalisation, kFinalisationGenerated);
            const auto finalKey = makeChunkKey(dm, pos.x, pos.z, {'6'});
            batch.Put(slice(finalKey), slice(finalisation));
        }
        for (const auto& write : writes) {
            const auto& pos = write.position;
            const auto key = subChunkKey(dm, pos.x, pos.y, pos.z);
            if (!write.subChunk || !write.subChunk->valid()) {
                batch.Delete(slice(key));
                continue;
            }
            const int fixed = ((pos.y << 4) - rangeStartForDim(dm)) >> 4;
            const auto encoded = BedrockWorldOperator::encodeSubChunkPayload(
                *write.subChunk,
                BedrockWorldOperator::Encoding::Disk,
                rangeStartForDim(dm),
                rangeEndForDim(dm),
                fixed);
            if (!encoded.ok) {
                return encoded.error;
            }
            batch.Put(slice(key), leveldb::Slice(reinterpret_cast<const char*>(encoded.value.data()), encoded.value.size()));
        }
        const auto status = mDb->Write(leveldb::WriteOptions(), &batch);
        return status.ok() ? "" : status.ToString();
    }

    BedrockWorldOperator::Result<std::optional<BedrockWorldOperator::Bytes>> loadSubChunkPayload(
        int dm, int x, int y, int z) const {
        if (!mDb) {
            return BedrockWorldOperator::Result<std::optional<BedrockWorldOperator::Bytes>>::failure(
                "database is not open");
        }
        std::string value;
        const auto status = mDb->Get(
            leveldb::ReadOptions(), slice(subChunkKey(dm, x, y, z)), &value);
        if (status.IsNotFound()) {
            return BedrockWorldOperator::Result<std::optional<BedrockWorldOperator::Bytes>>::success(
                std::nullopt);
        }
        if (!status.ok()) {
            return BedrockWorldOperator::Result<std::optional<BedrockWorldOperator::Bytes>>::failure(
                status.ToString());
        }
        BedrockWorldOperator::Bytes payload(value.begin(), value.end());
        return BedrockWorldOperator::Result<std::optional<BedrockWorldOperator::Bytes>>::success(
            std::move(payload));
    }

    std::string saveSubChunkPayloadsBatch(
        int dm,
        std::span<const BedrockWorldOperator::PositionedSubChunkPayload> writes) {
        if (!mDb) {
            return "database is not open";
        }
        leveldb::WriteBatch batch;
        std::set<std::pair<int, int>> metadata;
        for (const auto& write : writes) {
            if (!write.payload) {
                continue;
            }
            const auto& pos = write.position;
            if (!metadata.emplace(pos.x, pos.z).second) {
                continue;
            }
            const auto metaKey = makeChunkKey(dm, pos.x, pos.z, {','});
            batch.Put(slice(metaKey), leveldb::Slice(std::string(1, static_cast<char>(kChunkVersion))));
            std::vector<uint8_t> finalisation;
            writeLe32(finalisation, kFinalisationGenerated);
            const auto finalKey = makeChunkKey(dm, pos.x, pos.z, {'6'});
            batch.Put(slice(finalKey), slice(finalisation));
        }
        for (const auto& write : writes) {
            const auto key = subChunkKey(
                dm, write.position.x, write.position.y, write.position.z);
            if (!write.payload) {
                batch.Delete(slice(key));
                continue;
            }
            const auto& payload = *write.payload;
            batch.Put(
                slice(key),
                leveldb::Slice(
                    reinterpret_cast<const char*>(payload.data()), payload.size()));
        }
        const auto status = mDb->Write(leveldb::WriteOptions(), &batch);
        return status.ok() ? "" : status.ToString();
    }

    std::vector<std::pair<std::string, int>> loadSubChunkPayloads(int dm, int x, int z, int minSubY, int maxSubY) const {
        std::vector<std::pair<std::string, int>> result;
        if (!mDb) return result;
        const int minY = std::max(minSubY, rangeStartForDim(dm) >> 4);
        const int maxY = std::min(maxSubY, rangeEndForDim(dm) >> 4);
        result.reserve(static_cast<size_t>(std::max(0, maxY - minY + 1)));
        for (int y = minY; y <= maxY; ++y) {
            const auto data = get(subChunkKey(dm, x, y, z));
            if (!data.empty()) result.emplace_back(data, y);
        }
        return result;
    }

    std::vector<std::string> loadChunkPayloadOnly(int dm, int x, int z, bool& exists) {
        exists = has(makeChunkKey(dm, x, z, {','})) || has(makeChunkKey(dm, x, z, {'v'}));
        if (!exists) {
            return {};
        }
        const int count = (rangeEndForDim(dm) - rangeStartForDim(dm) + 1) >> 4;
        std::vector<std::string> result(static_cast<size_t>(count));
        const int startSub = rangeStartForDim(dm) >> 4;
        for (int i = 0; i < count; ++i) {
            result[static_cast<size_t>(i)] = get(subChunkKey(dm, x, startSub + i, z));
        }
        return result;
    }

    std::string saveChunkPayloadOnly(int dm, int x, int z, const std::vector<std::string>& payloads) {
        writeChunkMeta(dm, x, z);
        const int startSub = rangeStartForDim(dm) >> 4;
        for (size_t i = 0; i < payloads.size(); ++i) {
            auto key = subChunkKey(dm, x, startSub + static_cast<int>(i), z);
            if (payloads[i].empty()) {
                (void)del(key);
            } else {
                auto err = put(key, payloads[i]);
                if (!err.empty()) {
                    return err;
                }
            }
        }
        return "";
    }

    std::string loadKeyWithPrefix(int dm, int x, int z, const std::vector<uint8_t>& suffix) {
        return get(makeChunkKey(dm, x, z, suffix));
    }

    std::string saveKeyWithPrefix(int dm, int x, int z, const std::vector<uint8_t>& suffix, const std::string& value) {
        if (value.empty()) {
            return del(makeChunkKey(dm, x, z, suffix));
        }
        return put(makeChunkKey(dm, x, z, suffix), value);
    }

    std::string loadBiomes(int dm, int x, int z) {
        std::string data = get(makeChunkKey(dm, x, z, {'+'}));
        if (data.size() <= 512) {
            return {};
        }
        return data.substr(512);
    }

    std::string saveBiomes(int dm, int x, int z, const std::string& payload) {
        if (payload.empty()) {
            return del(makeChunkKey(dm, x, z, {'+'}));
        }
        return put(makeChunkKey(dm, x, z, {'+'}), std::string(512, '\0') + payload);
    }

    const std::vector<uint8_t>& levelDatPayload() const {
        return mLevelDatPayload;
    }

    std::string modifyLevelDat(std::vector<uint8_t> payload) {
        mLevelDatPayload = std::move(payload);
        return writeLevelDat();
    }

    std::string closeWorld() {
        std::string err = writeLevelDat();
        mDb.reset();
        return err;
    }

private:
    static leveldb::Slice slice(const std::vector<uint8_t>& v) {
        return leveldb::Slice(reinterpret_cast<const char*>(v.data()), v.size());
    }

    static std::vector<uint8_t> subChunkKey(int dm, int x, int y, int z) {
        return makeChunkKey(dm, x, z, {'/', static_cast<uint8_t>(y & 0xff)});
    }

    void writeChunkMeta(int dm, int x, int z) {
        (void)put(makeChunkKey(dm, x, z, {','}), std::string(1, static_cast<char>(kChunkVersion)));
        std::vector<uint8_t> finalisation;
        writeLe32(finalisation, kFinalisationGenerated);
        (void)put(makeChunkKey(dm, x, z, {'6'}), std::string(reinterpret_cast<const char*>(finalisation.data()), finalisation.size()));
    }

    static std::vector<uint8_t> defaultLevelDatPayload() {
        std::vector<uint8_t> payload;
        StateValue s;
        s.type = TagType::String;
        s.stringValue = "*";
        writeNbtEntry(payload, TagType::String, "baseGameVersion", s);
        s.stringValue = "World";
        writeNbtEntry(payload, TagType::String, "LevelName", s);
        s.stringValue = "1.21.90";
        writeNbtEntry(payload, TagType::String, "InventoryVersion", s);
        StateValue i;
        i.type = TagType::Int;
        i.intValue = 1;
        writeNbtEntry(payload, TagType::Int, "GameType", i);
        i.intValue = 2;
        writeNbtEntry(payload, TagType::Int, "Generator", i);
        i.intValue = 9;
        writeNbtEntry(payload, TagType::Int, "StorageVersion", i);
        i.intValue = 1;
        writeNbtEntry(payload, TagType::Int, "WorldVersion", i);
        payload.push_back(0);
        return encodeRootCompound(payload);
    }

    void loadLevelDat() {
        std::ifstream in(mDir / "level.dat", std::ios::binary);
        if (!in.is_open()) {
            mLevelDatPayload = defaultLevelDatPayload();
            (void)writeLevelDat();
            return;
        }
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (bytes.size() >= 8) {
            const uint32_t len = readLe32(bytes.data() + 4);
            if (bytes.size() >= static_cast<size_t>(8) + len) {
                mLevelDatPayload.assign(bytes.begin() + 8, bytes.begin() + 8 + len);
                return;
            }
        }
        mLevelDatPayload = defaultLevelDatPayload();
    }

    std::string writeLevelDat() {
        std::error_code ec;
        fs::create_directories(mDir, ec);
        std::ofstream out(mDir / "level.dat", std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return "failed to open level.dat";
        }
        std::vector<uint8_t> header;
        writeLe32(header, 10);
        writeLe32(header, static_cast<uint32_t>(mLevelDatPayload.size()));
        out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(mLevelDatPayload.data()), static_cast<std::streamsize>(mLevelDatPayload.size()));
        std::ofstream name(mDir / "levelname.txt", std::ios::binary | std::ios::trunc);
        if (name.is_open()) {
            name << "World";
        }
        return out ? "" : "failed to write level.dat";
    }

    fs::path mDir;
    std::unique_ptr<leveldb::DB> mDb;
    std::vector<uint8_t> mLevelDatPayload;
};

ObjectStore<NativeBedrockWorld> gWorlds;

std::string stringsToPackedPayload(const std::vector<std::string>& values) {
    std::vector<uint8_t> out;
    for (const auto& value : values) {
        writeLe32(out, static_cast<uint32_t>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    }
    return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

std::vector<std::string> unpackPackedPayload(const std::vector<uint8_t>& payload) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos + 4 <= payload.size()) {
        const uint32_t len = readLe32(payload.data() + pos);
        pos += 4;
        if (pos + len > payload.size()) {
            break;
        }
        out.emplace_back(reinterpret_cast<const char*>(payload.data() + pos), len);
        pos += len;
    }
    return out;
}

std::vector<uint32_t> unpackU32Matrix(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> out(bytes.size() / 4);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = readLe32(bytes.data() + i * 4);
    }
    return out;
}

std::vector<uint8_t> packU32(const std::vector<uint32_t>& values) {
    std::vector<uint8_t> out;
    out.reserve(values.size() * 4);
    for (uint32_t v : values) {
        writeLe32(out, v);
    }
    return out;
}

} // namespace

namespace BedrockWorldOperator {

namespace {
int toDimensionId(Dimension dim)
{
    return static_cast<int>(dim);
}

EncodingKind toEncodingKind(Encoding encoding)
{
    return encoding == Encoding::Network ? EncodingKind::Network : EncodingKind::Disk;
}

Bytes toBytes(const std::string& value)
{
    return Bytes(value.begin(), value.end());
}

std::string decodeContext(Encoding encoding, int rangeStart, int rangeEnd, size_t size, std::span<const std::uint8_t> payload)
{
    std::ostringstream oss;
    oss << "encoding=" << (encoding == Encoding::Network ? "network" : "disk")
        << " range=" << rangeStart << ".." << rangeEnd
        << " size=" << size;
    if (!payload.empty()) {
        oss << " version=" << static_cast<unsigned int>(payload.front());
    }
    return oss.str();
}
}

struct SubChunk::Impl {
    std::shared_ptr<NativeSubChunk> native;
};

SubChunk::SubChunk() = default;
SubChunk::SubChunk(std::shared_ptr<Impl> impl) : mImpl(std::move(impl)) {}
SubChunk::~SubChunk() = default;

SubChunk SubChunk::createAirFilled()
{
    auto impl = std::make_shared<Impl>();
    impl->native = std::make_shared<NativeSubChunk>();
    return SubChunk(std::move(impl));
}

bool SubChunk::valid() const noexcept
{
    return mImpl && mImpl->native;
}

BlockRuntimeList SubChunk::blocks(int layer) const
{
    if (!valid()) {
        return BlockRuntimeList(4096, airRuntimeId());
    }
    return mImpl->native->blocks(layer);
}

std::span<const std::uint32_t> SubChunk::blocksView(int layer) const noexcept
{
    return valid()
        ? mImpl->native->blocksView(layer)
        : std::span<const std::uint32_t>{};
}

Result<void> SubChunk::setBlocks(std::span<const std::uint32_t> blocks, int layer)
{
    if (layer < 0) {
        return Result<void>::failure("SubChunk::setBlocks: layer must be non-negative");
    }
    if (blocks.size() != 4096) {
        return Result<void>::failure("SubChunk::setBlocks: expected 4096 block runtime ids");
    }
    if (!valid()) {
        *this = createAirFilled();
    }
    mImpl->native->setBlocks(layer, BlockRuntimeList(blocks.begin(), blocks.end()));
    return Result<void>::success();
}

struct Chunk::Impl {
    std::shared_ptr<NativeChunk> native;
};

Chunk::Chunk() = default;
Chunk::Chunk(std::shared_ptr<Impl> impl) : mImpl(std::move(impl)) {}
Chunk::~Chunk() = default;

struct World::Impl {
    std::shared_ptr<NativeBedrockWorld> native;
};

World::World() = default;
World::World(std::shared_ptr<Impl> impl) : mImpl(std::move(impl)) {}
World::~World()
{
    if (mImpl && mImpl->native) {
        mImpl->native->closeWorld();
    }
}

Result<World> World::open(const std::filesystem::path& dir)
{
    auto native = NativeBedrockWorld::open(dir.string());
    if (!native) {
        return Result<World>::failure("World::open failed: " + dir.string());
    }
    auto impl = std::make_shared<Impl>();
    impl->native = std::move(native);
    return Result<World>::success(World(std::move(impl)));
}

bool World::valid() const noexcept
{
    return mImpl && mImpl->native;
}

Result<void> World::close()
{
    if (!valid()) {
        return Result<void>::failure("World::close: world is not open");
    }
    const auto err = mImpl->native->closeWorld();
    mImpl->native.reset();
    if (!err.empty()) {
        return Result<void>::failure("World::close: " + err);
    }
    return Result<void>::success();
}

Result<Bytes> World::levelDat() const
{
    if (!valid()) {
        return Result<Bytes>::failure("World::levelDat: world is not open");
    }
    return Result<Bytes>::success(mImpl->native->levelDatPayload());
}

Result<std::optional<Bytes>> World::loadSubChunkPayload(Dimension dim, SubChunkPos pos) const
{
    if (!valid()) {
        return Result<std::optional<Bytes>>::failure(
            "World::loadSubChunkPayload: world is not open");
    }
    const int dimensionId = toDimensionId(dim);
    const int minSubY = rangeStartForDim(dimensionId) >> 4;
    const int maxSubY = rangeEndForDim(dimensionId) >> 4;
    if (pos.y < minSubY || pos.y > maxSubY) {
        return Result<std::optional<Bytes>>::failure(
            "World::loadSubChunkPayload: subchunk Y is out of dimension range");
    }
    auto loaded = mImpl->native->loadSubChunkPayload(
        dimensionId, pos.x, pos.y, pos.z);
    if (!loaded) {
        return Result<std::optional<Bytes>>::failure(
            "World::loadSubChunkPayload: " + loaded.error);
    }
    return loaded;
}

Result<SubChunk> World::loadSubChunk(Dimension dim, SubChunkPos pos) const
{
    if (!valid()) {
        return Result<SubChunk>::failure("World::loadSubChunk: world is not open");
    }
    auto native = mImpl->native->loadSubChunk(toDimensionId(dim), pos.x, pos.y, pos.z);
    if (!native) {
        return Result<SubChunk>::failure("World::loadSubChunk: subchunk not found");
    }
    auto impl = std::make_shared<SubChunk::Impl>();
    impl->native = std::move(native);
    return Result<SubChunk>::success(SubChunk(std::move(impl)));
}

Result<std::vector<DecodedSubChunk>> World::loadSubChunks(Dimension dim, ChunkPos chunk, int minSubY, int maxSubY) const
{
    if (!valid()) {
        return Result<std::vector<DecodedSubChunk>>::failure("World::loadSubChunks: world is not open");
    }
    if (minSubY > maxSubY) {
        return Result<std::vector<DecodedSubChunk>>::success({});
    }
    const int dimensionId = toDimensionId(dim);
    const auto payloads = mImpl->native->loadSubChunkPayloads(dimensionId, chunk.x, chunk.z, minSubY, maxSubY);
    std::vector<DecodedSubChunk> result;
    result.reserve(payloads.size());
    for (const auto& [payload, y] : payloads) {
        const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        auto decoded = decodeSubChunkPayload(bytes, Encoding::Disk, rangeStartForDim(dimensionId), rangeEndForDim(dimensionId));
        if (!decoded.ok) continue;
        decoded.value.index = y;
        result.push_back(std::move(decoded.value));
    }
    return Result<std::vector<DecodedSubChunk>>::success(std::move(result));
}

Result<BlockStateList> World::loadSubChunkBlockStates(Dimension dim, SubChunkPos pos) const
{
    if (!valid()) {
        return Result<BlockStateList>::failure("world is not open");
    }
    try {
        auto blocks = mImpl->native->loadSubChunkBlockStates(toDimensionId(dim), pos.x, pos.y, pos.z);
        if (blocks.empty()) {
            return Result<BlockStateList>::failure("subchunk not found");
        }
        return Result<BlockStateList>::success(std::move(blocks));
    } catch (const std::exception& e) {
        return Result<BlockStateList>::failure(e.what());
    } catch (...) {
        return Result<BlockStateList>::failure("failed to load disk block states");
    }
}

Result<std::vector<SubChunkPos>> World::listSubChunks(Dimension dim) const
{
    if (!valid()) {
        return Result<std::vector<SubChunkPos>>::failure("world is not open");
    }
    try {
        return Result<std::vector<SubChunkPos>>::success(
            mImpl->native->listSubChunks(toDimensionId(dim)));
    } catch (const std::exception& e) {
        return Result<std::vector<SubChunkPos>>::failure(e.what());
    } catch (...) {
        return Result<std::vector<SubChunkPos>>::failure("failed to enumerate subchunks");
    }
}

Result<void> World::saveSubChunk(Dimension dim, SubChunkPos pos, const SubChunk& subChunk)
{
    if (!valid()) {
        return Result<void>::failure("World::saveSubChunk: world is not open");
    }
    if (!subChunk.valid()) {
        return Result<void>::failure("World::saveSubChunk: subchunk is not valid");
    }
    const auto err = mImpl->native->saveSubChunk(toDimensionId(dim), pos.x, pos.y, pos.z, *subChunk.mImpl->native);
    if (!err.empty()) {
        return Result<void>::failure("World::saveSubChunk: " + err);
    }
    return Result<void>::success();
}

Result<void> World::saveSubChunksBatch(Dimension dim, std::span<const PositionedSubChunkWrite> writes)
{
    if (!valid()) {
        return Result<void>::failure("World::saveSubChunksBatch: world is not open");
    }
    if (writes.empty()) {
        return Result<void>::success();
    }
    const auto err = mImpl->native->saveSubChunksBatch(toDimensionId(dim), writes);
    if (!err.empty()) {
        return Result<void>::failure("World::saveSubChunksBatch: " + err);
    }
    return Result<void>::success();
}

Result<void> World::saveSubChunkPayloadsBatch(
    Dimension dim,
    std::span<const PositionedSubChunkPayload> writes)
{
    if (!valid()) {
        return Result<void>::failure(
            "World::saveSubChunkPayloadsBatch: world is not open");
    }
    if (writes.empty()) {
        return Result<void>::success();
    }
    const int dimensionId = toDimensionId(dim);
    const int minSubY = rangeStartForDim(dimensionId) >> 4;
    const int maxSubY = rangeEndForDim(dimensionId) >> 4;
    for (const auto& write : writes) {
        if (write.position.y < minSubY || write.position.y > maxSubY) {
            return Result<void>::failure(
                "World::saveSubChunkPayloadsBatch: subchunk Y is out of dimension range");
        }
        if (!write.payload) {
            continue;
        }
        if (write.payload->empty()) {
            return Result<void>::failure(
                "World::saveSubChunkPayloadsBatch: payload is empty; use nullopt to delete");
        }
        const auto version = write.payload->front();
        if (version != 1 && version != 8 && version != 9) {
            return Result<void>::failure(
                "World::saveSubChunkPayloadsBatch: unsupported subchunk payload version");
        }
        if ((version == 8 || version == 9) && write.payload->size() < 2) {
            return Result<void>::failure(
                "World::saveSubChunkPayloadsBatch: truncated subchunk payload header");
        }
        if ((version == 8 || version == 9) && (*write.payload)[1] == 0) {
            return Result<void>::failure(
                "World::saveSubChunkPayloadsBatch: subchunk payload has no storages");
        }
        if (version == 9) {
            if (write.payload->size() < 3) {
                return Result<void>::failure(
                    "World::saveSubChunkPayloadsBatch: truncated v9 subchunk payload header");
            }
            const int encodedY = static_cast<int>(
                static_cast<std::int8_t>((*write.payload)[2]));
            if (encodedY != write.position.y) {
                return Result<void>::failure(
                    "World::saveSubChunkPayloadsBatch: v9 payload Y does not match position");
            }
        }
    }
    const auto err = mImpl->native->saveSubChunkPayloadsBatch(dimensionId, writes);
    if (!err.empty()) {
        return Result<void>::failure("World::saveSubChunkPayloadsBatch: " + err);
    }
    return Result<void>::success();
}

Result<Bytes> World::loadNbt(Dimension dim, ChunkPos pos) const
{
    if (!valid()) {
        return Result<Bytes>::failure("World::loadNbt: world is not open");
    }
    return Result<Bytes>::success(toBytes(mImpl->native->loadKeyWithPrefix(toDimensionId(dim), pos.x, pos.z, {'1'})));
}

Result<std::vector<ChunkPos>> World::listNbtChunks(Dimension dim) const
{
    if (!valid()) {
        return Result<std::vector<ChunkPos>>::failure("world is not open");
    }
    try {
        return Result<std::vector<ChunkPos>>::success(
            mImpl->native->listNbtChunks(toDimensionId(dim)));
    } catch (const std::exception& e) {
        return Result<std::vector<ChunkPos>>::failure(e.what());
    } catch (...) {
        return Result<std::vector<ChunkPos>>::failure("failed to enumerate NBT chunks");
    }
}

Result<void> World::saveNbt(Dimension dim, ChunkPos pos, std::span<const std::uint8_t> payload)
{
    if (!valid()) {
        return Result<void>::failure("World::saveNbt: world is not open");
    }
    const std::string value(reinterpret_cast<const char*>(payload.data()), payload.size());
    const auto err = mImpl->native->saveKeyWithPrefix(toDimensionId(dim), pos.x, pos.z, {'1'}, value);
    if (!err.empty()) {
        return Result<void>::failure("World::saveNbt: " + err);
    }
    return Result<void>::success();
}

Result<Bytes> World::loadBiomes(Dimension dim, ChunkPos pos) const
{
    if (!valid()) {
        return Result<Bytes>::failure("World::loadBiomes: world is not open");
    }
    return Result<Bytes>::success(toBytes(mImpl->native->loadBiomes(toDimensionId(dim), pos.x, pos.z)));
}

Result<void> World::saveBiomes(Dimension dim, ChunkPos pos, std::span<const std::uint8_t> payload)
{
    if (!valid()) {
        return Result<void>::failure("World::saveBiomes: world is not open");
    }
    const std::string value(reinterpret_cast<const char*>(payload.data()), payload.size());
    const auto err = mImpl->native->saveBiomes(toDimensionId(dim), pos.x, pos.z, value);
    if (!err.empty()) {
        return Result<void>::failure("World::saveBiomes: " + err);
    }
    return Result<void>::success();
}

Result<DecodedSubChunk> decodeSubChunkPayload(std::span<const std::uint8_t> payload, Encoding encoding, int rangeStart, int rangeEnd)
{
    if (payload.empty()) {
        return Result<DecodedSubChunk>::failure("decodeSubChunkPayload: empty payload");
    }
    try {
        const bool profileEnabled = decodeProfileEnabled();
        const auto call = profileEnabled
            ? gDecodeProfile.calls.fetch_add(1, std::memory_order_relaxed)
            : 0;
        const bool sampleProfile = profileEnabled && ((call & 63u) == 0);
        if (sampleProfile) {
            gDecodeProfile.sampledCalls.fetch_add(1, std::memory_order_relaxed);
        }
        const auto copyStart = sampleProfile
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        Bytes bytes(payload.begin(), payload.end());
        if (sampleProfile) {
            gDecodeProfile.payloadCopyNs.fetch_add(
                elapsedNs(copyStart), std::memory_order_relaxed);
        }
        auto [native, index] = decodeSubChunk(
            bytes, rangeStart, toEncodingKind(encoding), sampleProfile);
        if (!native) {
            return Result<DecodedSubChunk>::failure("decodeSubChunkPayload: decoder returned no subchunk");
        }
        const auto wrapperStart = sampleProfile
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        auto impl = std::make_shared<SubChunk::Impl>();
        impl->native = std::move(native);
        DecodedSubChunk decoded;
        decoded.subChunk = SubChunk(std::move(impl));
        decoded.index = index;
        if (sampleProfile) {
            gDecodeProfile.wrapperNs.fetch_add(
                elapsedNs(wrapperStart), std::memory_order_relaxed);
        }
        return Result<DecodedSubChunk>::success(std::move(decoded));
    } catch (const std::exception& e) {
        return Result<DecodedSubChunk>::failure(
            std::string("decodeSubChunkPayload failed: ") + e.what() + " (" +
            decodeContext(encoding, rangeStart, rangeEnd, payload.size(), payload) + ")"
        );
    } catch (...) {
        return Result<DecodedSubChunk>::failure(
            "decodeSubChunkPayload failed: unknown BWO subchunk decode failure (" +
            decodeContext(encoding, rangeStart, rangeEnd, payload.size(), payload) + ")"
        );
    }
}

void resetSubChunkDecodeProfile() noexcept
{
    gDecodeProfile.calls.store(0, std::memory_order_relaxed);
    gDecodeProfile.sampledCalls.store(0, std::memory_order_relaxed);
    gDecodeProfile.sampledLayers.store(0, std::memory_order_relaxed);
    gDecodeProfile.sampledPaletteEntries.store(0, std::memory_order_relaxed);
    gDecodeProfile.payloadCopyNs.store(0, std::memory_order_relaxed);
    gDecodeProfile.nativeInitNs.store(0, std::memory_order_relaxed);
    gDecodeProfile.packedReadNs.store(0, std::memory_order_relaxed);
    gDecodeProfile.paletteResolveNs.store(0, std::memory_order_relaxed);
    gDecodeProfile.blockExpandNs.store(0, std::memory_order_relaxed);
    gDecodeProfile.setBlocksNs.store(0, std::memory_order_relaxed);
    gDecodeProfile.wrapperNs.store(0, std::memory_order_relaxed);
}

SubChunkDecodeProfile subChunkDecodeProfile() noexcept
{
    return {
        gDecodeProfile.calls.load(std::memory_order_relaxed),
        gDecodeProfile.sampledCalls.load(std::memory_order_relaxed),
        gDecodeProfile.sampledLayers.load(std::memory_order_relaxed),
        gDecodeProfile.sampledPaletteEntries.load(std::memory_order_relaxed),
        gDecodeProfile.payloadCopyNs.load(std::memory_order_relaxed),
        gDecodeProfile.nativeInitNs.load(std::memory_order_relaxed),
        gDecodeProfile.packedReadNs.load(std::memory_order_relaxed),
        gDecodeProfile.paletteResolveNs.load(std::memory_order_relaxed),
        gDecodeProfile.blockExpandNs.load(std::memory_order_relaxed),
        gDecodeProfile.setBlocksNs.load(std::memory_order_relaxed),
        gDecodeProfile.wrapperNs.load(std::memory_order_relaxed)
    };
}

void resetSubChunkEncodeProfile() noexcept
{
    gEncodeProfile.calls.store(0, std::memory_order_relaxed);
    gEncodeProfile.sampledCalls.store(0, std::memory_order_relaxed);
    gEncodeProfile.sampledLayers.store(0, std::memory_order_relaxed);
    gEncodeProfile.sampledPaletteEntries.store(0, std::memory_order_relaxed);
    gEncodeProfile.paletteBuildNs.store(0, std::memory_order_relaxed);
    gEncodeProfile.indexPackNs.store(0, std::memory_order_relaxed);
    gEncodeProfile.packedWriteNs.store(0, std::memory_order_relaxed);
    gEncodeProfile.paletteWriteNs.store(0, std::memory_order_relaxed);
}

SubChunkEncodeProfile subChunkEncodeProfile() noexcept
{
    return {
        gEncodeProfile.calls.load(std::memory_order_relaxed),
        gEncodeProfile.sampledCalls.load(std::memory_order_relaxed),
        gEncodeProfile.sampledLayers.load(std::memory_order_relaxed),
        gEncodeProfile.sampledPaletteEntries.load(std::memory_order_relaxed),
        gEncodeProfile.paletteBuildNs.load(std::memory_order_relaxed),
        gEncodeProfile.indexPackNs.load(std::memory_order_relaxed),
        gEncodeProfile.packedWriteNs.load(std::memory_order_relaxed),
        gEncodeProfile.paletteWriteNs.load(std::memory_order_relaxed)
    };
}

Result<Bytes> encodeSubChunkPayload(const SubChunk& subChunk, Encoding encoding, int rangeStart, int rangeEnd, int index)
{
    if (!subChunk.valid()) {
        return Result<Bytes>::failure("encodeSubChunkPayload: subchunk is not valid");
    }
    try {
        Bytes encoded = encodeSubChunk(*subChunk.mImpl->native, rangeStart, index, toEncodingKind(encoding));
        return Result<Bytes>::success(std::move(encoded));
    } catch (const std::exception& e) {
        return Result<Bytes>::failure(
            std::string("encodeSubChunkPayload failed: ") + e.what() + " (" +
            decodeContext(encoding, rangeStart, rangeEnd, 0, {}) + ")"
        );
    } catch (...) {
        return Result<Bytes>::failure("encodeSubChunkPayload failed: unknown BWO subchunk encode failure");
    }
}

void setBlockRuntimeResolver(BlockRuntimeResolver resolver)
{
    BlockStateRegistry::instance().setResolver(std::move(resolver));
}

std::uint32_t airRuntimeId()
{
    return BlockStateRegistry::instance().airRuntimeId();
}

std::optional<std::string> runtimeIdToName(std::uint32_t runtimeId)
{
    const auto* entry = BlockStateRegistry::instance().byRuntime(runtimeId);
    if (!entry) {
        return std::nullopt;
    }
    return entry->name;
}

std::optional<std::uint32_t> nameToRuntimeId(std::string_view name)
{
    return BlockStateRegistry::instance().toRuntime(std::string(name), {});
}

} // namespace BedrockWorldOperator

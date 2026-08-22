#include <WaterStructure/canonical.hpp>
#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

struct DetailRequest {
    std::int32_t chunk_x = 0;
    std::int32_t chunk_z = 0;
    std::int32_t sub_y = 0;
    std::int32_t layer = 0;
};

std::int32_t parse_int(std::string_view text)
{
    std::int32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("无效整数参数: " + std::string(text));
    }
    return value;
}

std::string hex(std::span<const std::uint8_t> bytes)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

class Sha256Stream {
public:
    Sha256Stream() = default;
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    void update(std::span<const std::uint8_t> bytes)
    {
        if (mFinished) throw std::runtime_error("SHA-256 已完成，不能继续写入");
        if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - mTotalBytes) {
            throw std::runtime_error("SHA-256 输入过长");
        }
        mTotalBytes += static_cast<std::uint64_t>(bytes.size());
        if (mBuffered != 0) {
            const auto count = std::min(bytes.size(), mBuffer.size() - mBuffered);
            std::copy_n(bytes.begin(), count, mBuffer.begin() + static_cast<std::ptrdiff_t>(mBuffered));
            mBuffered += count;
            bytes = bytes.subspan(count);
            if (mBuffered == mBuffer.size()) {
                transform(mBuffer.data());
                mBuffered = 0;
            }
        }
        while (bytes.size() >= mBuffer.size()) {
            transform(bytes.data());
            bytes = bytes.subspan(mBuffer.size());
        }
        if (!bytes.empty()) {
            std::copy(bytes.begin(), bytes.end(), mBuffer.begin());
            mBuffered = bytes.size();
        }
    }

    void update(std::string_view bytes)
    {
        update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    }

    std::string finish()
    {
        if (mFinished) throw std::runtime_error("SHA-256 已完成");
        const auto bit_length = mTotalBytes * 8;
        mBuffer[mBuffered++] = 0x80;
        if (mBuffered > 56) {
            std::fill(mBuffer.begin() + static_cast<std::ptrdiff_t>(mBuffered), mBuffer.end(), 0);
            transform(mBuffer.data());
            mBuffered = 0;
        }
        std::fill(mBuffer.begin() + static_cast<std::ptrdiff_t>(mBuffered), mBuffer.begin() + 56, 0);
        for (std::size_t i = 0; i < 8; ++i) mBuffer[63 - i] = static_cast<std::uint8_t>(bit_length >> (i * 8));
        transform(mBuffer.data());
        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < mState.size(); ++i) {
            digest[i * 4] = static_cast<std::uint8_t>(mState[i] >> 24);
            digest[i * 4 + 1] = static_cast<std::uint8_t>(mState[i] >> 16);
            digest[i * 4 + 2] = static_cast<std::uint8_t>(mState[i] >> 8);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(mState[i]);
        }
        mFinished = true;
        return hex(digest);
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRoundConstants{
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };

    void transform(const std::uint8_t* block)
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto p = i * 4;
            words[i] = (static_cast<std::uint32_t>(block[p]) << 24) |
                (static_cast<std::uint32_t>(block[p + 1]) << 16) |
                (static_cast<std::uint32_t>(block[p + 2]) << 8) | block[p + 3];
        }
        for (std::size_t i = 16; i < words.size(); ++i) {
            const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto a = mState[0], b = mState[1], c = mState[2], d = mState[3];
        auto e = mState[4], f = mState[5], g = mState[6], h = mState[7];
        for (std::size_t i = 0; i < words.size(); ++i) {
            const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + sum1 + choose + kRoundConstants[i] + words[i];
            const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = sum0 + majority;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        mState[0] += a; mState[1] += b; mState[2] += c; mState[3] += d;
        mState[4] += e; mState[5] += f; mState[6] += g; mState[7] += h;
    }

    std::array<std::uint32_t, 8> mState{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    std::array<std::uint8_t, 64> mBuffer{};
    std::uint64_t mTotalBytes = 0;
    std::size_t mBuffered = 0;
    bool mFinished = false;
};

std::string sha256(std::span<const std::uint8_t> bytes)
{
    Sha256Stream hash;
    hash.update(bytes);
    return hash.finish();
}

void sha256_self_test()
{
    const auto verify = [](std::string_view input, std::string_view expected) {
        Sha256Stream hash;
        for (std::size_t offset = 0; offset < input.size(); ++offset) {
            const auto* byte = reinterpret_cast<const std::uint8_t*>(input.data() + offset);
            hash.update(std::span<const std::uint8_t>(byte, 1));
        }
        if (hash.finish() != expected) throw std::runtime_error("SHA-256 self-test failed");
    };
    verify("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    verify("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    verify("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void hash_u32(Sha256Stream& hash, std::uint32_t value)
{
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    hash.update(bytes);
}

void hash_u64(Sha256Stream& hash, std::uint64_t value)
{
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
    hash.update(bytes);
}

void hash_file_contents(Sha256Stream& hash, const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取输入: " + path.string());
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
    }
    if (!input.eof()) throw std::runtime_error("读取输入失败: " + path.string());
}

std::string input_sha256(const std::filesystem::path& path)
{
    if (std::filesystem::is_regular_file(path)) {
        Sha256Stream hash;
        hash_file_contents(hash, path);
        return hash.finish();
    }
    if (!std::filesystem::is_directory(path)) {
        throw std::runtime_error("输入不是普通文件或目录: " + path.string());
    }

    struct Entry {
        std::string relative_path;
        std::filesystem::path absolute_path;
        std::uint64_t size = 0;
    };
    std::vector<Entry> entries;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("目录输入不允许符号链接: " + entry.path().string());
        }
        if (!entry.is_regular_file()) continue;
        const auto relative_u8 = std::filesystem::relative(entry.path(), path).generic_u8string();
        entries.push_back({
            std::string(reinterpret_cast<const char*>(relative_u8.data()), relative_u8.size()),
            entry.path(),
            entry.file_size()
        });
    }
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.relative_path < right.relative_path;
    });

    Sha256Stream hash;
    hash.update(std::string_view("WS-DIR-SHA256-V1\0", 17));
    for (const auto& entry : entries) {
        if (entry.relative_path.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("目录相对路径过长");
        }
        hash_u32(hash, static_cast<std::uint32_t>(entry.relative_path.size()));
        hash.update(entry.relative_path);
        hash_u64(hash, entry.size);
        hash_file_contents(hash, entry.absolute_path);
    }
    return hash.finish();
}

std::filesystem::path find_mapping(const std::filesystem::path& executable)
{
    auto directory = std::filesystem::absolute(executable).parent_path();
    for (int i = 0; i < 8 && !directory.empty(); ++i) {
        const auto candidate = directory / "assets" / "block_mappings_v1.json";
        if (std::filesystem::is_regular_file(candidate)) return candidate;
        directory = directory.parent_path();
    }
    const auto local = std::filesystem::current_path() / "assets" / "block_mappings_v1.json";
    return std::filesystem::is_regular_file(local) ? local : std::filesystem::path{};
}

struct LayerSummary {
    std::string hash;
    std::size_t non_air = 0;
};

LayerSummary summarize_layer(
    const water_structure::BlockLayer& layer,
    const water_structure::RuntimeRegistry& registry,
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>& cache)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(layer.size() * 32);
    std::size_t non_air = 0;
    for (const auto runtime_id : layer) {
        if (runtime_id != registry.air_runtime_id()) ++non_air;
        auto found = cache.find(runtime_id);
        if (found == cache.end()) {
            const auto state = registry.state(runtime_id);
            if (!state) {
                throw std::runtime_error("runtime ID 没有对应方块状态: " + std::to_string(runtime_id));
            }
            auto canonical = water_structure::canonical_block_state(*state);
            if (!canonical) throw std::runtime_error(canonical.error());
            found = cache.emplace(runtime_id, std::move(canonical).value()).first;
        }
        const auto length = static_cast<std::uint32_t>(found->second.size());
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<std::uint8_t>(length >> shift));
        }
        bytes.insert(bytes.end(), found->second.begin(), found->second.end());
    }
    return { sha256(bytes), non_air };
}

class JsonSpool final {
public:
    explicit JsonSpool(std::string_view label)
    {
        static std::atomic<std::uint64_t> sequence{ 0 };
        const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
        const auto timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#if defined(_WIN32)
        const auto process_id = static_cast<std::uint64_t>(_getpid());
#else
        const auto process_id = static_cast<std::uint64_t>(getpid());
#endif
        mPath = std::filesystem::temp_directory_path() /
            ("water_structure_cpp_manifest_" + std::string(label) + "_" +
             std::to_string(process_id) + "_" + std::to_string(timestamp) + "_" +
             std::to_string(id) + ".part");
        mOutput.open(mPath, std::ios::binary | std::ios::trunc);
        if (!mOutput) throw std::runtime_error("无法创建 manifest 临时文件: " + mPath.string());
    }
    JsonSpool(const JsonSpool&) = delete;
    JsonSpool& operator=(const JsonSpool&) = delete;
    ~JsonSpool()
    {
        if (mOutput.is_open()) mOutput.close();
        std::error_code error;
        std::filesystem::remove(mPath, error);
    }

    void append(const nlohmann::json& value)
    {
        if (!mFirst) mOutput.put(',');
        mOutput << value.dump();
        if (!mOutput) throw std::runtime_error("写入 manifest 临时文件失败");
        mFirst = false;
    }

    void copy_to(std::ostream& output)
    {
        mOutput.flush();
        mOutput.close();
        std::ifstream input(mPath, std::ios::binary);
        if (!input) throw std::runtime_error("读取 manifest 临时文件失败");
        std::vector<char> buffer(1u << 20);
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) output.write(buffer.data(), count);
        }
        if (!input.eof() || !output) throw std::runtime_error("复制 manifest 临时文件失败");
    }

private:
    std::filesystem::path mPath;
    std::ofstream mOutput;
    bool mFirst = true;
};

void write_manifest_stream(
    const std::optional<std::filesystem::path>& output_path,
    nlohmann::json manifest,
    JsonSpool* chunks,
    JsonSpool* entities,
    bool include_arrays)
{
    manifest.erase("chunks");
    manifest.erase("block_entities");
    auto output = std::ofstream{};
    std::ostream* stream = &std::cout;
    if (output_path) {
        output.open(*output_path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("无法创建 manifest: " + output_path->string());
        stream = &output;
    }
    auto encoded = manifest.dump();
    if (encoded.empty() || encoded.back() != '}') {
        throw std::runtime_error("manifest 元数据不是 JSON object");
    }
    stream->write(encoded.data(), static_cast<std::streamsize>(encoded.size() - 1));
    if (include_arrays) {
        *stream << ",\"chunks\":[";
        chunks->copy_to(*stream);
        *stream << "],\"block_entities\":[";
        entities->copy_to(*stream);
        *stream << ']';
    }
    *stream << "}\n";
    if (!*stream) throw std::runtime_error("写入 manifest 失败");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view(argv[1]) == "--sha256-self-test") {
        try {
            sha256_self_test();
            std::cout << "SHA-256 self-test passed\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 2;
        }
    }
    if (argc < 2) {
        std::cerr << "usage: cpp_manifest <input> [output.json [--format name] [--detail chunkX chunkZ subY layer]]\n"
                     "       cpp_manifest --sha256-self-test\n";
        return 1;
    }
    try {
        std::optional<DetailRequest> detail;
        std::optional<water_structure::StructureId> forced_format;
        std::optional<std::filesystem::path> output_path;
        auto option_index = 2;
        if (argc >= 3 && !std::string_view(argv[2]).starts_with("--")) {
            output_path = std::filesystem::path(argv[2]);
            option_index = 3;
        }
        while (option_index < argc) {
            const auto option = std::string_view(argv[option_index]);
            if (option == "--format") {
                if (forced_format || option_index + 1 >= argc) {
                    throw std::runtime_error("--format 需要唯一的格式名称");
                }
                const auto requested = std::string_view(argv[option_index + 1]);
                const auto found = std::ranges::find_if(
                    water_structure::FormatRegistry::formats(),
                    [&](const auto& value) { return value.name == requested; });
                if (found == water_structure::FormatRegistry::formats().end()) {
                    throw std::runtime_error("未知格式名称: " + std::string(requested));
                }
                forced_format = found->id;
                option_index += 2;
                continue;
            }
            if (option == "--detail") {
                if (detail || option_index + 4 >= argc) {
                    throw std::runtime_error("--detail 需要 chunkX chunkZ subY layer");
                }
                detail = DetailRequest{
                    parse_int(argv[option_index + 1]), parse_int(argv[option_index + 2]),
                    parse_int(argv[option_index + 3]), parse_int(argv[option_index + 4])
                };
                if (detail->layer < 0 || detail->layer > 1) {
                    throw std::runtime_error("detail layer 必须是 0 或 1");
                }
                option_index += 5;
                continue;
            }
            throw std::runtime_error("未知 manifest 参数: " + std::string(option));
        }
        water_structure::RuntimeRegistry registry;
        const auto mapping = find_mapping(argv[0]);
        if (!mapping.empty()) {
            const auto loaded = registry.load_block_mappings(mapping);
            if (!loaded) throw std::runtime_error(loaded.error());
        }
        registry.install_as_bwo_resolver();
        const auto input = std::filesystem::path(argv[1]);
        nlohmann::json manifest;
        manifest["schema"] = 2;
        manifest["block_hash_algorithm"] = "canonical-block-state-v1";
        manifest["nbt_hash_algorithm"] = "canonical-nbt-v1";
        manifest["input_sha256"] = input_sha256(input);
        const auto opened = forced_format
            ? water_structure::FormatRegistry::open_as(input, *forced_format, registry)
            : water_structure::FormatRegistry::open(input, registry);
        if (!opened) {
            manifest["error"] = { { "category", "parse" }, { "message", opened.error() } };
        } else {
            const auto& structure = *opened.value();
            const auto size = structure.size();
            const auto offset = structure.offset();
            manifest["format"] = structure.name();
            manifest["size"] = { size.width, size.height, size.length };
            manifest["offset"] = { offset.x, offset.y, offset.z };
            if (detail) {
                const auto non_air = structure.count_non_air_blocks();
                if (!non_air) throw std::runtime_error(non_air.error());
                manifest["non_air_blocks"] = non_air.value();
            }
            JsonSpool chunk_spool("chunks");
            JsonSpool entity_spool("entities");
            std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> state_cache;
            std::size_t semantic_non_air = 0;
            const auto start_x = detail ? detail->chunk_x : 0;
            const auto end_x = detail ? detail->chunk_x + 1 : size.chunk_x_count();
            const auto start_z = detail ? detail->chunk_z : 0;
            const auto end_z = detail ? detail->chunk_z + 1 : size.chunk_z_count();
            if (start_x < 0 || start_z < 0 || end_x > size.chunk_x_count() ||
                end_z > size.chunk_z_count()) {
                manifest["error"] = {
                    { "category", "detail" },
                    { "message", "detail chunk is outside structure bounds" }
                };
            }
            for (std::int32_t x = start_x; !manifest.contains("error") && x < end_x; ++x) {
                for (std::int32_t z = start_z; z < end_z; ++z) {
                const water_structure::ChunkPos pos{ x, z };
                nlohmann::json entry{ { "x", pos.x }, { "z", pos.z }, { "subchunks", nlohmann::json::array() } };
                const auto chunks = structure.get_chunks(std::span<const water_structure::ChunkPos>(&pos, 1));
                if (!chunks) throw std::runtime_error(chunks.error());
                const auto chunk = chunks.value().find(pos);
                if (chunk != chunks.value().end()) {
                    std::vector<std::int32_t> ys;
                    for (const auto& [y, _] : chunk->second.sub_chunks) ys.push_back(y);
                    std::sort(ys.begin(), ys.end());
                    for (const auto y : ys) {
                        const auto& sub = chunk->second.sub_chunks.at(y);
                        auto layer0 = summarize_layer(sub.layer0, registry, state_cache);
                        auto layer1 = summarize_layer(sub.layer1, registry, state_cache);
                        semantic_non_air += layer0.non_air + layer1.non_air;
                        if (layer0.non_air == 0 && layer1.non_air == 0) continue;
                        entry["subchunks"].push_back({
                            { "y", y },
                            { "layer0_sha256", std::move(layer0.hash) },
                            { "layer1_sha256", std::move(layer1.hash) }
                        });
                        if (detail && detail->chunk_x == pos.x && detail->chunk_z == pos.z &&
                            detail->sub_y == y) {
                            const auto& layer = detail->layer == 0 ? sub.layer0 : sub.layer1;
                            nlohmann::json cells = nlohmann::json::array();
                            for (std::size_t index = 0; index < layer.size(); ++index) {
                                const auto state = registry.state(layer[index]);
                                if (!state) throw std::runtime_error("detail runtime ID 无法反查");
                                const auto canonical = water_structure::canonical_block_state(*state);
                                if (!canonical) throw std::runtime_error(canonical.error());
                                nlohmann::json properties = nlohmann::json::array();
                                auto sorted = state->states;
                                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                                    return a.name < b.name;
                                });
                                for (const auto& property : sorted) {
                                    properties.push_back({
                                        { "name", property.name },
                                        { "type", static_cast<std::uint8_t>(property.type) },
                                        { "value", property.value }
                                    });
                                }
                                cells.push_back({
                                    { "index", index },
                                    { "x", static_cast<std::int32_t>(index % 16) + pos.x * 16 },
                                    { "y", static_cast<std::int32_t>(index / 256) + y * 16 + 64 },
                                    { "z", static_cast<std::int32_t>((index / 16) % 16) + pos.z * 16 },
                                    { "state_sha256", sha256(canonical.value()) },
                                    { "name", state->name }, { "version", state->version },
                                    { "states", std::move(properties) }
                                });
                            }
                            manifest["detail"] = std::move(cells);
                        }
                    }
                }
                chunk_spool.append(entry);
                std::vector<nlohmann::json> local_entities;
                if (!detail) {
                    const auto entities = structure.get_chunk_nbt(
                        std::span<const water_structure::ChunkPos>(&pos, 1));
                    if (!entities) throw std::runtime_error(entities.error());
                    const auto found_entities = entities.value().find(pos);
                    if (found_entities != entities.value().end()) {
                        local_entities.reserve(found_entities->second.size());
                        for (const auto& entity : found_entities->second) {
                    auto canonical = water_structure::canonical_nbt(entity.payload);
                    if (!canonical) throw std::runtime_error(canonical.error());
                    auto fields = water_structure::canonical_nbt_fields(entity.payload);
                    if (!fields) throw std::runtime_error(fields.error());
                    nlohmann::json field_list = nlohmann::json::array();
                    for (const auto& field : fields.value()) {
                        field_list.push_back({
                            { "path", field.path },
                            { "type", field.value.empty() ? 0 : field.value.front() },
                            { "value_sha256", sha256(field.value) }
                        });
                    }
                    local_entities.push_back({
                        { "x", entity.pos.x }, { "y", entity.pos.y }, { "z", entity.pos.z },
                        { "nbt_sha256", sha256(canonical.value()) },
                        { "nbt_fields", std::move(field_list) }
                    });
                        }
                    }
                }
                std::sort(local_entities.begin(), local_entities.end(), [](const auto& a, const auto& b) {
                return std::tuple{
                    a["x"].template get<std::int32_t>(),
                    a["y"].template get<std::int32_t>(),
                    a["z"].template get<std::int32_t>(),
                    a["nbt_sha256"].template get<std::string>()
                } < std::tuple{
                    b["x"].template get<std::int32_t>(),
                    b["y"].template get<std::int32_t>(),
                    b["z"].template get<std::int32_t>(),
                    b["nbt_sha256"].template get<std::string>()
                };
                });
                for (const auto& entity : local_entities) entity_spool.append(entity);
                }
            }

            if (!detail && !manifest.contains("error")) {
                manifest["non_air_blocks"] = semantic_non_air;
            }

            // In detail mode the chunk spool contains one requested chunk, as
            // in the historical manifest; entities are intentionally empty.
            if (manifest.contains("error")) {
                // Keep the parse/detail error object and omit partial arrays.
                const auto failed = true;
                write_manifest_stream(output_path, std::move(manifest),
                    &chunk_spool, &entity_spool, false);
                return failed ? 2 : 0;
            }
            const auto failed = false;
            write_manifest_stream(output_path, std::move(manifest),
                &chunk_spool, &entity_spool, true);
            return failed ? 2 : 0;
        }
        const auto failed = manifest.contains("error");
        write_manifest_stream(output_path, std::move(manifest), nullptr, nullptr, false);
        return failed ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}

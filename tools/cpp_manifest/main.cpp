#include <WaterStructure/canonical.hpp>
#include <WaterStructure/format_registry.hpp>
#include <WaterStructure/runtime_registry.hpp>

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <charconv>
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

std::string sha256(std::span<const std::uint8_t> bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, hash_size = 0, result_size = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &result_size, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("初始化 SHA-256 失败");
    }
    std::vector<std::uint8_t> object(object_size), digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0 ||
        (!bytes.empty() && BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0) ||
        BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("计算 SHA-256 失败");
    }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    return hex(digest);
}

class Sha256Stream {
public:
    Sha256Stream()
    {
        DWORD result_size = 0;
        if (BCryptOpenAlgorithmProvider(&mAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(mAlgorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&mObjectSize),
                sizeof(mObjectSize), &result_size, 0) < 0 ||
            BCryptGetProperty(mAlgorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&mHashSize),
                sizeof(mHashSize), &result_size, 0) < 0) {
            throw std::runtime_error("初始化流式 SHA-256 失败");
        }
        mObject.resize(mObjectSize);
        if (BCryptCreateHash(mAlgorithm, &mHash, mObject.data(), mObjectSize, nullptr, 0, 0) < 0) {
            throw std::runtime_error("创建流式 SHA-256 失败");
        }
    }

    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;

    ~Sha256Stream()
    {
        if (mHash) BCryptDestroyHash(mHash);
        if (mAlgorithm) BCryptCloseAlgorithmProvider(mAlgorithm, 0);
    }

    void update(std::span<const std::uint8_t> bytes)
    {
        if (!bytes.empty() && BCryptHashData(mHash, const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()), 0) < 0) {
            throw std::runtime_error("更新流式 SHA-256 失败");
        }
    }

    void update(std::string_view bytes)
    {
        update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    }

    std::string finish()
    {
        std::vector<std::uint8_t> digest(mHashSize);
        if (BCryptFinishHash(mHash, digest.data(), mHashSize, 0) < 0) {
            throw std::runtime_error("完成流式 SHA-256 失败");
        }
        BCryptDestroyHash(mHash);
        mHash = nullptr;
        return hex(digest);
    }

private:
    BCRYPT_ALG_HANDLE mAlgorithm = nullptr;
    BCRYPT_HASH_HANDLE mHash = nullptr;
    DWORD mObjectSize = 0;
    DWORD mHashSize = 0;
    std::vector<std::uint8_t> mObject;
};

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

std::string layer_hash(
    const water_structure::BlockLayer& layer,
    const water_structure::RuntimeRegistry& registry,
    std::unordered_map<std::uint32_t, std::vector<std::uint8_t>>& cache)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(layer.size() * 32);
    for (const auto runtime_id : layer) {
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
    return sha256(bytes);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: cpp_manifest <input> [output.json [--format name] [--detail chunkX chunkZ subY layer]]\n";
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
            const auto non_air = structure.count_non_air_blocks();
            if (!non_air) throw std::runtime_error(non_air.error());
            manifest["non_air_blocks"] = non_air.value();
            std::vector<water_structure::ChunkPos> positions;
            for (std::int32_t x = 0; x < size.chunk_x_count(); ++x)
                for (std::int32_t z = 0; z < size.chunk_z_count(); ++z) positions.push_back({ x, z });
            auto chunks = structure.get_chunks(positions);
            if (!chunks) throw std::runtime_error(chunks.error());
            std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> state_cache;
            nlohmann::json chunk_list = nlohmann::json::array();
            for (const auto pos : positions) {
                nlohmann::json entry{ { "x", pos.x }, { "z", pos.z }, { "subchunks", nlohmann::json::array() } };
                const auto chunk = chunks.value().find(pos);
                if (chunk != chunks.value().end()) {
                    std::vector<std::int32_t> ys;
                    for (const auto& [y, _] : chunk->second.sub_chunks) ys.push_back(y);
                    std::sort(ys.begin(), ys.end());
                    for (const auto y : ys) {
                        const auto& sub = chunk->second.sub_chunks.at(y);
                        const auto layer0_non_air = std::ranges::any_of(sub.layer0, [&](const auto value) {
                            return value != registry.air_runtime_id();
                        });
                        const auto layer1_non_air = std::ranges::any_of(sub.layer1, [&](const auto value) {
                            return value != registry.air_runtime_id();
                        });
                        if (!layer0_non_air && !layer1_non_air) continue;
                        entry["subchunks"].push_back({
                            { "y", y },
                            { "layer0_sha256", layer_hash(sub.layer0, registry, state_cache) },
                            { "layer1_sha256", layer_hash(sub.layer1, registry, state_cache) }
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
                chunk_list.push_back(std::move(entry));
            }
            manifest["chunks"] = std::move(chunk_list);
            auto entities = structure.get_chunk_nbt(positions);
            if (!entities) throw std::runtime_error(entities.error());
            nlohmann::json entity_list = nlohmann::json::array();
            for (const auto& [_, values] : entities.value())
                for (const auto& entity : values) {
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
                    entity_list.push_back({
                        { "x", entity.pos.x }, { "y", entity.pos.y }, { "z", entity.pos.z },
                        { "nbt_sha256", sha256(canonical.value()) },
                        { "nbt_fields", std::move(field_list) }
                    });
                }
            std::sort(entity_list.begin(), entity_list.end(), [](const auto& a, const auto& b) {
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
            manifest["block_entities"] = std::move(entity_list);
        }
        const auto text = manifest.dump(2) + "\n";
        if (output_path) { std::ofstream output(*output_path, std::ios::binary | std::ios::trunc); output << text; if (!output) throw std::runtime_error("写入 manifest 失败"); }
        else std::cout << text;
        return manifest.contains("error") ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 3;
    }
}

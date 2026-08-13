#pragma once

#include "result.hpp"
#include "types.hpp"

#include <BedrockWorldOperator/BedrockWorldOperator.hpp>

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace water_structure {

class RuntimeRegistry {
public:
    RuntimeRegistry();

    std::uint32_t register_state(BlockState state);
    std::optional<std::uint32_t> find(
        std::string_view name,
        std::span<const BlockStateProperty> states = {},
        std::int32_t version = 0
    ) const;
    std::optional<std::uint32_t> find_compatible(BlockState state) const;
    std::optional<BlockState> state(std::uint32_t runtime_id) const;
    std::optional<BlockState> java_state(std::uint32_t runtime_id) const;
    std::unordered_map<std::uint32_t, BlockState> java_states_snapshot() const;

    Result<void> load_legacy_pool(const std::filesystem::path& path, std::uint8_t pool_id = 117);
    Result<void> load_block_mappings(const std::filesystem::path& path);
    std::vector<std::uint32_t> legacy_pool_snapshot(std::uint8_t pool_id) const;
    std::optional<std::uint32_t> legacy_runtime_id(std::uint8_t pool_id, std::size_t index) const;
    std::optional<std::uint32_t> legacy_runtime_id(std::string_view name, std::uint16_t data) const;
    std::optional<std::uint32_t> legacy_state_runtime_id(
        std::string_view name,
        std::span<const BlockStateProperty> states) const;
    std::optional<std::uint32_t> schematic_runtime_id(std::uint8_t block_id, std::uint8_t data) const;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> schematic_block(
        std::uint32_t runtime_id) const;
    std::optional<std::uint32_t> java_runtime_id(std::string_view block_state) const;
    std::optional<std::uint32_t> compatible_java_runtime_id(std::string_view block_state) const;

    std::uint32_t air_runtime_id() const noexcept { return mAirRuntimeId; }
    void install_as_bwo_resolver();

private:
    static std::string normalize_name(std::string_view name);
    static std::string normalize_legacy_name(std::string_view name);
    static std::string key_for(const BlockState& state);
    static std::string state_key_for(const BlockState& state);
    static std::string state_key_for(
        std::string_view name,
        std::span<const BlockStateProperty> states
    );
    static std::string key_for(
        std::string_view name,
        std::span<const BlockStateProperty> states,
        std::int32_t version
    );

    mutable std::mutex mMutex;
    std::uint32_t mNextRuntimeId = 1;
    std::uint32_t mAirRuntimeId = 0;
    std::unordered_map<std::string, std::uint32_t> mByKey;
    std::unordered_map<std::string, std::uint32_t> mByStateKey;
    std::unordered_map<std::string, std::uint32_t> mByName;
    std::unordered_map<std::uint32_t, BlockState> mByRuntimeId;
    std::unordered_map<std::uint32_t, BlockState> mJavaByRuntimeId;
    std::unordered_map<std::uint8_t, std::vector<std::uint32_t>> mLegacyPools;
    std::unordered_map<std::string, std::unordered_map<std::uint16_t, std::uint32_t>> mLegacyByName;
    std::unordered_map<std::string, std::uint32_t> mLegacyDefaultByName;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint32_t>> mLegacyByState;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> mLegacyStateOrder;
    struct JavaCandidateProperty {
        std::string_view name;
        std::string_view value;
    };
    struct JavaCandidate {
        std::string_view encoded;
        std::uint32_t runtime_id = 0;
        mutable std::optional<std::size_t> order;
        std::vector<JavaCandidateProperty> properties;
    };

    std::unordered_map<std::string, std::uint32_t> mJavaMapping;
    std::unordered_map<std::string, std::uint32_t> mJavaRoundTripMapping;
    std::unordered_map<std::string, std::vector<JavaCandidate>> mJavaCandidatesByName;
    std::unordered_map<std::string, std::uint32_t> mJavaDefaultByName;
    std::unordered_map<std::string, std::size_t> mJavaOrder;
    std::vector<std::uint32_t> mSchematicMapping;
    std::unordered_map<std::uint32_t, std::pair<std::uint8_t, std::uint8_t>> mSchematicReverse;
    bool mHasSchematicMapping = false;
    std::function<BlockState(BlockState)> mUpgradeState;
};

} // namespace water_structure

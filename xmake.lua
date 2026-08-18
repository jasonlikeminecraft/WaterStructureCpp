set_project("WaterStructureCpp")
set_version("0.1.0")
set_languages("cxx23")
set_encodings("utf-8")
add_defines("LEVELDB_COMPILE_LIBRARY")
if is_plat("windows") then
    add_cxxflags("/utf-8", "/bigobj", "/wd4838", "/wd4828", { tools = { "cl", "clang_cl" } })
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "LEVELDB_PLATFORM_WINDOWS")
else
    add_defines("LEVELDB_PLATFORM_POSIX")
end
add_rules("mode.debug", "mode.release")

local function apply_release_optimization()
    if is_mode("release") then
        set_optimize("fastest")
        if is_plat("windows") then
            add_cxxflags("/GL", { tools = { "cl", "clang_cl" } })
            add_ldflags("/LTCG", "/OPT:REF", "/OPT:ICF", { tools = { "link" } })
        end
    end
end

add_requires("nlohmann_json 3.11.3")
add_requires("msgpack-cxx 7.0.0")
add_requires("brotli 1.2.0")
add_requires("minizip-ng 4.2.1")
add_requires("doctest 2.5.3")
add_requires("zlib", { configs = { shared = false } })

target("leveldb")
    set_kind("static")
    set_languages("cxx20")
    apply_release_optimization()
    add_files(
        "thirdparty/LevelDB/db/builder.cc",
        "thirdparty/LevelDB/db/c.cc",
        "thirdparty/LevelDB/db/db_impl.cc",
        "thirdparty/LevelDB/db/db_iter.cc",
        "thirdparty/LevelDB/db/dbformat.cc",
        "thirdparty/LevelDB/db/dumpfile.cc",
        "thirdparty/LevelDB/db/filename.cc",
        "thirdparty/LevelDB/db/log_reader.cc",
        "thirdparty/LevelDB/db/log_writer.cc",
        "thirdparty/LevelDB/db/memtable.cc",
        "thirdparty/LevelDB/db/repair.cc",
        "thirdparty/LevelDB/db/table_cache.cc",
        "thirdparty/LevelDB/db/version_edit.cc",
        "thirdparty/LevelDB/db/version_set.cc",
        "thirdparty/LevelDB/db/write_batch.cc",
        "thirdparty/LevelDB/table/block.cc",
        "thirdparty/LevelDB/table/block_builder.cc",
        "thirdparty/LevelDB/table/filter_block.cc",
        "thirdparty/LevelDB/table/format.cc",
        "thirdparty/LevelDB/table/iterator.cc",
        "thirdparty/LevelDB/table/merger.cc",
        "thirdparty/LevelDB/table/table.cc",
        "thirdparty/LevelDB/table/table_builder.cc",
        "thirdparty/LevelDB/table/two_level_iterator.cc",
        "thirdparty/LevelDB/util/arena.cc",
        "thirdparty/LevelDB/util/bloom.cc",
        "thirdparty/LevelDB/util/cache.cc",
        "thirdparty/LevelDB/util/coding.cc",
        "thirdparty/LevelDB/util/comparator.cc",
        "thirdparty/LevelDB/util/crc32c.cc",
        "thirdparty/LevelDB/util/env.cc",
        "thirdparty/LevelDB/util/filter_policy.cc",
        "thirdparty/LevelDB/util/hash.cc",
        "thirdparty/LevelDB/util/logging.cc",
        "thirdparty/LevelDB/util/options.cc",
        "thirdparty/LevelDB/util/status.cc"
    )
    if is_plat("windows") then
        add_files("thirdparty/LevelDB/util/env_windows.cc")
    else
        add_files("thirdparty/LevelDB/util/env_posix.cc")
        add_cxxflags("-fPIC")
        if not is_plat("android") then
            add_syslinks("pthread", { public = true })
        end
    end
    add_includedirs("thirdparty/LevelDB/include", "thirdparty/LevelDB", { public = true })
    add_packages("zlib", { public = true })

target("bedrock_world_operator")
    set_kind("static")
    set_languages("cxx23")
    apply_release_optimization()
    if not is_plat("windows") then
        add_cxxflags("-fPIC")
    end
    add_files("thirdparty/BedrockWorldOperator/src/*.cpp")
    add_includedirs("thirdparty/BedrockWorldOperator/include", { public = true })
    add_deps("leveldb", { public = true })

target("libnbt")
    set_kind("static")
    set_languages("cxx20")
    apply_release_optimization()
    if not is_plat("windows") then
        add_cxxflags("-fPIC")
    end
    add_files(
        "thirdparty/libnbt/src/*.cpp",
        "thirdparty/libnbt/src/io/*.cpp",
        "thirdparty/libnbt/src/text/*.cpp"
    )
    add_includedirs("thirdparty/libnbt", { public = true })
    add_packages("zlib", { public = true })

target("water_structure")
    set_kind("static")
    set_languages("cxx23")
    set_symbols("debug")
    apply_release_optimization()
    add_files("src/c_api.cpp", "src/core/*.cpp", "src/world/*.cpp", "src/formats/*.cpp")
    add_includedirs("include", { public = true })
    add_deps("bedrock_world_operator", "libnbt", { public = true })
    add_packages("nlohmann_json", "msgpack-cxx", "brotli", "zlib", { public = true })

target("water_structure_shared")
    set_kind("shared")
    set_languages("cxx23")
    set_symbols("debug")
    add_defines("WATER_STRUCTURE_BUILD_SHARED", { public = true })
    apply_release_optimization()
    add_files("src/c_api.cpp", "src/core/*.cpp", "src/world/*.cpp", "src/formats/*.cpp")
    add_includedirs("include", { public = true })
    add_deps("bedrock_world_operator", "libnbt", { public = true })
    add_packages("nlohmann_json", "msgpack-cxx", "brotli", "zlib", { public = true })

target("water_structure_cli")
    set_kind("binary")
    set_languages("cxx23")
    set_symbols("debug")
    apply_release_optimization()
    add_files("src/cli/main.cpp")
    add_includedirs("include")
    add_deps("water_structure")

target("water_structure_tests")
    set_kind("binary")
    set_languages("cxx23")
    apply_release_optimization()
    add_files("tests/*.cpp")
    add_includedirs("include")
    add_deps("water_structure")
    if is_plat("windows") then
        add_syslinks("bcrypt")
    end

target("water_structure_bench")
    set_kind("binary")
    set_languages("cxx23")
    apply_release_optimization()
    add_files("benchmarks/*.cpp")
    add_includedirs("include")
    add_deps("water_structure")
    add_syslinks("psapi")

target("cpp_manifest")
    set_kind("binary")
    set_languages("cxx23")
    apply_release_optimization()
    add_files("tools/cpp_manifest/*.cpp")
    add_includedirs("include")
    add_deps("water_structure")
    add_packages("nlohmann_json")
    add_syslinks("bcrypt")

target("world_compare")
    set_kind("binary")
    set_languages("cxx23")
    apply_release_optimization()
    add_files("tools/world_compare/*.cpp")
    add_includedirs("include")
    add_deps("water_structure")

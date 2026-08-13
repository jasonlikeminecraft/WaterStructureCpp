vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jasonlikeminecraft/WaterStructureCpp
    REF "v${VERSION}"
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DWATER_STRUCTURE_BUILD_TOOLS=OFF -DWATER_STRUCTURE_BUILD_SHARED=ON)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/WaterStructure)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

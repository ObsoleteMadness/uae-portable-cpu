vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ObsoleteMadness/uae-portable-cpu
    REF "v${VERSION}"
    SHA512 0 # Updated during release workflow
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_TESTS=OFF
        -DBUILD_SHARED_LIBS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME uae-portable-cpu CONFIG_PATH lib/cmake/uae-portable-cpu)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

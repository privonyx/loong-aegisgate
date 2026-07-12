if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/sentencepiece
    REF "v${VERSION}"
    SHA512 012850b63b2323e16acc5dacc0a494ad3f6375425ee86274f0946032e47c088a3b307758b99d752fcf54acf76c82d7d13d0c14bbf07aa9b612c4f1fbd30cf1cf
    HEAD_REF master
    PATCHES
        abseil.diff
        linkage.diff
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" SPM_ENABLE_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSPM_ENABLE_SHARED=${SPM_ENABLE_SHARED}
        -DSPM_ENABLE_TCMALLOC=OFF
        -DSPM_ABSL_PROVIDER=package
        -DSPM_PROTOBUF_PROVIDER=package
        # sentencepiece 的 find_package(Protobuf) 走 CMake 内置 module 模式，在本仓库
        # 锁定的 protobuf 3.21.12 下只解析到完整库(protobuf::libprotobuf)、未创建
        # protobuf::libprotobuf-lite imported target，沿用上游写死的 -lite 目标名会链接成
        # "protobuf::libprotobuf-lite-NOTFOUND"。改指向必定存在的 protobuf::libprotobuf
        # (lite 的超集)：sentencepiece 由此把外部真 protobuf 作为独立库链入，libsentencepiece.a
        # 不再内置 protobuf 目标文件，从根上消除与 control-plane/otel 真 protobuf 的符号重复。
        -DPROTOBUF_LITE_LIBRARY=protobuf::libprotobuf
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

vcpkg_copy_tools(TOOL_NAMES spm_decode spm_encode spm_export_vocab spm_normalize spm_train AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

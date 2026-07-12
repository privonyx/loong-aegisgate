find_path(hnswlib_INCLUDE_DIR NAMES hnswlib/hnswlib.h)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(hnswlib DEFAULT_MSG hnswlib_INCLUDE_DIR)
if(hnswlib_FOUND AND NOT TARGET hnswlib::hnswlib)
    add_library(hnswlib::hnswlib INTERFACE IMPORTED)
    set_target_properties(hnswlib::hnswlib PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${hnswlib_INCLUDE_DIR}")
endif()

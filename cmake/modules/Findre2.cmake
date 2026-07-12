find_path(re2_INCLUDE_DIR NAMES re2/re2.h)
find_library(re2_LIBRARY NAMES re2)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(re2 DEFAULT_MSG re2_LIBRARY re2_INCLUDE_DIR)
if(re2_FOUND AND NOT TARGET re2::re2)
    add_library(re2::re2 UNKNOWN IMPORTED)
    set_target_properties(re2::re2 PROPERTIES
        IMPORTED_LOCATION "${re2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${re2_INCLUDE_DIR}")
endif()

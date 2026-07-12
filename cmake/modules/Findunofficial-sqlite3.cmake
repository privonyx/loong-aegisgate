find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(unofficial-sqlite3 DEFAULT_MSG SQLITE3_LIBRARY SQLITE3_INCLUDE_DIR)
if(unofficial-sqlite3_FOUND AND NOT TARGET unofficial::sqlite3::sqlite3)
    add_library(unofficial::sqlite3::sqlite3 UNKNOWN IMPORTED)
    set_target_properties(unofficial::sqlite3::sqlite3 PROPERTIES
        IMPORTED_LOCATION "${SQLITE3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}")
endif()

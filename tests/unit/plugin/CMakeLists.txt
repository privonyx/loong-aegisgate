add_library(sample_plugin SHARED sample_plugin.cpp)
target_include_directories(sample_plugin PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sample_plugin PRIVATE aegisgate_core)
set_target_properties(sample_plugin PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

add_executable(test_plugin_loader test_plugin_loader.cpp)
target_link_libraries(test_plugin_loader PRIVATE aegisgate_core GTest::gtest_main ${CMAKE_DL_LIBS})
target_compile_definitions(test_plugin_loader PRIVATE
    SAMPLE_PLUGIN_PATH="$<TARGET_FILE:sample_plugin>"
)
add_dependencies(test_plugin_loader sample_plugin)
add_test(NAME PluginLoaderTest COMMAND test_plugin_loader)

add_executable(test_rule_pack_manager test_rule_pack_manager.cpp)
target_link_libraries(test_rule_pack_manager PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME RulePackManagerTest COMMAND test_rule_pack_manager)

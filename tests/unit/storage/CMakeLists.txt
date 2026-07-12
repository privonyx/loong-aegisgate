add_executable(test_cache_store test_cache_store.cpp)
target_link_libraries(test_cache_store PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME CacheStoreTest COMMAND test_cache_store)

add_executable(test_persistent_store test_persistent_store.cpp)
target_link_libraries(test_persistent_store PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME PersistentStoreTest COMMAND test_persistent_store)

add_executable(test_config_version_store_default test_config_version_store_default.cpp)
target_link_libraries(test_config_version_store_default PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ConfigVersionStoreDefaultTest COMMAND test_config_version_store_default)

add_executable(test_config_version_store_memory test_config_version_store_memory.cpp)
target_link_libraries(test_config_version_store_memory PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ConfigVersionStoreMemoryTest COMMAND test_config_version_store_memory)

add_executable(test_config_version_store_sqlite test_config_version_store_sqlite.cpp)
target_link_libraries(test_config_version_store_sqlite PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ConfigVersionStoreSqliteTest COMMAND test_config_version_store_sqlite)

add_executable(test_sqlite_persistent_store test_sqlite_persistent_store.cpp)
target_link_libraries(test_sqlite_persistent_store PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME SQLitePersistentStoreTest COMMAND test_sqlite_persistent_store)

# TASK-20260617-02 Epic 1 — savings_events persistence (memory + sqlite).
add_executable(test_savings_events_store test_savings_events_store.cpp)
target_link_libraries(test_savings_events_store PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME SavingsEventsStoreTest COMMAND test_savings_events_store)

# Phase 9.3.4 RolloutController (TASK-20260422-01)
# Covers Memory backend in A.4; A.5 extends this TU with SQLite fixture.
# TASK-20260507-01 Epic 3.1 adds the PG backend behind ENABLE_PG so the typed
# test matrix becomes Memory × SQLite × PG when both flags are on.
add_executable(test_rollout_storage test_rollout_storage.cpp)
target_link_libraries(test_rollout_storage PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME RolloutStorageTest COMMAND test_rollout_storage)
if(ENABLE_PG)
    target_compile_definitions(test_rollout_storage PRIVATE AEGISGATE_TEST_PG=1)
    target_link_libraries(test_rollout_storage PRIVATE PostgreSQL::PostgreSQL)
endif()

add_executable(test_connection_pool test_connection_pool.cpp)
target_link_libraries(test_connection_pool PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ConnectionPoolTest COMMAND test_connection_pool)

# Phase 11.5 AutonomyApprovalWorkflow (TASK-20260518-02 Epic 1.0)
# Typed-test fixture covering Memory + SQLite backends. PG counterpart is
# embedded in test_pg_persistent_store.cpp to share the no-real-PG pattern.
add_executable(test_persistent_store_approvals
    test_persistent_store_approvals.cpp)
target_link_libraries(test_persistent_store_approvals
    PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME PersistentStoreApprovalsTest COMMAND test_persistent_store_approvals)

if(ENABLE_REDIS)
    add_executable(test_redis_cache_store test_redis_cache_store.cpp)
    target_link_libraries(test_redis_cache_store PRIVATE aegisgate_core GTest::gtest_main)
    add_test(NAME RedisCacheStoreTest COMMAND test_redis_cache_store)
endif()

add_executable(test_memory_rbac test_memory_rbac.cpp)
target_link_libraries(test_memory_rbac PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME MemoryRbacTest COMMAND test_memory_rbac)

add_executable(test_sqlite_rbac test_sqlite_rbac.cpp)
target_link_libraries(test_sqlite_rbac PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME SQLiteRbacTest COMMAND test_sqlite_rbac)

if(ENABLE_PG)
    add_executable(test_pg_persistent_store test_pg_persistent_store.cpp)
    target_link_libraries(test_pg_persistent_store PRIVATE aegisgate_core GTest::gtest_main)
    add_test(NAME PgPersistentStoreTest COMMAND test_pg_persistent_store)
endif()

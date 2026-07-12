# Phase 11.5 AutonomyApprovalWorkflow tests (TASK-20260518-02).
# Each Epic 1 sub-task adds a binary here as it lands.

add_executable(test_approval_state test_approval_state.cpp)
target_link_libraries(test_approval_state PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ApprovalStateTest COMMAND test_approval_state)

add_executable(test_approval_proposal test_approval_proposal.cpp)
target_link_libraries(test_approval_proposal PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ApprovalProposalTest COMMAND test_approval_proposal)

add_executable(test_i_approval_applier test_i_approval_applier.cpp)
target_link_libraries(test_i_approval_applier PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME IApprovalApplierTest COMMAND test_i_approval_applier)

add_executable(test_approval_queue test_approval_queue.cpp)
target_link_libraries(test_approval_queue PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME ApprovalQueueTest COMMAND test_approval_queue)

add_executable(test_approval_workflow test_approval_workflow.cpp)
target_link_libraries(test_approval_workflow PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME AutonomyApprovalWorkflowTest COMMAND test_approval_workflow)

add_executable(test_decision_trace_pii test_decision_trace_pii.cpp)
target_link_libraries(test_decision_trace_pii PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME DecisionTracePiiTest COMMAND test_decision_trace_pii)

add_executable(test_cost_autonomy_applier test_cost_autonomy_applier.cpp)
target_link_libraries(test_cost_autonomy_applier PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME CostAutonomyApplierTest COMMAND test_cost_autonomy_applier)

add_executable(test_bandit_autonomy_applier test_bandit_autonomy_applier.cpp)
target_link_libraries(test_bandit_autonomy_applier PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME BanditAutonomyApplierTest COMMAND test_bandit_autonomy_applier)

# TASK-20260523-03 Epic 1 — BaseAutonomyApplier (GoF Template Method base).
add_executable(test_base_autonomy_applier test_base_autonomy_applier.cpp)
target_link_libraries(test_base_autonomy_applier PRIVATE aegisgate_core GTest::gtest_main)
add_test(NAME BaseAutonomyApplierTest COMMAND test_base_autonomy_applier)

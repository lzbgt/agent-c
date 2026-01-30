cmake_minimum_required(VERSION 3.20)

project(agent LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AGENT_BUILD_HOST "Build CLI/daemon host components (requires CURL; JSONCPP optional)" ON)

add_library(agent_core STATIC
  core/src/agent_alloc.c
  core/src/agent_runner.c
  core/src/agent_session.c
  core/src/agent_session_codec.c
  core/src/agent_string.c
  core/src/agent_tool_loop.c
  core/src/agent_tool_provider.c
  core/src/agent_tools.c
)

target_include_directories(agent_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

include(FindPkgConfig)
set(AGENT_ENABLE_JSONCPP OFF)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(JSONCPP jsoncpp)
  if(JSONCPP_FOUND)
    set(AGENT_ENABLE_JSONCPP ON)
  endif()
endif()

target_compile_options(agent_core PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic>
)

if(AGENT_BUILD_HOST)
  find_package(CURL REQUIRED)
  if(NOT AGENT_ENABLE_JSONCPP)
    message(FATAL_ERROR "AGENT_BUILD_HOST=ON requires jsoncpp (pkg-config: jsoncpp). Use -DAGENT_BUILD_HOST=OFF for core-only builds.")
  endif()

  # Host-side adapter library shared by CLI and daemon.
  add_library(agent_host STATIC
    cli/src/default_system_prompt.cpp
    cli/src/file_persistor.cpp
    cli/src/openai_client.cpp
    cli/src/openai_provider.cpp
    cli/src/openai_tool_provider.cpp
    cli/src/sse_parser.cpp
    cli/src/session_store.cpp
    cli/src/summary_compaction.cpp
    cli/src/summary_llm.cpp
    cli/src/tool_loop.cpp
    cli/src/tool_loop_compaction.cpp
    cli/src/tool_loop_truncation.cpp
    cli/src/toolset_basic.cpp
    cli/src/toolset_host.cpp
    cli/src/toolset_host_exec.cpp
  )

  target_compile_options(agent_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )

  target_include_directories(agent_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/src
  )

  target_link_libraries(agent_host PUBLIC agent_core CURL::libcurl)

  if(AGENT_ENABLE_JSONCPP)
    target_include_directories(agent_host PUBLIC ${JSONCPP_INCLUDE_DIRS})
    target_link_directories(agent_host PUBLIC ${JSONCPP_LIBRARY_DIRS})
    target_link_libraries(agent_host PUBLIC ${JSONCPP_LIBRARIES})
    target_compile_definitions(agent_host PUBLIC AGENT_HAVE_JSONCPP=1)
    target_compile_definitions(agent_core PRIVATE AGENT_HAVE_JSONCPP=1)
  endif()

  add_executable(agent
    cli/src/main.cpp
  )

  target_compile_options(agent PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )

  target_link_libraries(agent PRIVATE agent_host)

  add_executable(agentd
    daemon/src/main.cpp
    daemon/src/config_endpoint.cpp
    daemon/src/cors.cpp
    daemon/src/daemon_auth.cpp
    daemon/src/file_endpoint.cpp
    daemon/src/http_util.cpp
    daemon/src/http_server.cpp
    daemon/src/job_endpoints.cpp
    daemon/src/job_stream_endpoint.cpp
    daemon/src/job_manager.cpp
    daemon/src/json_util.cpp
    daemon/src/openrouter_models_endpoint.cpp
    daemon/src/run_endpoints.cpp
    daemon/src/session_endpoints.cpp
    daemon/src/tools_endpoint.cpp
    daemon/src/sandbox_policy.cpp
    daemon/src/openrouter_util.cpp
    daemon/src/string_util.cpp
  )

  target_compile_options(agentd PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )

  target_link_libraries(agentd PRIVATE agent_host)
endif()

enable_testing()
add_executable(agent_core_tests
  tests/test_session.c
  tests/test_session_codec.c
  tests/test_tool_loop.c
  tests/test_runner.c
)
target_link_libraries(agent_core_tests PRIVATE agent_core)
add_test(NAME agent_core_tests COMMAND agent_core_tests)

if(AGENT_BUILD_HOST)
  add_test(NAME agent_openrouter_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_openrouter_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_host_tools_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_host_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_openrouter_fs_read_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_fs_read_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_fs_read_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_openrouter_fs_find_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_fs_find_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_fs_find_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_tools_list_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tools_list_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_auth_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_auth_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_config_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_config_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_sandbox_tighten_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sandbox_tighten_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_cors_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_cors_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_file_symlink_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_file_symlink_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_local_tool_loop_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_tool_loop_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_session_audit_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_session_audit_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_local_async_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_async_host_tools_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_local_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_stream_assistant_smoke.sh $<TARGET_FILE:agentd>)

  add_test(
    NAME agentd_non_loopback_auth_guard_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_non_loopback_auth_guard_smoke.sh $<TARGET_FILE:agentd>
  )

  add_test(NAME agentd_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_host_tools_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_host_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_stream_assistant_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_stream_assistant_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_sse_auth_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sse_auth_smoke.sh $<TARGET_FILE:agentd>)

  add_test(NAME agentd_openrouter_models_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_openrouter_models_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_openrouter_models_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_async_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_async_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_async_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_sse_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sse_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_sse_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agentd_cancel_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_cancel_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_cancel_smoke PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME agentd_heartbeat_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_heartbeat_smoke.sh $<TARGET_FILE:agentd>)
  set_tests_properties(agentd_heartbeat_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_deepseek_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_deepseek_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_deepseek_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_tools_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_deepseek_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_deepseek_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_host_tools_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_deepseek_host_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_deepseek_fs_read_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_fs_read_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_deepseek_fs_read_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_deepseek_fs_find_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_fs_find_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_deepseek_fs_find_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_deepseek_reasoner_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_reasoner_tools_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_deepseek_reasoner_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(AGENT_BUILD_HOST AND AGENT_ENABLE_JSONCPP)
  add_executable(host_toolset_tests
    tests/test_host_toolset.cpp
  )
  target_link_libraries(host_toolset_tests PRIVATE agent_host)
  add_test(NAME host_toolset_tests COMMAND host_toolset_tests)

  add_executable(session_store_tests
    tests/test_session_store.cpp
  )
  target_link_libraries(session_store_tests PRIVATE agent_host)
  add_test(NAME session_store_tests COMMAND session_store_tests)

  add_executable(file_persistor_tests
    tests/test_file_persistor.cpp
  )
  target_link_libraries(file_persistor_tests PRIVATE agent_host)
  add_test(NAME file_persistor_tests COMMAND file_persistor_tests)

  add_executable(job_manager_tests
    tests/test_job_manager.cpp
    daemon/src/job_manager.cpp
    daemon/src/json_util.cpp
  )
  target_include_directories(job_manager_tests PRIVATE daemon/src ${JSONCPP_INCLUDE_DIRS})
  target_link_directories(job_manager_tests PRIVATE ${JSONCPP_LIBRARY_DIRS})
  target_link_libraries(job_manager_tests PRIVATE ${JSONCPP_LIBRARIES})
  add_test(NAME job_manager_tests COMMAND job_manager_tests)

  add_executable(sse_parser_tests
    tests/test_sse_parser.cpp
    cli/src/sse_parser.cpp
  )
  target_include_directories(sse_parser_tests PRIVATE cli/src)
  add_test(NAME sse_parser_tests COMMAND sse_parser_tests)
  set_tests_properties(sse_parser_tests PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(tool_loop_compaction_tests
    tests/test_tool_loop_compaction.cpp
  )
  target_link_libraries(tool_loop_compaction_tests PRIVATE agent_host)
  add_test(NAME tool_loop_compaction_tests COMMAND tool_loop_compaction_tests)

  add_executable(tool_loop_truncation_tests
    tests/test_tool_loop_truncation.cpp
  )
  target_link_libraries(tool_loop_truncation_tests PRIVATE agent_host)
  add_test(NAME tool_loop_truncation_tests COMMAND tool_loop_truncation_tests)

  add_executable(summary_compaction_tests
    tests/test_summary_compaction.cpp
  )
  target_link_libraries(summary_compaction_tests PRIVATE agent_host)
  add_test(NAME summary_compaction_tests COMMAND summary_compaction_tests)

  add_executable(openai_error_tests
    tests/test_openai_errors.cpp
  )
  target_link_libraries(openai_error_tests PRIVATE agent_host)
  add_test(NAME openai_error_tests COMMAND openai_error_tests)

  add_executable(openai_provider_tests
    tests/test_openai_provider.cpp
  )
  target_link_libraries(openai_provider_tests PRIVATE agent_host)
  add_test(NAME openai_provider_tests COMMAND openai_provider_tests)
 endif()

if(AGENT_BUILD_HOST)
  add_executable(cors_tests
    tests/test_cors.cpp
    daemon/src/cors.cpp
  )
  target_include_directories(cors_tests PRIVATE daemon/src)
  add_test(NAME cors_tests COMMAND cors_tests)
endif()

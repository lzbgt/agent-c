cmake_minimum_required(VERSION 3.20)

project(agent LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AGENT_BUILD_HOST "Build CLI/daemon host components (requires CURL; JSONCPP optional)" ON)
option(AGENTD_ENABLE_HTTP "Build agentd HTTP server + agentd executable" ON)
option(AGENT_BUILD_ESP32SIM "Build esp32sim host harness for embedded agent_core" ON)

add_library(agent_core STATIC
  core/src/agent_alloc.c
  core/src/agent_cbor_det.c
  core/src/agent_cbor_read.c
  core/src/agent_base64.c
  core/src/agent_ed25519.c
  core/src/agent_edge_interop.c
  core/src/agent_hmac_sha256.c
  core/src/agent_json_c14n.c
  core/src/agent_multimodal_prefix.c
  core/src/agent_um_eais_task_assign_read.c
  core/src/agent_um_eais_task_lifecycle_write.c
  core/src/agent_um_eais_node_write.c
  core/src/agent_um_eais_node_caps_rsp_write.c
  core/src/agent_um_eais_outbox_read.c
  core/src/agent_um_eais_platform_caps_req_read.c
  core/src/agent_umbmp_envelope_read.c
  core/src/agent_umbmp_auth.c
  core/src/agent_runner.c
  core/src/agent_session.c
  core/src/agent_session_codec.c
  core/src/agent_sha256.c
  core/src/agent_sse_parser.c
  core/src/agent_stream_decoder.c
  core/src/agent_string.c
  core/src/agent_tool_loop.c
  core/src/agent_tool_loop_json.c
  core/src/agent_tool_provider.c
  core/src/agent_tools.c
  core/third_party/ed25519_donna/ed25519.c
)

target_include_directories(agent_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

target_include_directories(agent_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/core/third_party/ed25519_donna
)

include(FindPkgConfig)
set(AGENT_ENABLE_JSONCPP OFF)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(JSONCPP jsoncpp)
  if(JSONCPP_FOUND)
    set(AGENT_ENABLE_JSONCPP ON)
  endif()
endif()

set(AGENT_ENABLE_SQLITE3 OFF)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(SQLITE3 sqlite3)
  if(SQLITE3_FOUND)
    set(AGENT_ENABLE_SQLITE3 ON)
  endif()
endif()

target_compile_options(agent_core PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic>
)

target_compile_definitions(agent_core PRIVATE
  ED25519_REFHASH=1
  ED25519_TEST=1
  ED25519_NO_INLINE_ASM=1
)

# Small helper used by smoke tests (sign/print pubkey).
add_executable(agent_ed25519_tool
  tools/agent_ed25519_tool.cpp
)
target_compile_options(agent_ed25519_tool PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
)
target_link_libraries(agent_ed25519_tool PRIVATE agent_core)

  # Host-side helper for validating CBOR outbox bytes with the embedded decoder.
  if(AGENT_BUILD_HOST)
    find_package(CURL REQUIRED)
    if(NOT AGENT_ENABLE_JSONCPP)
      message(FATAL_ERROR "AGENT_BUILD_HOST=ON requires jsoncpp (pkg-config: jsoncpp). Use -DAGENT_BUILD_HOST=OFF for core-only builds.")
    endif()

    # Host-side helper for generating CBOR UM-BMP envelopes using the embedded encoder.
    add_executable(agent_core_umbmp_cbor_encode
      tools/agent_core_umbmp_cbor_encode.cpp
    )
    target_compile_options(agent_core_umbmp_cbor_encode PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
    target_link_libraries(agent_core_umbmp_cbor_encode PRIVATE agent_core)

  # Host-side helper for validating CBOR outbox bytes with the embedded decoder.
  add_executable(agent_core_outbox_cbor_check
    tools/agent_core_outbox_cbor_check.cpp
  )
  target_compile_options(agent_core_outbox_cbor_check PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )
  target_link_libraries(agent_core_outbox_cbor_check PRIVATE agent_core)

  # Host-side adapter library shared by CLI and daemon.
  add_library(agent_host STATIC
    cli/src/base64.cpp
    cli/src/cli_transcript_writer.cpp
    cli/src/default_system_prompt.cpp
    cli/src/file_persistor.cpp
    cli/src/openai_client.cpp
    cli/src/openai_provider.cpp
    cli/src/openai_stream_decoder.cpp
    cli/src/openai_stream_adapter.cpp
    cli/src/openai_tool_provider.cpp
    cli/src/session_store.cpp
    cli/src/summary_compaction.cpp
    cli/src/summary_llm.cpp
    cli/src/memory_index.cpp
    cli/src/tool_loop.cpp
    cli/src/tool_loop_compaction.cpp
    cli/src/tool_loop_truncation.cpp
    cli/src/toolset_basic.cpp
    cli/src/toolset_host.cpp
    cli/src/toolset_host_fs.cpp
    cli/src/toolset_host_artifacts.cpp
    cli/src/toolset_host_exec.cpp
    cli/src/toolset_host_ui_wait_event.cpp
    cli/src/toolset_host_client_peek.cpp
    cli/src/toolset_host_memory.cpp
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

  if(AGENT_ENABLE_SQLITE3)
    target_include_directories(agent_host PUBLIC ${SQLITE3_INCLUDE_DIRS})
    target_link_directories(agent_host PUBLIC ${SQLITE3_LIBRARY_DIRS})
    target_link_libraries(agent_host PUBLIC ${SQLITE3_LIBRARIES})
    target_compile_definitions(agent_host PUBLIC AGENT_HAVE_SQLITE3=1)
  endif()

  add_executable(agent
    cli/src/main.cpp
  )

  target_compile_options(agent PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )

  target_link_libraries(agent PRIVATE agent_host)

  if(AGENT_BUILD_ESP32SIM)
    add_executable(esp32sim
      tools/esp32sim.cpp
    )

    target_compile_options(esp32sim PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )

    target_link_libraries(esp32sim PRIVATE agent_host)
  endif()

  # Build the daemon as an embeddable library. HTTP serving is optional and controlled by
  # `AGENTD_ENABLE_HTTP` (useful for app embedding where a cloud broker / custom transport is used).
  set(AGENTD_LIB_SOURCES
    daemon/src/agent_db.cpp
    daemon/src/agent_db_state.cpp
    daemon/src/agent_db_sessions.cpp
    daemon/src/agent_db_audit.cpp
    daemon/src/agent_db_jobs.cpp
    daemon/src/agent_db_runs.cpp
    daemon/src/agent_db_blobs.cpp
    daemon/src/agent_db_stats.cpp
    daemon/src/agent_db_edge_nodes.cpp
    daemon/src/agent_db_edge_tasks.cpp
    daemon/src/agent_db_edge_workflows.cpp
    daemon/src/agentd_api.cpp
    daemon/src/client_profiles.cpp
    daemon/src/config_endpoint.cpp
    daemon/src/caps_endpoint.cpp
    daemon/src/avm_endpoints.cpp
    daemon/src/cbor_decode.cpp
    daemon/src/cbor_encode.cpp
    daemon/src/cors.cpp
    daemon/src/db_query_endpoints.cpp
    daemon/src/db_query_blob_endpoints.cpp
    daemon/src/db_query_edge_endpoints.cpp
    daemon/src/daemon_auth.cpp
    daemon/src/diagnostics_endpoints.cpp
    daemon/src/edge_interop_auth.cpp
    daemon/src/edge_interop_endpoints.cpp
    daemon/src/edge_deadline_sweeper.cpp
    daemon/src/edge_rules.cpp
    daemon/src/edge_util.cpp
    daemon/src/edge_workflow_endpoints.cpp
    daemon/src/edge_workflow_engine.cpp
    daemon/src/blob_endpoints.cpp
    daemon/src/blob_object_store.cpp
    daemon/src/blob_tier_policy.cpp
    daemon/src/file_endpoint.cpp
    daemon/src/http_client.cpp
    daemon/src/http_allowlist.cpp
    daemon/src/http_util.cpp
    daemon/src/net_compat.cpp
    daemon/src/job_endpoints.cpp
    daemon/src/job_manager.cpp
    daemon/src/job_engine.cpp
    daemon/src/health_endpoint.cpp
    daemon/src/json_util.cpp
    daemon/src/llm_usage.cpp
    daemon/src/openrouter_models_endpoint.cpp
    daemon/src/orchestrate_endpoints.cpp
    daemon/src/ota_endpoints.cpp
    daemon/src/drain_state.cpp
    daemon/src/provider_util.cpp
    daemon/src/memory_consolidator.cpp
    daemon/src/memory_retention.cpp
    daemon/src/memory_checkpoints.cpp
    daemon/src/memory_salience.cpp
    daemon/src/memory_recaps.cpp
    daemon/src/memory_endpoints.cpp
    daemon/src/runtime_config.cpp
    daemon/src/secrets_file.cpp
    daemon/src/run_client_acks.cpp
    daemon/src/run_provider_keys.cpp
    daemon/src/run_request.cpp
    daemon/src/run_request_config.cpp
    daemon/src/run_request_parse.cpp
    daemon/src/run_request_persist.cpp
    daemon/src/run_request_tool_loop.cpp
    daemon/src/run_request_replay.cpp
    daemon/src/run_endpoints.cpp
    daemon/src/run_replay_endpoint.cpp
    daemon/src/run_endpoints_internal.cpp
    daemon/src/run_memory_context.cpp
    daemon/src/run_multimodal.cpp
    daemon/src/trace_endpoints.cpp
    daemon/src/trace_id_util.cpp
    daemon/src/scene_store.cpp
    daemon/src/session_endpoints.cpp
    daemon/src/tool_plugins.cpp
    daemon/src/tool_servers.cpp
    daemon/src/tool_extension_mux.cpp
    daemon/src/tools_endpoint.cpp
    daemon/src/sandbox_policy.cpp
    daemon/src/openrouter_util.cpp
    daemon/src/string_util.cpp
    daemon/src/workflow_aggregate.cpp
    daemon/src/workflow_engine_common.cpp
    daemon/src/workflow_engine.cpp
    daemon/src/workflow_engine_pick.cpp
    daemon/src/workflow_endpoint_util.cpp
    daemon/src/workflow_http_json.cpp
    daemon/src/workflow_agentd_call.cpp
    daemon/src/workflow_memory_correlate.cpp
    daemon/src/workflow_memory_query.cpp
    daemon/src/workflow_memory_ops.cpp
    daemon/src/workflow_templates.cpp
    daemon/src/workflow_query_endpoints.cpp
    daemon/src/workflow_admin_endpoints.cpp
    daemon/src/workflow_endpoints.cpp
    daemon/src/workflow_submit.cpp
    daemon/src/workflow_submit_macros.cpp
    daemon/src/workflow_submit_task_builders.cpp
    daemon/src/workflow_fairq_cost.cpp
  )

  if(AGENTD_ENABLE_HTTP)
    list(APPEND AGENTD_LIB_SOURCES
      daemon/src/agentd_service.cpp
      daemon/src/http_server.cpp
      daemon/src/job_stream_endpoint.cpp
      daemon/src/edge_workflow_stream_endpoint.cpp
      daemon/src/workflow_stream_endpoint.cpp
    )
  endif()

  add_library(agentd_lib STATIC ${AGENTD_LIB_SOURCES})

  target_compile_options(agentd_lib PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )

  target_include_directories(agentd_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon/include
    ${CMAKE_CURRENT_SOURCE_DIR}/daemon/src
  )

  target_link_libraries(agentd_lib PUBLIC agent_host)
  if(WIN32)
    target_link_libraries(agentd_lib PUBLIC ws2_32)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(agentd_lib PUBLIC dl)
  endif()

  if(AGENT_ENABLE_SQLITE3)
    target_include_directories(agentd_lib PRIVATE ${SQLITE3_INCLUDE_DIRS})
    target_link_directories(agentd_lib PRIVATE ${SQLITE3_LIBRARY_DIRS})
    target_link_libraries(agentd_lib PUBLIC ${SQLITE3_LIBRARIES})
    target_compile_definitions(agentd_lib PRIVATE AGENT_HAVE_SQLITE3=1)
  endif()

  if(AGENTD_ENABLE_HTTP)
    add_executable(agentd
      daemon/src/main.cpp
      daemon/src/daemon_cli.cpp
    )

    target_compile_options(agentd PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )

    target_link_libraries(agentd PRIVATE agentd_lib)
    target_compile_definitions(agentd PRIVATE AGENTD_ENABLE_HTTP=1)
  endif()

  # Demo tool plugin (dlopen-loaded by agentd via --tool-plugin). Also used by a smoke test.
  add_library(agentd_tool_plugin_echo MODULE
    tools/agentd_tool_plugin_echo.c
  )
  target_compile_options(agentd_tool_plugin_echo PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic>
  )

  # Test plugin that returns a large payload to exercise daemon caps.
  add_library(agentd_tool_plugin_big MODULE
    tools/agentd_tool_plugin_big.c
  )
  target_compile_options(agentd_tool_plugin_big PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic>
  )

  add_executable(agentd_tool_plugin_host
    tools/agentd_tool_plugin_host.cpp
  )
  target_compile_options(agentd_tool_plugin_host PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )
  target_link_libraries(agentd_tool_plugin_host PRIVATE agentd_lib)

  if(AGENTD_ENABLE_HTTP)
    target_compile_definitions(agentd_lib PUBLIC AGENTD_ENABLE_HTTP=1)
  endif()
endif()

enable_testing()
add_executable(agent_core_tests
  tests/test_base64.c
  tests/test_cbor_det.c
  tests/test_cbor_read.c
  tests/test_edge_interop.c
  tests/test_um_eais_task_assign_read.c
  tests/test_um_eais_task_lifecycle_write.c
  tests/test_um_eais_node_write.c
  tests/test_um_eais_node_caps_rsp_write.c
  tests/test_um_eais_platform_caps_req_read.c
  tests/test_um_eais_outbox_read.c
  tests/test_umbmp_envelope_read.c
  tests/test_umbmp_envelope_write.c
  tests/test_umbmp_auth_core.c
  tests/test_ed25519.c
  tests/test_hmac_sha256.c
  tests/test_json_c14n.c
  tests/test_session.c
  tests/test_session_codec.c
  tests/test_sse_parser.c
  tests/test_stream_decoder.c
  tests/test_tool_loop.c
  tests/test_tool_loop_memory_flush.c
  tests/test_runner.c
)
target_link_libraries(agent_core_tests PRIVATE agent_core)
add_test(NAME agent_core_tests COMMAND agent_core_tests)

if(AGENT_BUILD_HOST)
  add_executable(umbmp_auth_vectors_tool
    tools/umbmp_auth_vectors_tool.cpp
  )
  target_compile_options(umbmp_auth_vectors_tool PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )
  target_link_libraries(umbmp_auth_vectors_tool PRIVATE agentd_lib)

  add_executable(docs_sanity_tests
    tests/test_docs_sanity.cpp
  )
  add_test(NAME docs_sanity_tests COMMAND docs_sanity_tests ${CMAKE_CURRENT_SOURCE_DIR}/docs/DB.md)

  add_executable(openapi_sanity_tests
    tests/test_openapi_sanity.cpp
  )
  add_test(
    NAME openapi_sanity_tests
    COMMAND openapi_sanity_tests
      ${CMAKE_CURRENT_SOURCE_DIR}/docs/openapi/agentd.yaml
      ${CMAKE_CURRENT_SOURCE_DIR}/docs/openapi/broker.yaml
  )

  add_executable(um_eais_spec_sanity_tests
    tests/test_um_eais_spec_sanity.cpp
  )
  target_link_libraries(um_eais_spec_sanity_tests PRIVATE ${JSONCPP_LIBRARIES})
  target_include_directories(um_eais_spec_sanity_tests PRIVATE ${JSONCPP_INCLUDE_DIRS})
  target_link_directories(um_eais_spec_sanity_tests PRIVATE ${JSONCPP_LIBRARY_DIRS})
  add_test(
    NAME um_eais_spec_sanity_tests
    COMMAND um_eais_spec_sanity_tests ${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/um-eais
  )

  add_executable(run_events_spec_sanity_tests
    tests/test_run_events_spec_sanity.cpp
  )
  target_link_libraries(run_events_spec_sanity_tests PRIVATE ${JSONCPP_LIBRARIES})
  target_include_directories(run_events_spec_sanity_tests PRIVATE ${JSONCPP_INCLUDE_DIRS})
  target_link_directories(run_events_spec_sanity_tests PRIVATE ${JSONCPP_LIBRARY_DIRS})
  add_test(
    NAME run_events_spec_sanity_tests
    COMMAND run_events_spec_sanity_tests ${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/run-events
  )

  add_executable(run_events_fixture_sanity_tests
    tests/test_run_events_fixture_sanity.cpp
  )
  target_link_libraries(run_events_fixture_sanity_tests PRIVATE ${JSONCPP_LIBRARIES})
  target_include_directories(run_events_fixture_sanity_tests PRIVATE ${JSONCPP_INCLUDE_DIRS})
  target_link_directories(run_events_fixture_sanity_tests PRIVATE ${JSONCPP_LIBRARY_DIRS})
  add_test(
    NAME run_events_fixture_sanity_tests
    COMMAND run_events_fixture_sanity_tests ${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/run-events/fixtures/run_events_v1.jsonl
  )

  add_executable(umbmp_auth_vectors_tests
    tests/test_umbmp_auth_vectors.cpp
  )
  target_compile_options(umbmp_auth_vectors_tests PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )
  target_link_libraries(umbmp_auth_vectors_tests PRIVATE agentd_lib)
  add_test(
    NAME umbmp_auth_vectors_tests
    COMMAND umbmp_auth_vectors_tests ${CMAKE_CURRENT_SOURCE_DIR}/docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json
  )

  add_executable(cbor_det_roundtrip_tests
    tests/test_cbor_det_roundtrip.cpp
  )
  target_compile_options(cbor_det_roundtrip_tests PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  )
  target_link_libraries(cbor_det_roundtrip_tests PRIVATE agentd_lib)
  add_test(NAME cbor_det_roundtrip_tests COMMAND cbor_det_roundtrip_tests)

  if(WIN32)
    add_executable(tool_plugin_host_limits_tests
      tests/test_tool_plugin_host_limits.cpp
    )
    add_test(
      NAME agentd_tool_plugin_host_limits_windows
      COMMAND tool_plugin_host_limits_tests
        $<TARGET_FILE:agentd_tool_plugin_host>
        $<TARGET_FILE:agentd_tool_plugin_echo>
    )
  endif()

  add_test(NAME agent_openrouter_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_openrouter_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_host_tools_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_host_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_openrouter_fs_read_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_fs_read_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_fs_read_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_openrouter_fs_find_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_fs_find_smoke.sh $<TARGET_FILE:agent>)
  set_tests_properties(agent_openrouter_fs_find_smoke PROPERTIES SKIP_RETURN_CODE 77)

  add_test(NAME agent_local_stream_tool_loop_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_local_stream_tool_loop_smoke.sh $<TARGET_FILE:agent>)
  add_test(NAME agent_local_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_local_stream_assistant_smoke.sh $<TARGET_FILE:agent>)

  if(AGENTD_ENABLE_HTTP)
    add_test(NAME agentd_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME agentd_tools_list_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tools_list_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_avm_job_scan_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_avm_job_scan_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_auth_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_auth_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_config_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_config_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_config_update_provider_validation_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_config_update_provider_validation_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_diagnostics_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_diagnostics_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_secrets_dotenv_path_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_secrets_dotenv_path_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_config_update_workflow_http_policy_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_config_update_workflow_http_policy_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_sandbox_tighten_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sandbox_tighten_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_cors_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_cors_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_file_symlink_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_file_symlink_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_file_traversal_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_file_traversal_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_local_tool_loop_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_tool_loop_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_tool_server_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_server_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_tool_server_ping_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_server_ping_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_tool_server_restart_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_server_restart_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_trace_id_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_trace_id_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_http_body_limit_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_http_body_limit_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_upload_limit_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_upload_limit_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_trace_workflow_events_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_trace_workflow_events_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_trace_memory_correlate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_trace_memory_correlate_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_sessions_root_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sessions_root_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_artifact_protocol_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_artifact_protocol_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_ui_action_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_ui_action_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_client_profile_injection_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_client_profile_injection_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_missing_tool_calls_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_missing_tool_calls_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_client_rpc_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_client_rpc_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_client_rpc_progress_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_client_rpc_progress_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_client_rpc_dom_apply_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_client_rpc_dom_apply_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_client_rpc_entity_apply_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_client_rpc_entity_apply_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_ui_wait_event_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_ui_wait_event_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_ui_wait_any_all_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_ui_wait_any_all_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_session_audit_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_session_audit_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_tools_none_multimodal_events_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tools_none_multimodal_events_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_session_client_events_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_session_client_events_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_session_clients_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_session_clients_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_local_async_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_async_host_tools_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_local_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_stream_assistant_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_local_stream_tool_loop_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_local_stream_tool_loop_smoke.sh $<TARGET_FILE:agentd>)
    add_test(NAME agentd_job_restart_durability_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_job_restart_durability_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_job_restart_durability_smoke PROPERTIES SKIP_RETURN_CODE 77)
		    add_test(NAME agentd_orchestrate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_orchestrate_smoke.sh $<TARGET_FILE:agentd>)
		    set_tests_properties(agentd_orchestrate_smoke PROPERTIES SKIP_RETURN_CODE 77)
		    add_test(NAME agentd_workflow_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_inputs_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_inputs_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_infer_deps_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_infer_deps_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_get_spec_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_get_spec_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_deadline_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_deadline_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_idempotency_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_idempotency_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_cancel_running_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_cancel_running_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_admission_control_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_admission_control_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_delegate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_delegate_attempt_caps_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_attempt_caps_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_macro_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_macro_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_best_of_n_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_best_of_n_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_attempt_caps_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_attempt_caps_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_quorum_ok_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_quorum_ok_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_quorum_hashes_default_pointers_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_quorum_hashes_default_pointers_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_distinct_nodes_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_distinct_nodes_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_strict_all_ok_allow_error_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_strict_all_ok_allow_error_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_delegate_parallel_inputs_propagate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_delegate_parallel_inputs_propagate_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_put_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_put_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_search_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_search_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_timeline_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_timeline_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_structured_query_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_structured_query_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_correlate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_correlate_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_query_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_query_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_http_json_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_http_json_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_http_allowlist_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_http_allowlist_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_http_deny_cidr_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_http_deny_cidr_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_http_dns_pin_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_http_dns_pin_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_agentd_call_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_agentd_call_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_agentd_call_broker_proxy_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_agentd_call_broker_proxy_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_agentd_parallel_macro_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_agentd_parallel_macro_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_agentd_parallel_quorum_hashes_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_agentd_parallel_quorum_hashes_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_agentd_parallel_quorum_hashes_default_pointers_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_agentd_parallel_quorum_hashes_default_pointers_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_agentd_parallel_distinct_nodes_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_agentd_parallel_distinct_nodes_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_memory_consolidate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_memory_consolidate_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_avm_capsule_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_avm_capsule_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_aggregate_quorum_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_aggregate_quorum_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_aggregate_quorum_ok_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_aggregate_quorum_ok_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_aggregate_strict_all_ok_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_aggregate_strict_all_ok_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_aggregate_first_ok_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_aggregate_first_ok_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_aggregate_best_of_n_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_aggregate_best_of_n_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_aggregate_quorum_hashes_object_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_aggregate_quorum_hashes_object_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_invoke_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_invoke_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_parallel_macro_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_parallel_macro_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_parallel_quorum_hashes_default_pointers_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_parallel_quorum_hashes_default_pointers_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_wait_sensor_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_wait_sensor_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_invoke_quorum_hashes_node_ids_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_invoke_quorum_hashes_node_ids_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_invoke_quorum_hashes_distinct_nodes_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_invoke_quorum_hashes_distinct_nodes_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_workflow_edge_invoke_match_any_exclude_nodes_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_invoke_match_any_exclude_nodes_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_edge_invoke_template_args_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_invoke_template_args_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_edge_agent_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_agent_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_edge_agent_ref_payload_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_edge_agent_ref_payload_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_workflow_stream_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_stream_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_expect_extended_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_expect_extended_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_workflow_inflight_cap_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_inflight_cap_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(
				      NAME agentd_workflow_expect_tool_calls_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_expect_tool_calls_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agentd_tool_plugin_echo>
				    )
				    add_test(
				      NAME agentd_workflow_budget_tool_calls_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_tool_calls_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agentd_tool_plugin_echo>
				    )
				    add_test(
				      NAME agentd_workflow_budget_steps_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_steps_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agentd_tool_plugin_echo>
				    )
					    add_test(
					      NAME agentd_workflow_budget_tokens_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_tokens_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agentd_tool_plugin_echo>
					    )
					    add_test(
					      NAME agentd_workflow_budget_tokens_stream_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_tokens_stream_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agentd_tool_plugin_echo>
					    )
					    add_test(
					      NAME agentd_workflow_budget_retry_charges_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_retry_charges_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agentd_tool_plugin_echo>
				    )
					    add_test(
					      NAME agentd_workflow_budget_bulk_cancel_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_bulk_cancel_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agentd_tool_plugin_echo>
					    )
					    add_test(NAME agentd_workflow_budget_host_tool_memory_put_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_budget_host_tool_memory_put_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_inflight_session_cap_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_inflight_session_cap_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_scan_fairness_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_scan_fairness_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_wrr_session_weight_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_wrr_session_weight_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_drr_session_weight_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_drr_session_weight_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_drr_durable_deficit_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_drr_durable_deficit_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_drr_cost_telemetry_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_drr_cost_telemetry_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_stats_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_stats_smoke.sh $<TARGET_FILE:agentd>)
							    add_test(NAME agentd_workflow_stats_budget_pressure_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_stats_budget_pressure_smoke.sh $<TARGET_FILE:agentd>)
						    add_test(NAME agentd_workflow_stats_sessions_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_workflow_stats_sessions_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_edge_interop_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_interop_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_edge_auth_hmac_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_auth_hmac_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(
				      NAME agentd_edge_auth_hmac_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_auth_hmac_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				    )
				    add_test(
				      NAME agentd_edge_auth_ed25519_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_auth_ed25519_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				        $<TARGET_FILE:agent_ed25519_tool>
				    )
				    add_test(
				      NAME agentd_edge_auth_ed25519_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_auth_ed25519_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_ed25519_tool>
				    )
				    add_test(
				      NAME agentd_edge_message_cbor_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_message_cbor_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				    )
				    add_test(
				      NAME agentd_edge_task_loop_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_loop_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				    )
				    add_test(
				      NAME agentd_edge_heartbeat_auth_hmac_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_heartbeat_auth_hmac_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				    )
				    add_test(
				      NAME agentd_edge_task_loop_auth_hmac_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_loop_auth_hmac_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				    )
				    add_test(
				      NAME agentd_edge_task_loop_auth_ed25519_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_loop_auth_ed25519_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				        $<TARGET_FILE:agent_ed25519_tool>
				    )
				    add_test(
				      NAME agentd_edge_invoke_task_loop_auth_hmac_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_invoke_task_loop_auth_hmac_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				    )
				    add_test(
				      NAME agentd_edge_invoke_task_loop_auth_ed25519_cbor_wire_smoke
				      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_invoke_task_loop_auth_ed25519_cbor_wire_smoke.sh
				        $<TARGET_FILE:agentd>
				        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
				        $<TARGET_FILE:agent_ed25519_tool>
				    )
				    add_test(NAME agentd_edge_outbox_cbor_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_outbox_cbor_smoke.sh $<TARGET_FILE:agentd> $<TARGET_FILE:agent_core_outbox_cbor_check>)
				    add_test(NAME agentd_edge_caps_sha256_validation_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_caps_sha256_validation_smoke.sh $<TARGET_FILE:agentd>)
				    add_test(NAME agentd_edge_task_assign_schema_validation_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_assign_schema_validation_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_edge_task_done_result_schema_validation_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_done_result_schema_validation_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_edge_task_attest_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_attest_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(
					      NAME agentd_edge_task_attest_sig_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_attest_sig_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agent_ed25519_tool>
					    )
					    add_test(
					      NAME agentd_edge_task_attest_required_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_task_attest_required_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agent_ed25519_tool>
					    )
					    add_test(NAME agentd_edge_rules_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_rules_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(
					      NAME agentd_edge_rules_sensor_event_auth_hmac_cbor_wire_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_rules_sensor_event_auth_hmac_cbor_wire_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
					    )
					    add_test(
					      NAME agentd_edge_rules_sensor_event_auth_ed25519_cbor_wire_smoke
					      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_rules_sensor_event_auth_ed25519_cbor_wire_smoke.sh
					        $<TARGET_FILE:agentd>
					        $<TARGET_FILE:agent_core_umbmp_cbor_encode>
					        $<TARGET_FILE:agent_ed25519_tool>
					    )
					    add_test(NAME agentd_edge_rules_durable_workflow_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_rules_durable_workflow_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_edge_workflow_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_workflow_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_trace_edge_interop_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_trace_edge_interop_smoke.sh $<TARGET_FILE:agentd>)
					    add_test(NAME agentd_edge_resource_lock_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_resource_lock_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_workflow_submit_message_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_workflow_submit_message_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_workflow_cancel_message_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_workflow_cancel_message_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_interop_transcript_replay_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_interop_transcript_replay_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_interop_task_loop_replay_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_interop_task_loop_replay_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_interop_task_loop_trace_attest_v0_2_replay_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_interop_task_loop_trace_attest_v0_2_replay_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_message_dedupe_replay_handoff_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_message_dedupe_replay_handoff_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(NAME agentd_edge_message_dedupe_replay_task_done_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_message_dedupe_replay_task_done_smoke.sh $<TARGET_FILE:agentd>)
			    add_test(
			      NAME agentd_edge_durable_workflow_submit_message_smoke
			      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_durable_workflow_submit_message_smoke.sh
			        $<TARGET_FILE:agentd>
			        $<TARGET_FILE:agentd_tool_plugin_echo>
			    )
		    add_test(NAME agentd_edge_deadline_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_edge_deadline_smoke.sh $<TARGET_FILE:agentd>)
		    add_test(NAME agentd_memory_consolidate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_memory_consolidate_smoke.sh $<TARGET_FILE:agentd>)
		    add_test(NAME agentd_memory_retention_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_memory_retention_smoke.sh $<TARGET_FILE:agentd>)
		    add_test(NAME agentd_memory_salience_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_memory_salience_smoke.sh $<TARGET_FILE:agentd>)
		    add_test(NAME agentd_memory_recaps_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_memory_recaps_smoke.sh $<TARGET_FILE:agentd>)
		    add_test(NAME agentd_memory_correlate_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_memory_correlate_smoke.sh $<TARGET_FILE:agentd>)
	    add_test(NAME agentd_provider_retry_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_provider_retry_smoke.sh $<TARGET_FILE:agentd>)
	    set_tests_properties(agentd_provider_retry_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_stream_provider_retry_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_stream_provider_retry_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_stream_provider_retry_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_test(
      NAME agentd_tool_plugin_smoke
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_plugin_smoke.sh
        $<TARGET_FILE:agentd>
        $<TARGET_FILE:agentd_tool_plugin_echo>
    )

    add_test(
      NAME agentd_tool_plugin_server_smoke
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_plugin_server_smoke.sh
        $<TARGET_FILE:agentd>
        $<TARGET_FILE:agentd_tool_plugin_host>
        $<TARGET_FILE:agentd_tool_plugin_echo>
    )

    add_test(
      NAME agentd_tool_plugin_big_smoke
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_plugin_big_smoke.sh
        $<TARGET_FILE:agentd>
        $<TARGET_FILE:agentd_tool_plugin_big>
    )

    add_test(
      NAME agentd_tool_plugin_server_big_smoke
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_tool_plugin_server_big_smoke.sh
        $<TARGET_FILE:agentd>
        $<TARGET_FILE:agentd_tool_plugin_host>
        $<TARGET_FILE:agentd_tool_plugin_big>
    )

    if(AGENT_ENABLE_SQLITE3)
      add_test(NAME agentd_db_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_db_smoke.sh $<TARGET_FILE:agentd>)
      add_test(NAME agentd_db_query_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_db_query_smoke.sh $<TARGET_FILE:agentd>)
      add_test(NAME agentd_blob_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_blob_smoke.sh $<TARGET_FILE:agentd>)
      add_test(NAME agentd_run_replay_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_run_replay_smoke.sh $<TARGET_FILE:agentd>)
    endif()

    add_test(
      NAME agentd_non_loopback_auth_guard_smoke
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_non_loopback_auth_guard_smoke.sh $<TARGET_FILE:agentd>
    )

    add_test(NAME agentd_host_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_host_tools_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_host_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME agentd_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_stream_assistant_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_stream_assistant_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_deepseek_stream_tool_call_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_deepseek_stream_tool_call_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_deepseek_stream_tool_call_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_openrouter_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_openrouter_stream_assistant_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_openrouter_stream_assistant_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_moonshot_stream_assistant_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_moonshot_stream_assistant_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_moonshot_stream_assistant_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_openrouter_stream_tool_call_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_openrouter_stream_tool_call_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_openrouter_stream_tool_call_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_moonshot_stream_tool_call_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_moonshot_stream_tool_call_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_moonshot_stream_tool_call_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME agentd_sse_auth_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sse_auth_smoke.sh $<TARGET_FILE:agentd>)

	  add_test(NAME agentd_openrouter_models_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_openrouter_models_smoke.sh $<TARGET_FILE:agentd>)
	  set_tests_properties(agentd_openrouter_models_smoke PROPERTIES SKIP_RETURN_CODE 77)

	  add_test(NAME agentd_deepseek_toolloop_image_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_deepseek_toolloop_image_smoke.sh $<TARGET_FILE:agentd>)
	  set_tests_properties(agentd_deepseek_toolloop_image_smoke PROPERTIES SKIP_RETURN_CODE 77)

	  add_test(NAME agentd_moonshot_upload_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_moonshot_upload_smoke.sh $<TARGET_FILE:agentd>)
	  set_tests_properties(agentd_moonshot_upload_smoke PROPERTIES SKIP_RETURN_CODE 77)
	  add_test(NAME agentd_moonshot_toolloop_image_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_moonshot_toolloop_image_smoke.sh $<TARGET_FILE:agentd>)
	  set_tests_properties(agentd_moonshot_toolloop_image_smoke PROPERTIES SKIP_RETURN_CODE 77)
	  add_test(NAME agentd_moonshot_tool_call_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_moonshot_tool_call_smoke.sh $<TARGET_FILE:agentd>)
	  set_tests_properties(agentd_moonshot_tool_call_smoke PROPERTIES SKIP_RETURN_CODE 77)
	
	    add_test(NAME agentd_async_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_async_smoke.sh $<TARGET_FILE:agentd>)
	    set_tests_properties(agentd_async_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME agentd_sse_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_sse_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_sse_smoke PROPERTIES SKIP_RETURN_CODE 77)

    add_test(NAME agentd_cancel_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_cancel_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_cancel_smoke PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME agentd_heartbeat_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/agentd_heartbeat_smoke.sh $<TARGET_FILE:agentd>)
    set_tests_properties(agentd_heartbeat_smoke PROPERTIES SKIP_RETURN_CODE 77)
  endif()

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
    daemon/src/net_compat.cpp
  )
  target_include_directories(job_manager_tests PRIVATE daemon/src ${JSONCPP_INCLUDE_DIRS})
  target_link_directories(job_manager_tests PRIVATE ${JSONCPP_LIBRARY_DIRS})
  target_link_libraries(job_manager_tests PRIVATE ${JSONCPP_LIBRARIES})
  if(WIN32)
    target_link_libraries(job_manager_tests PRIVATE ws2_32)
  endif()
  add_test(NAME job_manager_tests COMMAND job_manager_tests)

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

  add_executable(openai_tool_provider_stream_tests
    tests/test_openai_tool_provider_stream.cpp
  )
  target_link_libraries(openai_tool_provider_stream_tests PRIVATE agent_host)
  add_test(NAME openai_tool_provider_stream_tests COMMAND openai_tool_provider_stream_tests)

  add_executable(openai_stream_decoder_tests
    tests/test_openai_stream_decoder.cpp
  )
  target_link_libraries(openai_stream_decoder_tests PRIVATE agent_host)
  add_test(NAME openai_stream_decoder_tests COMMAND openai_stream_decoder_tests)

  if(AGENT_ENABLE_JSONCPP)
    add_executable(workflow_fairq_cost_tests
      tests/test_workflow_fairq_cost.cpp
    )
    target_link_libraries(workflow_fairq_cost_tests PRIVATE agentd_lib)
    target_compile_definitions(workflow_fairq_cost_tests PRIVATE AGENT_HAVE_JSONCPP=1)
    add_test(NAME workflow_fairq_cost_tests COMMAND workflow_fairq_cost_tests)

    add_executable(cbor_decode_tests
      tests/test_cbor_decode.cpp
    )
    target_link_libraries(cbor_decode_tests PRIVATE agentd_lib)
    target_compile_definitions(cbor_decode_tests PRIVATE AGENT_HAVE_JSONCPP=1)
    add_test(NAME cbor_decode_tests COMMAND cbor_decode_tests)

    add_executable(s3_presign_tests
      tests/test_s3_presign.cpp
    )
    target_link_libraries(s3_presign_tests PRIVATE agentd_lib)
    target_compile_definitions(s3_presign_tests PRIVATE AGENT_HAVE_JSONCPP=1)
    add_test(NAME s3_presign_tests COMMAND s3_presign_tests)
  endif()

  if(AGENT_ENABLE_SQLITE3)
    add_executable(agent_db_tests
      tests/test_agent_db.cpp
      daemon/src/agent_db.cpp
      daemon/src/agent_db_state.cpp
      daemon/src/agent_db_sessions.cpp
      daemon/src/agent_db_audit.cpp
      daemon/src/agent_db_jobs.cpp
      daemon/src/agent_db_runs.cpp
      daemon/src/agent_db_edge_nodes.cpp
      daemon/src/agent_db_edge_tasks.cpp
      daemon/src/agent_db_edge_workflows.cpp
      daemon/src/agent_db_stats.cpp
    )
    target_include_directories(agent_db_tests PRIVATE daemon/src core/include ${SQLITE3_INCLUDE_DIRS})
    target_link_directories(agent_db_tests PRIVATE ${SQLITE3_LIBRARY_DIRS})
    target_link_libraries(agent_db_tests PRIVATE agent_core ${SQLITE3_LIBRARIES})
    target_compile_definitions(agent_db_tests PRIVATE AGENT_HAVE_SQLITE3=1)
    add_test(NAME agent_db_tests COMMAND agent_db_tests)

    if(AGENT_ENABLE_JSONCPP)
      add_executable(scene_store_mirror_artifact_tests
        tests/test_scene_store_mirror_artifact.cpp
      )
      target_link_libraries(scene_store_mirror_artifact_tests PRIVATE agentd_lib)
      target_compile_definitions(scene_store_mirror_artifact_tests PRIVATE AGENT_HAVE_SQLITE3=1 AGENT_HAVE_JSONCPP=1)
      add_test(NAME scene_store_mirror_artifact_tests COMMAND scene_store_mirror_artifact_tests)

      add_executable(scene_store_clear_preserves_artifacts_tests
        tests/test_scene_store_clear_preserves_artifacts.cpp
      )
      target_link_libraries(scene_store_clear_preserves_artifacts_tests PRIVATE agentd_lib)
      target_compile_definitions(scene_store_clear_preserves_artifacts_tests PRIVATE AGENT_HAVE_SQLITE3=1 AGENT_HAVE_JSONCPP=1)
      add_test(NAME scene_store_clear_preserves_artifacts_tests COMMAND scene_store_clear_preserves_artifacts_tests)
    endif()

    if(AGENTD_ENABLE_HTTP)
      add_executable(agentd_service_tests
        tests/test_agentd_service.cpp
      )
      target_link_libraries(agentd_service_tests PRIVATE agentd_lib)
      add_test(NAME agentd_service_tests COMMAND agentd_service_tests)
    endif()
  endif()
endif()

if(AGENT_BUILD_HOST)
  add_executable(cors_tests
    tests/test_cors.cpp
    daemon/src/cors.cpp
  )
  target_include_directories(cors_tests PRIVATE daemon/src daemon/include)
  add_test(NAME cors_tests COMMAND cors_tests)
endif()

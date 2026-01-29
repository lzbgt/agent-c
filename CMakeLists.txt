cmake_minimum_required(VERSION 3.20)

project(agent LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(agent_core STATIC
  core/src/agent_alloc.c
  core/src/agent_runner.c
  core/src/agent_session.c
  core/src/agent_string.c
  core/src/agent_tools.c
)

target_include_directories(agent_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

target_compile_options(agent_core PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wpedantic>
)

add_executable(agent
  cli/src/main.cpp
  cli/src/openai_client.cpp
  cli/src/session_store.cpp
  cli/src/tool_loop.cpp
  cli/src/toolset_basic.cpp
  cli/src/toolset_host.cpp
)

target_compile_options(agent PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
)

target_link_libraries(agent PRIVATE agent_core)

find_package(CURL REQUIRED)
target_link_libraries(agent PRIVATE CURL::libcurl)

include(FindPkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(JSONCPP jsoncpp)
  if(JSONCPP_FOUND)
    target_include_directories(agent PRIVATE ${JSONCPP_INCLUDE_DIRS})
    target_link_directories(agent PRIVATE ${JSONCPP_LIBRARY_DIRS})
    target_link_libraries(agent PRIVATE ${JSONCPP_LIBRARIES})
    target_compile_definitions(agent PRIVATE AGENT_HAVE_JSONCPP=1)
    target_compile_definitions(agent_core PRIVATE AGENT_HAVE_JSONCPP=1)
  endif()
endif()

enable_testing()
add_executable(agent_core_tests
  tests/test_session.c
  tests/test_runner.c
)
target_link_libraries(agent_core_tests PRIVATE agent_core)
add_test(NAME agent_core_tests COMMAND agent_core_tests)

add_test(NAME agent_openrouter_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/openrouter_smoke.sh $<TARGET_FILE:agent>)
set_tests_properties(agent_openrouter_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME agent_deepseek_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_smoke.sh $<TARGET_FILE:agent>)
set_tests_properties(agent_deepseek_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME agent_deepseek_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_tools_smoke.sh $<TARGET_FILE:agent>)
set_tests_properties(agent_deepseek_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME agent_deepseek_reasoner_tools_smoke COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/deepseek_reasoner_tools_smoke.sh $<TARGET_FILE:agent>)
set_tests_properties(agent_deepseek_reasoner_tools_smoke PROPERTIES SKIP_RETURN_CODE 77)

if(JSONCPP_FOUND)
  add_executable(host_toolset_tests
    tests/test_host_toolset.cpp
    cli/src/toolset_host.cpp
  )
  target_include_directories(host_toolset_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/cli/src
    ${JSONCPP_INCLUDE_DIRS}
  )
  target_link_directories(host_toolset_tests PRIVATE ${JSONCPP_LIBRARY_DIRS})
  target_link_libraries(host_toolset_tests PRIVATE agent_core ${JSONCPP_LIBRARIES})
  target_compile_definitions(host_toolset_tests PRIVATE AGENT_HAVE_JSONCPP=1)
  add_test(NAME host_toolset_tests COMMAND host_toolset_tests)
endif()

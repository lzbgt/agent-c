idf_component_register(
  SRCS
    "../../../../core/src/agent_alloc.c"
    "../../../../core/src/agent_runner.c"
    "../../../../core/src/agent_session.c"
    "../../../../core/src/agent_session_codec.c"
    "../../../../core/src/agent_string.c"
    "../../../../core/src/agent_tool_loop.c"
    "../../../../core/src/agent_tool_provider.c"
    "../../../../core/src/agent_tools.c"
  INCLUDE_DIRS
    "../../../../core/include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wpedantic)


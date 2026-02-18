#include <stdlib.h>
#include <string.h>

static const char* kManifest =
  "{"
  "\"ok\":true,"
  "\"tools\":["
  "{"
  "\"name\":\"ext_big\","
  "\"description\":\"Return a large JSON payload (test plugin)\","
  "\"parameters\":{"
  "\"type\":\"object\","
  "\"properties\":{"
  "\"size\":{\"type\":\"integer\",\"minimum\":1}"
  "},"
  "\"required\":[\"size\"]"
  "}"
  "}"
  "]"
  "}";

const char* agentd_tool_plugin_manifest_json() {
  return kManifest;
}

const char* agentd_tool_plugin_manifest_json_ex(const char* config_json) {
  (void)config_json;
  return kManifest;
}

static char* build_big_response(void) {
  const size_t payload_len = (4 * 1024 * 1024) + 1024;
  const char* prefix = "{\"ok\":true,\"data\":\"";
  const char* suffix = "\"}";
  const size_t prefix_len = strlen(prefix);
  const size_t suffix_len = strlen(suffix);
  const size_t total = prefix_len + payload_len + suffix_len + 1;

  char* out = (char*)malloc(total);
  if (!out) return NULL;
  memcpy(out, prefix, prefix_len);
  memset(out + prefix_len, 'a', payload_len);
  memcpy(out + prefix_len + payload_len, suffix, suffix_len);
  out[total - 1] = 0;
  return out;
}

char* agentd_tool_plugin_execute_json(const char* tool_name, const char* arguments_json) {
  (void)tool_name;
  (void)arguments_json;
  return build_big_response();
}

char* agentd_tool_plugin_execute_json_ex(const char* tool_name, const char* arguments_json, const char* config_json) {
  (void)config_json;
  return agentd_tool_plugin_execute_json(tool_name, arguments_json);
}

void agentd_tool_plugin_free(char* p) {
  if (p) free(p);
}

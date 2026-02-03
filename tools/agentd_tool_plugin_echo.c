#include <stdlib.h>
#include <string.h>

static void json_escape_append(const char* s, char* out, size_t* out_len, size_t out_cap) {
  if (!s || !out || !out_len || out_cap == 0) return;
  for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
    const unsigned char c = *p;
    const char* rep = NULL;
    char tmp[8];
    tmp[0] = 0;
    if (c == '\\') rep = "\\\\";
    else if (c == '"') rep = "\\\"";
    else if (c == '\n') rep = "\\n";
    else if (c == '\r') rep = "\\r";
    else if (c == '\t') rep = "\\t";
    else if (c < 0x20) {
      // \u00XX
      static const char* hex = "0123456789abcdef";
      tmp[0] = '\\';
      tmp[1] = 'u';
      tmp[2] = '0';
      tmp[3] = '0';
      tmp[4] = hex[(c >> 4) & 0xF];
      tmp[5] = hex[c & 0xF];
      tmp[6] = 0;
      rep = tmp;
    }

    if (rep) {
      const size_t need = strlen(rep);
      if (*out_len + need + 1 >= out_cap) return;
      memcpy(out + *out_len, rep, need);
      *out_len += need;
      out[*out_len] = 0;
    } else {
      if (*out_len + 2 >= out_cap) return;
      out[*out_len] = (char)c;
      (*out_len)++;
      out[*out_len] = 0;
    }
  }
}

// Required symbol: returns a JSON manifest describing tools.
//
// Shape (v1):
// {
//   "ok": true,
//   "tools": [
//     { "name": "...", "description": "...", "parameters": { ...json schema... } }
//   ]
// }
const char* agentd_tool_plugin_manifest_json() {
  // NOTE: keep this JSON small and strict. Tool schema is an OpenAI-compatible JSON Schema object.
  return "{"
         "\"ok\":true,"
         "\"tools\":["
         "{"
         "\"name\":\"ext_echo\","
         "\"description\":\"Echo a short string (demo tool plugin)\","
         "\"parameters\":{"
         "\"type\":\"object\","
         "\"properties\":{"
         "\"text\":{\"type\":\"string\",\"maxLength\":256}"
         "},"
         "\"required\":[\"text\"]"
         "}"
         "}"
         "]"
         "}";
}

// Required symbol: executes a tool call and returns a newly allocated JSON string result.
char* agentd_tool_plugin_execute_json(const char* tool_name, const char* arguments_json) {
  const char* tn = (tool_name && tool_name[0]) ? tool_name : "unknown";
  const char* aj = (arguments_json && arguments_json[0]) ? arguments_json : "{}";

  // Allocate a bounded response (this is a demo plugin; keep it tiny).
  const size_t cap = 2048;
  char* out = (char*)malloc(cap);
  if (!out) return NULL;
  out[0] = 0;

  size_t n = 0;
  const char* prefix = "{\"ok\":true,\"tool\":\"";
  const size_t pl = strlen(prefix);
  if (pl + 1 >= cap) {
    free(out);
    return NULL;
  }
  memcpy(out + n, prefix, pl);
  n += pl;
  out[n] = 0;
  json_escape_append(tn, out, &n, cap);

  const char* mid = "\",\"arguments_json\":\"";
  const size_t ml = strlen(mid);
  if (n + ml + 1 >= cap) {
    free(out);
    return NULL;
  }
  memcpy(out + n, mid, ml);
  n += ml;
  out[n] = 0;
  json_escape_append(aj, out, &n, cap);

  const char* suffix = "\"}";
  const size_t sl = strlen(suffix);
  if (n + sl + 1 >= cap) {
    free(out);
    return NULL;
  }
  memcpy(out + n, suffix, sl);
  n += sl;
  out[n] = 0;
  return out;
}

// Required symbol: frees the string returned by agentd_tool_plugin_execute_json.
void agentd_tool_plugin_free(char* p) {
  if (p) free(p);
}

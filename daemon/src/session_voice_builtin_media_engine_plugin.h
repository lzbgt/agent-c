#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  AGENTD_VOICE_MEDIA_ENGINE_PROVIDER_ABI_V1 = 1,
};

typedef struct agentd_voice_media_engine_provider_v1 {
  uint32_t abi_version;
  const char* media_engine_kind;
  int native_media_supported;

  int (*create)(void** out_instance, char* err_buf, size_t err_buf_size);
  void (*destroy)(void* instance);

  int (*initialize)(
    void* instance,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  );

  int (*handle_remote_description)(
    void* instance,
    const char* description_type,
    const char* description_sdp,
    uint64_t initial_remote_candidate_count,
    char* answer_type_buf,
    size_t answer_type_buf_size,
    char* answer_sdp_buf,
    size_t answer_sdp_buf_size,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  );

  int (*handle_remote_candidate)(
    void* instance,
    const char* candidate,
    const char* sdp_mid,
    int sdp_mline_index,
    int has_sdp_mline_index,
    char* event_json_buf,
    size_t event_json_buf_size,
    char* err_buf,
    size_t err_buf_size
  );

  void (*handle_remote_bye)(
    void* instance,
    const char* reason,
    char* event_json_buf,
    size_t event_json_buf_size
  );

  void (*handle_local_shutdown)(
    void* instance,
    char* event_json_buf,
    size_t event_json_buf_size
  );
} agentd_voice_media_engine_provider_v1;

typedef const agentd_voice_media_engine_provider_v1* (*agentd_voice_media_engine_get_api_v1_fn)();

#define AGENTD_VOICE_MEDIA_ENGINE_GET_API_V1_SYMBOL "agentd_voice_media_engine_get_api_v1"

#ifdef __cplusplus
}  // extern "C"
#endif

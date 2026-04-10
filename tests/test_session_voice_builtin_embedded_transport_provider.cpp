#include "session_voice_builtin_media_engine.h"
#include "session_voice_dtls_srtp_util.h"

#include "session_voice_runtime_internal.h"

#include <juice/juice.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using agentd::DaemonConfig;
using agentd::VoiceBrokerSignalDescription;
using agentd::VoiceBrokerSignalRemoteDescriptionReady;
using agentd::VoicePeerRuntime;
using agentd::DtlsSrtpKeyBlock;
using agentd::DtlsSrtpLocalRole;
using agentd::DtlsSrtpProfileSpec;
using agentd::DtlsSrtpSessionPair;
using agentd::create_dtls_srtp_session_pair;
using agentd::derive_dtls_srtp_key_block;
using agentd::export_dtls_srtp_keying_material;
using agentd::make_builtin_voice_peer_media_engine;
using agentd::note_voice_peer_media_engine_event;
using agentd::resolve_dtls_srtp_profile_spec;
using agentd::selected_dtls_srtp_profile_name;

#ifndef AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH
#define AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH ""
#endif

struct DtlsEndpoint {
  SSL_CTX* ctx = nullptr;
  SSL* ssl = nullptr;
  bool handshake_ready = false;
  std::string selected_srtp_profile;
  int packets_sent = 0;
  int packets_received = 0;
};

struct LoopbackRemoteContext {
  agentd::VoicePeerBuiltinMediaEngine* engine = nullptr;
  VoicePeerRuntime* runtime = nullptr;
  juice_agent_t* remote_agent = nullptr;
  DtlsEndpoint client;
  juice_state_t remote_state = JUICE_STATE_DISCONNECTED;
  Json::Value last_candidate_event = Json::Value(Json::nullValue);
  Json::Value last_polled_event = Json::Value(Json::nullValue);
  std::string last_error;
  int candidate_events = 0;
  bool gathering_done = false;
  bool rtp_media_observed = false;
  bool dtls_handshake_started = false;
  bool dtls_handshake_ready = false;
};

void destroy_endpoint(DtlsEndpoint* endpoint) {
  if (!endpoint) return;
  if (endpoint->ssl) {
    SSL_free(endpoint->ssl);
    endpoint->ssl = nullptr;
  }
  if (endpoint->ctx) {
    SSL_CTX_free(endpoint->ctx);
    endpoint->ctx = nullptr;
  }
}

bool configure_client_endpoint(DtlsEndpoint* endpoint) {
  assert(endpoint);
  endpoint->ctx = SSL_CTX_new(DTLS_client_method());
  if (!endpoint->ctx) return false;
  if (SSL_CTX_set_min_proto_version(endpoint->ctx, DTLS1_2_VERSION) != 1) return false;
  SSL_CTX_set_read_ahead(endpoint->ctx, 1);
  SSL_CTX_set_verify(endpoint->ctx, SSL_VERIFY_NONE, nullptr);
  if (SSL_CTX_set_tlsext_use_srtp(
        endpoint->ctx,
        "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32") != 0) {
    return false;
  }

  endpoint->ssl = SSL_new(endpoint->ctx);
  if (!endpoint->ssl) return false;
  BIO* rbio = BIO_new(BIO_s_dgram_mem());
  BIO* wbio = BIO_new(BIO_s_dgram_mem());
  assert(rbio);
  assert(wbio);
  (void)BIO_ctrl(rbio, BIO_CTRL_DGRAM_SET_MTU, 1200, nullptr);
  (void)BIO_ctrl(wbio, BIO_CTRL_DGRAM_SET_MTU, 1200, nullptr);
  SSL_set_bio(endpoint->ssl, rbio, wbio);
  SSL_set_options(endpoint->ssl, SSL_OP_NO_QUERY_MTU);
  SSL_set_mtu(endpoint->ssl, 1200);
  SSL_set_connect_state(endpoint->ssl);
  return true;
}

bool pump_outbound_packets_to_remote_agent(LoopbackRemoteContext* ctx) {
  assert(ctx);
  assert(ctx->remote_agent);
  BIO* wbio = SSL_get_wbio(ctx->client.ssl);
  assert(wbio);
  char packet[2048];
  for (;;) {
    const int n = BIO_read(wbio, packet, sizeof(packet));
    if (n <= 0) break;
    const int rc = juice_send(ctx->remote_agent, packet, static_cast<size_t>(n));
    if (rc != JUICE_ERR_SUCCESS) {
      ctx->last_error = "remote libjuice send failed with code " + std::to_string(rc);
      return false;
    }
    ctx->client.packets_sent += 1;
  }
  return true;
}

void advance_client_dtls_handshake(LoopbackRemoteContext* ctx) {
  assert(ctx);
  assert(ctx->client.ssl);
  if (ctx->client.handshake_ready) return;
  const int rc = SSL_do_handshake(ctx->client.ssl);
  assert(pump_outbound_packets_to_remote_agent(ctx));
  if (rc == 1) {
    ctx->client.handshake_ready = true;
    ctx->dtls_handshake_ready = true;
    ctx->client.selected_srtp_profile = selected_dtls_srtp_profile_name(ctx->client.ssl);
    return;
  }
  const int ssl_err = SSL_get_error(ctx->client.ssl, rc);
  assert(ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE);
}

void write_u16_be(unsigned char* out, uint16_t value) {
  out[0] = static_cast<unsigned char>((value >> 8) & 0xFF);
  out[1] = static_cast<unsigned char>(value & 0xFF);
}

void write_u32_be(unsigned char* out, uint32_t value) {
  out[0] = static_cast<unsigned char>((value >> 24) & 0xFF);
  out[1] = static_cast<unsigned char>((value >> 16) & 0xFF);
  out[2] = static_cast<unsigned char>((value >> 8) & 0xFF);
  out[3] = static_cast<unsigned char>(value & 0xFF);
}

void on_loopback_remote_state_changed(juice_agent_t*, juice_state_t state, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  ctx->remote_state = state;
}

void on_loopback_remote_candidate(juice_agent_t*, const char* sdp, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  assert(ctx->engine);
  assert(ctx->runtime);

  agentd::VoiceBrokerSignalIngress ingress;
  ingress.kind = agentd::VoiceBrokerSignalIngressKind::remote_candidate_ready;
  ingress.candidate.candidate = sdp ? sdp : "";

  Json::Value event(Json::nullValue);
  std::string err;
  const bool ok = ctx->engine->handle_remote_candidate(ingress, &event, &err);
  assert(ok);
  assert(err.empty());
  note_voice_peer_media_engine_event(ctx->runtime, event);
  ctx->last_candidate_event = event;
  ctx->candidate_events += 1;
}

void on_loopback_remote_gathering_done(juice_agent_t*, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  assert(ctx->engine);
  assert(ctx->runtime);
  ctx->gathering_done = true;

  agentd::VoiceBrokerSignalIngress ingress;
  ingress.kind = agentd::VoiceBrokerSignalIngressKind::remote_candidate_ready;
  ingress.candidate.candidate = "a=end-of-candidates";

  Json::Value event(Json::nullValue);
  std::string err;
  const bool ok = ctx->engine->handle_remote_candidate(ingress, &event, &err);
  assert(ok);
  assert(err.empty());
  note_voice_peer_media_engine_event(ctx->runtime, event);
  ctx->last_candidate_event = event;
}

void on_loopback_remote_recv(juice_agent_t*, const char* data, size_t size, void* user_ptr) {
  auto* ctx = static_cast<LoopbackRemoteContext*>(user_ptr);
  assert(ctx);
  assert(ctx->client.ssl);
  BIO* rbio = SSL_get_rbio(ctx->client.ssl);
  assert(rbio);
  assert(BIO_write(rbio, data, static_cast<int>(size)) == static_cast<int>(size));
  ctx->client.packets_received += 1;
  advance_client_dtls_handshake(ctx);
}

std::string make_libjuice_offer_sdp() {
  juice_config_t cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  juice_agent_t* agent = juice_create(&cfg);
  assert(agent);

  char description[JUICE_MAX_SDP_STRING_LEN];
  std::memset(description, 0, sizeof(description));
  const int rc = juice_get_local_description(agent, description, sizeof(description));
  assert(rc == JUICE_ERR_SUCCESS);
  const std::string offer = description;
  juice_destroy(agent);
  return offer;
}

std::string make_browser_style_offer_sdp() {
  return
    "v=0\r\n"
    "o=- 4962323985234234 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=ice-ufrag:remoteUfrag\r\n"
    "a=ice-pwd:remotePassword123\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=candidate:1 1 UDP 2113667327 192.168.0.11 40000 typ host\r\n"
    "a=end-of-candidates\r\n";
}

static void test_embedded_transport_provider_loads_and_answers_remote_offer() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().media_engine_kind == "builtin_native_plugin");
  assert(engine->info().native_media_supported);
  assert(engine->info().provider_name == "agentd_builtin_embedded_transport_provider");
  assert(engine->info().provider_capabilities["embedded_transport_provider"].asBool());
  assert(engine->info().provider_capabilities["transport_family"].asString() ==
         "embedded_transport_primitives");
  assert(engine->info().provider_capabilities["ice"].asBool());
  assert(engine->info().provider_capabilities["dtls"].asBool());
  assert(engine->info().provider_capabilities["dtls_handshake"].asBool());
  assert(engine->info().provider_capabilities["dtls_srtp_export"].asBool());
  assert(engine->info().provider_capabilities["srtp"].asBool());
  assert(engine->info().provider_capabilities["rtp_ingest"].asBool());
  assert(engine->info().provider_capabilities["sctp"].asBool());
  assert(engine->info().provider_capabilities["real_media_engine"].asBool() == false);

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(init_event["event"].asString() == "media_engine_initialized");
  assert(init_event["transport_family"].asString() == "embedded_transport_primitives");
  assert(init_event["libjuice_local_description_bytes"].asUInt64() > 0);
  assert(init_event["gather_started"].asBool());
  assert(init_event["dtls_identity_ready"].asBool());
  assert(init_event["dtls_handshake_ready"].asBool() == false);
  assert(init_event["dtls_exporter_ready"].asBool() == false);
  assert(init_event["srtp_contexts_ready"].asBool() == false);
  assert(init_event["srtp_inbound_ready"].asBool() == false);
  assert(init_event["srtp_outbound_ready"].asBool() == false);
  assert(init_event["dtls_handshake_state"].asString() == "ready_for_client_hello");
  assert(init_event["native_media_supported"].asBool());
  assert(init_event["native_media_active"].asBool() == false);
  assert(init_event["dtls_setup_role"].asString() == "passive");
  assert(!init_event["dtls_fingerprint_sha256"].asString().empty());
  assert(!init_event["dtls_certificate_subject"].asString().empty());
  assert(init_event["usrsctp_initialized"].asBool());
  assert(!init_event["srtp_version"].asString().empty());
  assert(runtime.native_media_provider["name"].asString() ==
         "agentd_builtin_embedded_transport_provider");

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_libjuice_offer_sdp();

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  assert(answer.type == "answer");
  assert(!answer.sdp.empty());
  assert(answer.sdp.find("a=ice-ufrag:") != std::string::npos);
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer_event["event"].asString() == "embedded_transport_answer_ready");
  assert(answer_event["remote_description_applied"].asBool());
  assert(answer_event["transport_family"].asString() == "embedded_transport_primitives");
  assert(answer_event["media_engine_state"].asString() == "answer_ready");
  assert(answer_event["sdp_answer_shape"].asString() == "ice_only");
  assert(answer_event["srtp_contexts_ready"].asBool() == false);
  assert(runtime.media_engine_state == "answer_ready");
  assert(runtime.native_media_supported);
  assert(!runtime.native_media_active);
}

static void test_embedded_transport_provider_mirrors_browser_offer_shape_with_dtls_identity() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().provider_name == "agentd_builtin_embedded_transport_provider");

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);
  assert(runtime.dtls_identity_ready);
  assert(runtime.native_media_supported);
  assert(!runtime.native_media_active);
  assert(!runtime.dtls_fingerprint_sha256.empty());
  assert(runtime.dtls_setup_role == "passive");
  assert(!runtime.dtls_certificate_subject.empty());

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_browser_style_offer_sdp();
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8") != std::string::npos);
  assert(answer.sdp.find("a=mid:0") != std::string::npos);
  assert(answer.sdp.find("a=inactive") != std::string::npos);
  assert(answer.sdp.find("a=setup:passive") != std::string::npos);
  assert(answer.sdp.find("a=fingerprint:sha-256 ") != std::string::npos);
  assert(answer.sdp.find(runtime.dtls_fingerprint_sha256) != std::string::npos);
  assert(answer.sdp.find("a=ice-ufrag:") != std::string::npos);
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  assert(answer_event["dtls_identity_ready"].asBool());
  assert(answer_event["dtls_setup_role"].asString() == "passive");
  assert(answer_event["sdp_answer_shape"].asString() == "browser_offer_mirrored_inactive");
  assert(answer_event["dtls_fingerprint_sha256"].asString() == runtime.dtls_fingerprint_sha256);
}

static void test_embedded_transport_provider_reaches_local_ice_connectivity() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());
  assert(engine->info().provider_name == "agentd_builtin_embedded_transport_provider");

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);

  LoopbackRemoteContext ctx;
  ctx.engine = engine.get();
  ctx.runtime = &runtime;

  juice_config_t remote_cfg;
  std::memset(&remote_cfg, 0, sizeof(remote_cfg));
  remote_cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  remote_cfg.cb_state_changed = &on_loopback_remote_state_changed;
  remote_cfg.cb_candidate = &on_loopback_remote_candidate;
  remote_cfg.cb_gathering_done = &on_loopback_remote_gathering_done;
  remote_cfg.user_ptr = &ctx;

  juice_agent_t* remote_agent = juice_create(&remote_cfg);
  assert(remote_agent);

  char offer_buf[JUICE_MAX_SDP_STRING_LEN];
  std::memset(offer_buf, 0, sizeof(offer_buf));
  assert(juice_get_local_description(remote_agent, offer_buf, sizeof(offer_buf)) == JUICE_ERR_SUCCESS);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = offer_buf;
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  assert(juice_set_remote_description(remote_agent, answer.sdp.c_str()) == JUICE_ERR_SUCCESS);
  assert(juice_set_remote_gathering_done(remote_agent) == JUICE_ERR_SUCCESS);
  assert(juice_gather_candidates(remote_agent) == JUICE_ERR_SUCCESS);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ctx.candidate_events > 0 &&
        ctx.remote_state != JUICE_STATE_DISCONNECTED) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  assert(ctx.gathering_done);
  assert(ctx.candidate_events > 0);
  assert(ctx.remote_state != JUICE_STATE_DISCONNECTED);
  assert(ctx.last_error.empty());
  assert(ctx.last_candidate_event["event"].asString() == "remote_candidate_ready");
  assert(ctx.last_candidate_event["remote_candidates_seen"].asUInt64() >= 1);
  assert(ctx.last_candidate_event["local_candidates_observed"].asUInt64() >= 1);
  assert(!ctx.last_candidate_event["libjuice_state"].asString().empty());
  assert(ctx.last_candidate_event.isMember("transport_connectivity_ready"));
  if (ctx.last_candidate_event["transport_connectivity_ready"].asBool()) {
    assert(!ctx.last_candidate_event["libjuice_selected_local_candidate"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_remote_candidate"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_local_address"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_remote_address"].asString().empty());
  }
  assert(runtime.media_remote_candidates_seen >= 1);
  juice_destroy(remote_agent);
}

}  // namespace

int main() {
  test_embedded_transport_provider_loads_and_answers_remote_offer();
  test_embedded_transport_provider_mirrors_browser_offer_shape_with_dtls_identity();
  test_embedded_transport_provider_reaches_local_ice_connectivity();
  return 0;
}

#include "session_voice_builtin_media_engine.h"
#include "session_voice_dtls_srtp_util.h"

#include "session_voice_runtime_internal.h"

#include <juice/juice.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>

#include <cassert>
#include <chrono>
#include <cstring>
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
  DtlsSrtpSessionPair client_srtp_sessions;
  juice_state_t remote_state = JUICE_STATE_DISCONNECTED;
  Json::Value last_candidate_event = Json::Value(Json::nullValue);
  Json::Value last_polled_event = Json::Value(Json::nullValue);
  std::string last_error;
  int candidate_events = 0;
  bool gathering_done = false;
  bool rtp_media_observed = false;
  bool dtls_handshake_started = false;
  bool dtls_handshake_ready = false;
  bool client_srtp_ready = false;
  int outbound_rtp_packets_observed = 0;
  int outbound_rtcp_packets_observed = 0;
  int outbound_rtcp_compound_packets_observed = 0;
  uint8_t last_outbound_payload_type = 255;
  uint16_t last_outbound_sequence = 0;
  uint32_t last_outbound_timestamp = 0;
  uint32_t last_outbound_ssrc = 0;
  size_t last_outbound_payload_size = 0;
  uint8_t last_outbound_rtcp_packet_type = 0;
  uint32_t last_outbound_rtcp_ssrc = 0;
  int outbound_rtcp_sender_reports_observed = 0;
  int outbound_rtcp_receiver_reports_observed = 0;
  size_t last_outbound_rtcp_compound_packet_count = 0;
};

bool remote_transport_ready(const LoopbackRemoteContext& ctx) {
  return ctx.remote_state == JUICE_STATE_CONNECTED ||
         ctx.remote_state == JUICE_STATE_COMPLETED;
}

bool event_transport_ready(const Json::Value& event) {
  return event.isObject() &&
         event.isMember("transport_connectivity_ready") &&
         event["transport_connectivity_ready"].isBool() &&
         event["transport_connectivity_ready"].asBool();
}

bool provider_transport_ready(const LoopbackRemoteContext& ctx) {
  return event_transport_ready(ctx.last_candidate_event) ||
         event_transport_ready(ctx.last_polled_event);
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
  if (!remote_transport_ready(*ctx)) return;
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

bool ensure_client_srtp_sessions(LoopbackRemoteContext* ctx) {
  assert(ctx);
  if (ctx->client_srtp_ready) return true;
  if (!ctx->client.handshake_ready) return false;

  DtlsSrtpProfileSpec profile;
  std::string err;
  assert(resolve_dtls_srtp_profile_spec(ctx->client.selected_srtp_profile, &profile, &err));
  assert(err.empty());

  std::vector<unsigned char> exporter_keying_material;
  assert(export_dtls_srtp_keying_material(
    ctx->client.ssl,
    profile,
    &exporter_keying_material,
    &err));
  assert(err.empty());

  DtlsSrtpKeyBlock key_block;
  assert(derive_dtls_srtp_key_block(
    profile,
    exporter_keying_material.data(),
    exporter_keying_material.size(),
    &key_block,
    &err));
  assert(err.empty());

  assert(create_dtls_srtp_session_pair(
    key_block,
    DtlsSrtpLocalRole::client,
    &ctx->client_srtp_sessions,
    &err));
  assert(err.empty());
  ctx->client_srtp_ready = true;
  return true;
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
  const auto* bytes = reinterpret_cast<const unsigned char*>(data);
  if (agentd::is_probable_rtp_or_rtcp_packet(bytes, size) &&
      !agentd::is_probable_dtls_packet(bytes, size)) {
    assert(ensure_client_srtp_sessions(ctx));
    agentd::ParsedRtpPacketInfo parsed_rtp;
    agentd::ParsedRtcpPacketInfo parsed_rtcp;
    bool was_rtcp = false;
    std::string err;
    assert(agentd::unprotect_inbound_srtp_packet(
      ctx->client_srtp_sessions.inbound,
      bytes,
      size,
      &parsed_rtp,
      &was_rtcp,
      &err,
      &parsed_rtcp));
    assert(err.empty());
    if (!was_rtcp) {
      ctx->outbound_rtp_packets_observed += 1;
      ctx->last_outbound_payload_type = parsed_rtp.payload_type;
      ctx->last_outbound_sequence = parsed_rtp.sequence;
      ctx->last_outbound_timestamp = parsed_rtp.timestamp;
      ctx->last_outbound_ssrc = parsed_rtp.ssrc;
      ctx->last_outbound_payload_size = parsed_rtp.payload_size;
    } else {
      ctx->outbound_rtcp_packets_observed += 1;
      ctx->last_outbound_rtcp_packet_type = parsed_rtcp.packet_type;
      ctx->last_outbound_rtcp_ssrc = parsed_rtcp.ssrc;
      ctx->last_outbound_rtcp_compound_packet_count =
        parsed_rtcp.compound_packet_count;
      if (parsed_rtcp.is_compound) {
        ctx->outbound_rtcp_compound_packets_observed += 1;
      }
      if (parsed_rtcp.packet_type == 200) {
        ctx->outbound_rtcp_sender_reports_observed += 1;
      } else if (parsed_rtcp.packet_type == 201) {
        ctx->outbound_rtcp_receiver_reports_observed += 1;
      }
    }
    return;
  }
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

std::string make_pcma_only_browser_offer_sdp() {
  return
    "v=0\r\n"
    "o=- 4962323985234234 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=ice-ufrag:remoteUfrag\r\n"
    "a=ice-pwd:remotePassword123\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=candidate:1 1 UDP 2113667327 192.168.0.11 40000 typ host\r\n"
    "a=end-of-candidates\r\n";
}

std::string make_browser_offer_with_feedback_extmaps_and_remote_track_attrs_sdp() {
  return
    "v=0\r\n"
    "o=- 4962323985234234 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 101\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtcp-rsize\r\n"
    "a=rtcp-mux-only\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
    "a=rtpmap:101 telephone-event/8000\r\n"
    "a=fmtp:111 minptime=10;useinbandfec=1\r\n"
    "a=rtcp-fb:111 transport-cc\r\n"
    "a=rtcp-fb:0 nack\r\n"
    "a=rtcp-fb:101 nack\r\n"
    "a=rtcp-fb:* nack\r\n"
    "a=extmap-allow-mixed\r\n"
    "a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
    "a=msid:remote_stream remote_track\r\n"
    "a=ssrc:1234 cname:remote-cname\r\n"
    "a=ssrc:1234 msid:remote_stream remote_track\r\n"
    "a=ice-ufrag:remoteUfrag\r\n"
    "a=ice-pwd:remotePassword123\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=candidate:1 1 UDP 2113667327 192.168.0.11 40000 typ host\r\n"
    "a=end-of-candidates\r\n";
}

std::string make_pcmu_only_active_audio_with_extra_opus_offer_sdp() {
  return
    "v=0\r\n"
    "o=- 4962323985234234 2 IN IP4 127.0.0.1\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0 1\r\n"
    "a=msid-semantic: WMS\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 0\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=sendrecv\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=ice-ufrag:remoteUfrag\r\n"
    "a=ice-pwd:remotePassword123\r\n"
    "a=fingerprint:sha-256 11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00\r\n"
    "a=setup:actpass\r\n"
    "a=candidate:1 1 UDP 2113667327 192.168.0.11 40000 typ host\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:1\r\n"
    "a=recvonly\r\n"
    "a=rtcp-mux\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=rtpmap:0 PCMU/8000\r\n"
    "a=rtpmap:8 PCMA/8000\r\n"
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
  assert(engine->info().provider_capabilities["rtp_transmit"].asBool());
  assert(engine->info().provider_capabilities["rtcp_ingest"].asBool());
  assert(engine->info().provider_capabilities["rtcp_transmit"].asBool());
  assert(engine->info().provider_capabilities["rtcp_compound"].asBool());
  assert(engine->info().provider_capabilities["rtcp_receiver_report"].asBool());
  assert(engine->info().provider_capabilities["audio_drain"].asBool());
  assert(engine->info().provider_capabilities["audio_owner_handoff"].asBool());
  assert(engine->info().provider_capabilities["audio_submit"].asBool());
  assert(engine->info().provider_capabilities["audio_outbound_pcmu"].asBool());
  assert(engine->info().provider_capabilities["audio_outbound_pcma"].asBool());
  assert(engine->info().provider_capabilities["audio_outbound_opus"].isBool());
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
  assert(runtime.native_media_provider["abi_version"].asInt() == 5);

  agentd::VoicePeerBuiltinAudioChunk drained;
  Json::Value drained_event(Json::nullValue);
  assert(engine->drain_audio(&drained, &drained_event, &err));
  assert(err.empty());
  assert(drained.pcm_samples.empty());
  assert(drained_event.isNull());

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
  assert(answer_event["audio_outbound_payload_type"].asInt64() == 0);
  assert(answer_event["audio_outbound_codec_name"].asString() == "PCMU");
  assert(answer_event["audio_outbound_sample_rate_hz"].asInt64() == 8000);
  assert(answer_event["audio_outbound_channels"].asInt64() == 1);
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
  assert(answer.sdp.find("a=sendrecv") != std::string::npos);
  assert(answer.sdp.find("a=inactive") == std::string::npos);
  assert(answer.sdp.find("a=setup:passive") != std::string::npos);
  assert(answer.sdp.find("a=fingerprint:sha-256 ") != std::string::npos);
  assert(answer.sdp.find(runtime.dtls_fingerprint_sha256) != std::string::npos);
  assert(answer.sdp.find("a=ice-ufrag:") != std::string::npos);
  assert(answer.sdp.find("a=candidate:") != std::string::npos);
  assert(answer_event["dtls_identity_ready"].asBool());
  assert(answer_event["dtls_setup_role"].asString() == "passive");
  assert(answer_event["sdp_answer_shape"].asString() == "browser_offer_mirrored_active");
  assert(answer_event["dtls_fingerprint_sha256"].asString() == runtime.dtls_fingerprint_sha256);
  if (engine->info().provider_capabilities["audio_outbound_opus"].asBool()) {
    assert(answer_event["audio_outbound_payload_type"].asInt64() == 111);
    assert(answer_event["audio_outbound_codec_name"].asString() == "OPUS");
    assert(answer_event["audio_outbound_sample_rate_hz"].asInt64() == 48000);
    assert(answer_event["audio_outbound_channels"].asInt64() == 2);
  } else {
    assert(answer_event["audio_outbound_payload_type"].asInt64() == 0);
    assert(answer_event["audio_outbound_codec_name"].asString() == "PCMU");
    assert(answer_event["audio_outbound_sample_rate_hz"].asInt64() == 8000);
    assert(answer_event["audio_outbound_channels"].asInt64() == 1);
  }
}

static void test_embedded_transport_provider_preserves_supported_feedback_and_strips_remote_tracks() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_browser_offer_with_feedback_extmaps_and_remote_track_attrs_sdp();
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("a=rtcp-fb:0 nack\r\n") != std::string::npos);
  assert(answer.sdp.find("a=rtcp-fb:* nack\r\n") != std::string::npos);
  assert(answer.sdp.find("a=rtcp-fb:101 nack\r\n") == std::string::npos);
  assert(answer.sdp.find("a=rtpmap:101 telephone-event/8000\r\n") == std::string::npos);
  assert(answer.sdp.find("a=extmap-allow-mixed\r\n") != std::string::npos);
  assert(answer.sdp.find("a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid\r\n") != std::string::npos);
  assert(answer.sdp.find("a=msid:remote_stream remote_track\r\n") == std::string::npos);
  assert(answer.sdp.find("a=ssrc:1234 ") == std::string::npos);
  assert(answer.sdp.find("a=msid:agentd_builtin_stream agentd_builtin_audio\r\n") != std::string::npos);
  if (engine->info().provider_capabilities["audio_outbound_opus"].asBool()) {
    assert(answer.sdp.find("a=rtcp-fb:111 transport-cc\r\n") != std::string::npos);
    assert(answer_event["audio_outbound_payload_type"].asInt64() == 111);
    assert(answer_event["audio_outbound_codec_name"].asString() == "OPUS");
  } else {
    assert(answer.sdp.find("a=rtcp-fb:111 transport-cc\r\n") == std::string::npos);
    assert(answer_event["audio_outbound_payload_type"].asInt64() == 0);
    assert(answer_event["audio_outbound_codec_name"].asString() == "PCMU");
  }
}

static void test_embedded_transport_provider_selects_pcma_when_pcmu_is_unavailable() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_pcma_only_browser_offer_sdp();
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("m=audio 9 UDP/TLS/RTP/SAVPF 8") != std::string::npos);
  assert(answer_event["audio_outbound_payload_type"].asInt64() == 8);
  assert(answer_event["audio_outbound_codec_name"].asString() == "PCMA");
  assert(answer_event["audio_outbound_sample_rate_hz"].asInt64() == 8000);
  assert(answer_event["audio_outbound_channels"].asInt64() == 1);
  assert(runtime.audio_outbound_payload_type == 8);
  assert(runtime.audio_outbound_codec_name == "PCMA");
}

static void test_embedded_transport_provider_selects_codec_from_accepted_audio_mline() {
  DaemonConfig cfg;
  cfg.audio_webrtc_builtin_mode = "native_plugin";
  cfg.audio_webrtc_builtin_native_library_path =
    AGENTD_TEST_VOICE_MEDIA_ENGINE_EMBEDDED_PLUGIN_PATH;

  std::string err;
  auto engine = make_builtin_voice_peer_media_engine(cfg, &err);
  assert(engine);
  assert(err.empty());

  VoicePeerRuntime runtime;
  Json::Value init_event(Json::nullValue);
  assert(engine->initialize(&runtime, &init_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, init_event);

  VoiceBrokerSignalRemoteDescriptionReady ready;
  ready.description.type = "offer";
  ready.description.sdp = make_pcmu_only_active_audio_with_extra_opus_offer_sdp();
  ready.initial_remote_candidates.resize(1);

  VoiceBrokerSignalDescription answer;
  Json::Value answer_event(Json::nullValue);
  assert(engine->handle_remote_description(ready, &answer, &answer_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, answer_event);
  assert(answer.type == "answer");
  assert(answer.sdp.find("a=group:BUNDLE 0") != std::string::npos);
  assert(answer.sdp.find("m=audio 9 UDP/TLS/RTP/SAVPF 0") != std::string::npos);
  assert(answer.sdp.find("m=audio 0 UDP/TLS/RTP/SAVPF 111 0 8") != std::string::npos);
  assert(answer_event["audio_outbound_payload_type"].asInt64() == 0);
  assert(answer_event["audio_outbound_codec_name"].asString() == "PCMU");
  assert(answer_event["audio_outbound_sample_rate_hz"].asInt64() == 8000);
  assert(answer_event["audio_outbound_channels"].asInt64() == 1);
  assert(runtime.audio_outbound_payload_type == 0);
  assert(runtime.audio_outbound_codec_name == "PCMU");
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
  assert(configure_client_endpoint(&ctx.client));

  juice_config_t remote_cfg;
  std::memset(&remote_cfg, 0, sizeof(remote_cfg));
  remote_cfg.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  remote_cfg.cb_state_changed = &on_loopback_remote_state_changed;
  remote_cfg.cb_candidate = &on_loopback_remote_candidate;
  remote_cfg.cb_gathering_done = &on_loopback_remote_gathering_done;
  remote_cfg.cb_recv = &on_loopback_remote_recv;
  remote_cfg.user_ptr = &ctx;

  juice_agent_t* remote_agent = juice_create(&remote_cfg);
  assert(remote_agent);
  ctx.remote_agent = remote_agent;

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
    if (ctx.candidate_events > 0 && remote_transport_ready(ctx)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  assert(ctx.gathering_done);
  assert(ctx.candidate_events > 0);
  assert(remote_transport_ready(ctx));
  assert(ctx.last_error.empty());
  assert(ctx.last_candidate_event["event"].asString() == "remote_candidate_ready");
  assert(ctx.last_candidate_event["remote_candidates_seen"].asUInt64() >= 1);
  assert(ctx.last_candidate_event["local_candidates_observed"].asUInt64() >= 1);
  assert(!ctx.last_candidate_event["libjuice_state"].asString().empty());
  assert(ctx.last_candidate_event.isMember("transport_connectivity_ready"));
  if (event_transport_ready(ctx.last_candidate_event)) {
    assert(!ctx.last_candidate_event["libjuice_selected_local_candidate"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_remote_candidate"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_local_address"].asString().empty());
    assert(!ctx.last_candidate_event["libjuice_selected_remote_address"].asString().empty());
  }
  assert(runtime.media_remote_candidates_seen >= 1);

  for (int i = 0; i < 128; ++i) {
    Json::Value polled_event(Json::nullValue);
    std::string poll_err;
    assert(engine->poll_status(&polled_event, &poll_err));
    assert(poll_err.empty());
    if (polled_event.isObject()) {
      note_voice_peer_media_engine_event(&runtime, polled_event);
      ctx.last_polled_event = polled_event;
    }
    if (remote_transport_ready(ctx) && provider_transport_ready(ctx)) {
      advance_client_dtls_handshake(&ctx);
    }
    if (ctx.dtls_handshake_ready && runtime.srtp_contexts_ready) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(ctx.dtls_handshake_ready);
  assert(ensure_client_srtp_sessions(&ctx));
  assert(runtime.srtp_inbound_ready);
  assert(runtime.srtp_outbound_ready);

  std::vector<unsigned char> inbound_plain_rtp(12 + 160, 0xFF);
  inbound_plain_rtp[0] = 0x80;
  inbound_plain_rtp[1] = 0;
  write_u16_be(inbound_plain_rtp.data() + 2, 77);
  write_u32_be(inbound_plain_rtp.data() + 4, 160);
  write_u32_be(inbound_plain_rtp.data() + 8, 0x10203040u);

  std::vector<unsigned char> protected_inbound_rtp;
  assert(agentd::protect_outbound_rtp_packet(
    ctx.client_srtp_sessions.outbound,
    inbound_plain_rtp.data(),
    inbound_plain_rtp.size(),
    &protected_inbound_rtp,
    &err));
  assert(err.empty());
  assert(juice_send(
           remote_agent,
           reinterpret_cast<const char*>(protected_inbound_rtp.data()),
           protected_inbound_rtp.size()) == JUICE_ERR_SUCCESS);

  const auto rr_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < rr_deadline) {
    Json::Value polled_event(Json::nullValue);
    std::string poll_err;
    assert(engine->poll_status(&polled_event, &poll_err));
    assert(poll_err.empty());
    if (polled_event.isObject()) {
      note_voice_peer_media_engine_event(&runtime, polled_event);
      ctx.last_polled_event = polled_event;
    }
    if (runtime.rtp_packets_received >= 1 &&
        runtime.rtcp_receiver_reports_sent >= 1 &&
        ctx.outbound_rtcp_receiver_reports_observed >= 1) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(runtime.rtp_packets_received >= 1);
  assert(runtime.rtcp_receiver_reports_sent >= 1);
  assert(runtime.rtcp_receiver_report_blocks_sent >= 1);
  assert(runtime.rtcp_last_sent_packet_type == 201);
  assert(runtime.rtcp_last_reported_rtp_ssrc == 0x10203040);
  assert(runtime.rtcp_last_report_highest_sequence >= 77);
  assert(ctx.outbound_rtcp_receiver_reports_observed >= 1);
  assert(ctx.last_outbound_rtcp_packet_type == 201);
  assert(ctx.outbound_rtcp_compound_packets_observed >= 1);
  assert(ctx.last_outbound_rtcp_compound_packet_count >= 2);

  agentd::VoicePeerBuiltinAudioChunk submit_chunk;
  submit_chunk.pcm_samples.assign(160, 2000);
  submit_chunk.sample_rate_hz = 8000;
  submit_chunk.channels = 1;
  submit_chunk.frame_samples_per_channel = 160;
  submit_chunk.codec_name = "PCM16";

  Json::Value submit_event(Json::nullValue);
  assert(engine->submit_audio(submit_chunk, &submit_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, submit_event);
  assert(submit_event["event"].asString() == "audio_chunk_transmitted");
  assert(submit_event["rtp_packets_sent"].asUInt64() >= 1);
  assert(submit_event["rtcp_packets_sent"].asUInt64() >= 1);
  assert(submit_event["rtcp_last_sent_packet_type"].asInt64() == 200);
  assert(runtime.rtcp_packets_sent >= 1);
  assert(runtime.rtcp_last_sent_packet_type == 200);
  const auto sender_reports_after_first_submit = runtime.rtcp_sender_reports_sent;

  const auto media_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < media_deadline) {
    if (ctx.outbound_rtp_packets_observed > 0 &&
        ctx.outbound_rtcp_sender_reports_observed > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(ctx.outbound_rtp_packets_observed >= 1);
  assert(ctx.outbound_rtcp_packets_observed >= 1);
  assert(ctx.outbound_rtcp_sender_reports_observed >= 1);
  assert(ctx.outbound_rtcp_receiver_reports_observed >= 1);
  assert(ctx.outbound_rtcp_compound_packets_observed >= 2);
  assert(ctx.last_outbound_rtcp_compound_packet_count >= 2);
  assert(ctx.last_outbound_payload_type == runtime.audio_outbound_payload_type);
  assert(ctx.last_outbound_ssrc == static_cast<uint32_t>(runtime.rtp_last_sent_ssrc));
  assert(ctx.last_outbound_payload_size > 0);
  assert(ctx.last_outbound_rtcp_ssrc == static_cast<uint32_t>(runtime.rtcp_last_sent_ssrc));

  Json::Value second_submit_event(Json::nullValue);
  assert(engine->submit_audio(submit_chunk, &second_submit_event, &err));
  assert(err.empty());
  note_voice_peer_media_engine_event(&runtime, second_submit_event);
  assert(second_submit_event["event"].asString() == "audio_chunk_transmitted");
  assert(second_submit_event["rtp_packets_sent"].asUInt64() >= 2);
  assert(runtime.rtcp_sender_reports_sent == sender_reports_after_first_submit);

  const auto second_media_deadline =
    std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < second_media_deadline) {
    if (ctx.outbound_rtp_packets_observed >= 2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(ctx.outbound_rtp_packets_observed >= 2);
  assert(ctx.outbound_rtcp_sender_reports_observed == sender_reports_after_first_submit);

  agentd::destroy_dtls_srtp_session_pair(&ctx.client_srtp_sessions);
  destroy_endpoint(&ctx.client);
  juice_destroy(remote_agent);
}

}  // namespace

int main() {
  test_embedded_transport_provider_loads_and_answers_remote_offer();
  test_embedded_transport_provider_mirrors_browser_offer_shape_with_dtls_identity();
  test_embedded_transport_provider_preserves_supported_feedback_and_strips_remote_tracks();
  test_embedded_transport_provider_selects_pcma_when_pcmu_is_unavailable();
  test_embedded_transport_provider_selects_codec_from_accepted_audio_mline();
  test_embedded_transport_provider_reaches_local_ice_connectivity();
  return 0;
}

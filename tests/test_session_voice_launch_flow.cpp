#include "session_voice_launch_flow.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace {

using agentd::VoicePeerBackendStartResult;
using agentd::VoicePeerBrokerSessionBinding;
using agentd::VoicePeerLaunchFlowOps;
using agentd::VoicePeerRuntime;
using agentd::VoicePeerStartupWaitResult;
using agentd::VoicePeerStartPlan;
using agentd::run_voice_peer_launch_flow_with_ops;
using agentd::voice_peer_launch_startup_sequence_json;

static VoicePeerStartPlan make_plan() {
  VoicePeerStartPlan plan;
  plan.runtime_kind = "bundled";
  plan.broker_token = "tok";
  plan.broker_agent_id = "agent-a";
  plan.broker_deployment_id = "deploy-b";
  plan.startup_wait_ms = 456;
  return plan;
}

static void test_launch_flow_runs_success_path() {
  VoicePeerStartPlan plan = make_plan();
  std::vector<std::string> calls;
  auto runtime = std::make_shared<VoicePeerRuntime>();
  runtime->running = true;
  runtime->ready = true;

  VoicePeerLaunchFlowOps ops;
  ops.resolve_broker_session =
    [&calls](
      const VoicePeerStartPlan&,
      VoicePeerBrokerSessionBinding* out_binding,
      int* out_http_status,
      std::string* out_err) {
      calls.push_back("resolve_broker_session");
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_binding) {
        out_binding->broker_session_id = "sess-1";
        out_binding->managed_broker_session = true;
      }
      return true;
    };
  ops.launch_runtime =
    [&calls, &runtime](
      const std::string& session_id,
      const VoicePeerStartPlan&,
      const VoicePeerBrokerSessionBinding& binding,
      std::shared_ptr<VoicePeerRuntime>* out_state,
      std::string* out_err) {
      calls.push_back("launch_runtime");
      assert(session_id == "voice-sid");
      assert(binding.broker_session_id == "sess-1");
      if (out_err) out_err->clear();
      if (out_state) *out_state = runtime;
      return true;
    };
  ops.register_runtime =
    [&calls](const std::string& session_id, const std::shared_ptr<VoicePeerRuntime>&) {
      calls.push_back("register_runtime");
      assert(session_id == "voice-sid");
    };
  ops.persist_runtime =
    [&calls](const VoicePeerRuntime&) {
      calls.push_back("persist_runtime");
    };
  ops.wait_startup =
    [&calls](const std::shared_ptr<VoicePeerRuntime>&, int64_t timeout_ms) {
      calls.push_back("wait_startup");
      assert(timeout_ms == 456);
      VoicePeerStartupWaitResult out;
      out.running = true;
      out.ready = true;
      return out;
    };
  ops.sync_runtime_state =
    [&calls](const std::shared_ptr<VoicePeerRuntime>&) {
      calls.push_back("sync_runtime_state");
    };
  ops.cleanup_runtime =
    [](const std::string&, const std::string&, Json::Value*, std::string* out_err) {
      if (out_err) out_err->clear();
      assert(false && "cleanup_runtime should not run on success");
      return false;
    };
  ops.release_managed_broker_session =
    [](const VoicePeerStartPlan&, const VoicePeerBrokerSessionBinding&, std::string* out_err) {
      if (out_err) out_err->clear();
      assert(false && "release should not run on success");
      return false;
    };

  VoicePeerBackendStartResult result;
  assert(run_voice_peer_launch_flow_with_ops("voice-sid", plan, ops, &result));
  assert(result.ok);
  assert(result.http_status == 200);
  assert(result.startup_confirmed);
  assert(result.state == runtime);
  assert((calls == std::vector<std::string>{
    "resolve_broker_session",
    "launch_runtime",
    "register_runtime",
    "persist_runtime",
    "wait_startup",
    "sync_runtime_state"}));
}

static void test_launch_flow_releases_managed_session_on_launch_failure() {
  VoicePeerStartPlan plan = make_plan();
  bool release_called = false;

  VoicePeerLaunchFlowOps ops;
  ops.resolve_broker_session =
    [](
      const VoicePeerStartPlan&,
      VoicePeerBrokerSessionBinding* out_binding,
      int* out_http_status,
      std::string* out_err) {
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_binding) {
        out_binding->broker_session_id = "sess-managed";
        out_binding->managed_broker_session = true;
      }
      return true;
    };
  ops.launch_runtime =
    [](
      const std::string&,
      const VoicePeerStartPlan&,
      const VoicePeerBrokerSessionBinding&,
      std::shared_ptr<VoicePeerRuntime>*,
      std::string* out_err) {
      if (out_err) *out_err = "spawn failed";
      return false;
    };
  ops.wait_startup = [](const std::shared_ptr<VoicePeerRuntime>&, int64_t) {
    return VoicePeerStartupWaitResult{};
  };
  ops.cleanup_runtime = [](const std::string&, const std::string&, Json::Value*, std::string*) {
    return true;
  };
  ops.release_managed_broker_session =
    [&release_called](const VoicePeerStartPlan&, const VoicePeerBrokerSessionBinding& binding, std::string* out_err) {
      release_called = true;
      assert(binding.managed_broker_session);
      if (out_err) out_err->clear();
      return true;
    };

  VoicePeerBackendStartResult result;
  assert(!run_voice_peer_launch_flow_with_ops("voice-sid", plan, ops, &result));
  assert(result.http_status == 500);
  assert(result.error == "spawn failed");
  assert(release_called);
}

static void test_launch_flow_cleans_up_startup_failure() {
  VoicePeerStartPlan plan = make_plan();
  auto runtime = std::make_shared<VoicePeerRuntime>();
  runtime->running = false;
  runtime->ready = false;
  runtime->last_error = "peer failed before ready";

  bool cleanup_called = false;
  VoicePeerLaunchFlowOps ops;
  ops.resolve_broker_session =
    [](
      const VoicePeerStartPlan&,
      VoicePeerBrokerSessionBinding* out_binding,
      int* out_http_status,
      std::string* out_err) {
      if (out_http_status) *out_http_status = 200;
      if (out_err) out_err->clear();
      if (out_binding) out_binding->broker_session_id = "sess-1";
      return true;
    };
  ops.launch_runtime =
    [&runtime](
      const std::string&,
      const VoicePeerStartPlan&,
      const VoicePeerBrokerSessionBinding&,
      std::shared_ptr<VoicePeerRuntime>* out_state,
      std::string* out_err) {
      if (out_err) out_err->clear();
      if (out_state) *out_state = runtime;
      return true;
    };
  ops.wait_startup =
    [](const std::shared_ptr<VoicePeerRuntime>&, int64_t) {
      return VoicePeerStartupWaitResult{};
    };
  ops.cleanup_runtime =
    [&cleanup_called](const std::string& session_id, const std::string& broker_token, Json::Value* out_cleanup, std::string* out_err) {
      cleanup_called = true;
      assert(session_id == "voice-sid");
      assert(broker_token == "tok");
      if (out_err) out_err->clear();
      if (out_cleanup) {
        (*out_cleanup)["broker_session_deleted"] = true;
      }
      return true;
    };
  ops.release_managed_broker_session =
    [](const VoicePeerStartPlan&, const VoicePeerBrokerSessionBinding&, std::string* out_err) {
      if (out_err) out_err->clear();
      return true;
    };

  VoicePeerBackendStartResult result;
  assert(!run_voice_peer_launch_flow_with_ops("voice-sid", plan, ops, &result));
  assert(result.http_status == 500);
  assert(result.error == "peer failed before ready");
  assert(result.startup_confirmed == false);
  assert(result.startup_cleanup["broker_session_deleted"].asBool());
  assert(cleanup_called);
}

static void test_launch_flow_requires_complete_ops() {
  VoicePeerStartPlan plan = make_plan();
  VoicePeerLaunchFlowOps ops;
  VoicePeerBackendStartResult result;
  assert(!run_voice_peer_launch_flow_with_ops("voice-sid", plan, ops, &result));
  assert(result.http_status == 500);
  assert(result.error == "voice peer launch flow ops incomplete");
}

static void test_startup_sequence_for_borrowed_session_is_not_deferred() {
  VoicePeerStartPlan plan = make_plan();
  plan.requested_broker_session_id = "sess-borrowed";
  const Json::Value seq = voice_peer_launch_startup_sequence_json(plan, true);
  assert(seq.isArray());
  assert(seq.size() == 4);
  assert(seq[0]["stage"].asString() == "borrowed_broker_session_preflight");
  assert(seq[0]["deferred"].asBool() == false);
  assert(seq[1]["stage"].asString() == "launch_runtime");
  assert(seq[1]["deferred"].asBool() == true);
}

}  // namespace

int main() {
  test_launch_flow_runs_success_path();
  test_launch_flow_releases_managed_session_on_launch_failure();
  test_launch_flow_cleans_up_startup_failure();
  test_launch_flow_requires_complete_ops();
  test_startup_sequence_for_borrowed_session_is_not_deferred();
  return 0;
}

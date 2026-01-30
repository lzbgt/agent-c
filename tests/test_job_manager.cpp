#include "job_manager.h"

#include "json_util.h"

#include <json/json.h>

#include <cassert>
#include <chrono>
#include <string>
#include <thread>

using namespace agentd;

static Json::Value ok_result(bool ok) {
  Json::Value r(Json::objectValue);
  r["ok"] = ok;
  return r;
}

static void test_event_ring_trims(void) {
  const std::string id = "jm_events";
  assert(job_create(id));

  // Push enough events to trigger a rebuild (soft max 4608 -> keep last 4096).
  for (int i = 0; i < 5000; i++) {
    job_append_event(id, "tick", "{\"i\":1}");
  }

  JobState s;
  assert(job_get(id, &s));
  assert(s.events.isArray());
  assert((uint64_t)s.events.size() <= 4608);
  assert(s.events_offset > 0);

  job_set_result(id, ok_result(true));
  assert(job_delete(id));
}

static void test_job_gc_ttl_removes_finished(void) {
  const std::string id = "jm_ttl";
  assert(job_create(id));
  job_set_result(id, ok_result(true)); // done

  // Sleep long enough to exceed a tiny TTL. Keep it small but non-zero to avoid flakiness.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  job_gc(/*ttl_ms=*/1, /*max_jobs=*/0);

  JobState s;
  assert(!job_get(id, &s));
}

static void test_job_gc_max_keeps_running(void) {
  const std::string run = "jm_run";
  assert(job_create(run));
  job_set_status(run, "running", "");

  // Create and finish a bunch of jobs.
  for (int i = 0; i < 10; i++) {
    const std::string id = "jm_done_" + std::to_string(i);
    assert(job_create(id));
    job_set_result(id, ok_result(true));
  }

  job_gc(/*ttl_ms=*/0, /*max_jobs=*/5);

  // Running job should never be removed by GC.
  JobState s;
  assert(job_get(run, &s));
  assert(s.status == "running");

  // At most 5 finished jobs should remain; which ones remain is implementation-defined.
  int remaining_done = 0;
  for (int i = 0; i < 10; i++) {
    const std::string id = "jm_done_" + std::to_string(i);
    JobState t;
    if (job_get(id, &t)) {
      remaining_done++;
      assert(t.status == "done" || t.status == "error");
      assert(job_delete(id));
    }
  }
  assert(remaining_done <= 5);

  // Cleanup running job.
  job_set_result(run, ok_result(true));
  assert(job_delete(run));
}

int main() {
  test_event_ring_trims();
  test_job_gc_ttl_removes_finished();
  test_job_gc_max_keeps_running();
  return 0;
}

#include "trace_id_util.h"

#include <cstdio>
#include <cstdlib>
#include <random>

namespace agentd {

bool trace_id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':' || c == '@';
    if (!ok) return false;
  }
  return true;
}

std::string make_uuidish_trace_id() {
  // Best-effort UUIDv4-ish without external deps. Good enough to avoid collisions in practice.
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);

  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);

  // v4 + variant.
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);

  char buf[96];
  (void)snprintf(
    buf,
    sizeof(buf),
    "trace_%08x-%04x-%04x-%04x-%012llx",
    a,
    (unsigned)b,
    (unsigned)c,
    (unsigned)d,
    (unsigned long long)(e & 0xffffffffffffull)
  );
  return std::string(buf);
}

}  // namespace agentd


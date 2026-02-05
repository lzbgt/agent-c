#pragma once

#include <string>

namespace agentd {

// Shared trace_id validator used across endpoints and edge interop.
// Keep this consistent with UM-EAIS / trace correlation docs.
bool trace_id_is_safe(const std::string& s);

// Best-effort UUIDv4-ish trace id generator (no external deps).
// Format: trace_<uuidish>
std::string make_uuidish_trace_id();

}  // namespace agentd


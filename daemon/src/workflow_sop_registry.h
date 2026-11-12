#pragma once

#include "agent_db.h"
#include <json/json.h>
#include <string>

namespace agentd {
// Resolves an exact immutable version into a copied inline definition. No execution.
// 0 on success/no reference; otherwise an HTTP status and a safe diagnostic.
int workflow_resolve_sop_reference(AgentDb* db, Json::Value* args, std::string* error);
}  // namespace agentd

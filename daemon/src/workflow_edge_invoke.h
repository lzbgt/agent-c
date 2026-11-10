#pragma once
#include "agent_db.h"
#include <json/json.h>
namespace agentd {
Json::Value workflow_edge_invoke_to_json(AgentDb *db,
                                         const AgentDb::WorkflowRow &workflow,
                                         const AgentDb::WorkflowTaskRow &task,
                                         const Json::Value &request);
}

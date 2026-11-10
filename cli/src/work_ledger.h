#pragma once
#include <json/json.h>
#include <filesystem>
#include <string>

// A host-side, transactionally persisted requirement/blocker ledger. Each mutation
// changes one item; open blockers affect only their explicit requirement IDs.
bool work_ledger_apply(const std::filesystem::path& path, const Json::Value& request,
                       Json::Value* response, std::string* error);

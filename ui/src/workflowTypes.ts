import type { components as AgentdComponents, paths as AgentdPaths } from "./api/generated/agentd-openapi";

export type WorkflowJsonPrimitive = string | number | boolean | null;
export type WorkflowJsonValue = WorkflowJsonPrimitive | WorkflowJsonObject | WorkflowJsonValue[];
export type WorkflowJsonObject = { [key: string]: WorkflowJsonValue | undefined };

export type WorkflowSubmitRequest = AgentdComponents["schemas"]["WorkflowSubmitRequest"];
export type WorkflowGetResponse = AgentdComponents["schemas"]["WorkflowGetResponse"];
export type WorkflowApiListResponse =
  AgentdPaths["/api/v1/workflows"]["get"]["responses"][200]["content"]["application/json"];
export type WorkflowApiSubmitResponse =
  AgentdPaths["/api/v1/workflow/submit"]["post"]["responses"][200]["content"]["application/json"];
export type WorkflowApiSchedule = AgentdComponents["schemas"]["WorkflowSchedule"];
export type WorkflowApiScheduleRun = AgentdComponents["schemas"]["WorkflowScheduleRun"];
export type WorkflowApiScheduleCreateRequest = AgentdComponents["schemas"]["WorkflowScheduleCreateRequest"];

export type WorkflowToolCallLimit = {
  tool: string;
  max_calls: number;
};

export type WorkflowDefaults = WorkflowJsonObject & {
  tools?: string;
  host_policy?: string;
  automation_profile?: string;
  yolo?: boolean;
  verbose?: boolean;
  model?: string;
  summary_model?: string;
  summary_max_chars?: number;
  base_url?: string;
  api_key?: string;
  proxy?: string;
  timeout_ms?: number;
  stream_assistant?: boolean;
  max_capture_bytes?: number;
  max_steps?: number;
  max_repeated_tool_calls?: number;
  max_tool_calls_total?: number;
  max_tool_calls_per_tool?: number;
  tool_call_limits?: WorkflowToolCallLimit[];
  max_chars?: number;
  keep_last?: number;
  memory_context_mode?: string;
  memory_include_structured?: boolean;
  memory_include_core?: boolean;
  memory_include_daily?: boolean;
  memory_include_session?: boolean;
  memory_daily_days?: number;
  memory_total_cap?: number;
  memory_search_query?: string;
  memory_search_use_index?: boolean;
  memory_search_case_sensitive?: boolean;
  memory_search_fallback_to_files?: boolean;
  memory_search_order?: string;
  memory_search_max_results?: number;
  memory_search_max_snippet_chars?: number;
  memory_search_context_lines?: number;
  trace?: boolean;
};

export type WorkflowPromptRequest = {
  prompt: string;
  no_session: true;
};

export type WorkflowRequestTask = {
  task_id: string;
  depends_on?: string[];
  request: WorkflowPromptRequest;
};

export type WorkflowAgentdParallelTarget = {
  id: string;
  base_url: string;
};

export type WorkflowAgentdParallelAggregate = {
  mode: "first_ok";
  task_ids: string[];
  ok_pointer: string;
  value_pointer: string;
};

export type WorkflowNestedSpec = WorkflowJsonObject & {
  tasks: WorkflowRequestTask[];
  defaults?: WorkflowDefaults;
  allow_inline_api_keys?: boolean;
};

export type WorkflowAgentdParallelTask = {
  task_id: string;
  depends_on?: string[];
  kind: "agentd_parallel";
  agentd_parallel: {
    targets: WorkflowAgentdParallelTarget[];
    agentd_call: {
      op: "workflow_submit_and_wait";
      timeout_ms: number;
      poll_ms: number;
      include_results: boolean;
      include_tasks: boolean;
      bearer_env?: string;
      workflow: WorkflowNestedSpec;
    };
    aggregate: WorkflowAgentdParallelAggregate;
  };
};

export type WorkflowTaskSpec = WorkflowRequestTask | WorkflowAgentdParallelTask;

export type WorkflowSpec = WorkflowJsonObject & {
  tasks: WorkflowTaskSpec[];
  defaults?: WorkflowDefaults;
  allow_inline_api_keys?: boolean;
  inputs?: Record<string, string>;
};

export type WorkflowScheduleSpec = WorkflowSubmitRequest;

export type WorkflowScheduleCreatePayload = {
  schedule_id?: string;
  cron: string;
  timezone?: string;
  spec: WorkflowSubmitRequest;
  metadata?: WorkflowJsonObject;
};

export type WorkflowSummaryRow = WorkflowJsonObject & {
  workflow_id: string;
  status?: string;
  priority?: number;
  deadline_unix_ms?: number;
  idempotency_key?: string;
  trace_id?: string;
  session_id?: string;
  cancel_requested?: boolean;
  error?: string;
  created_unix_ms?: number;
  updated_unix_ms?: number;
};

export type WorkflowTaskRow = WorkflowJsonObject & {
  task_id: string;
  status?: string;
  depends_on?: string[];
  allow_error?: boolean;
  attempt?: number;
  max_attempts?: number;
  error?: string;
  ready_unix_ms?: number;
  started_unix_ms?: number;
  finished_unix_ms?: number;
};

export type WorkflowBudgetSnapshot = WorkflowJsonObject;

export type WorkflowScheduleRow = WorkflowJsonObject & {
  schedule_id: string;
  status?: string;
  cron?: string;
  timezone?: string;
  created_unix_ms?: number;
  updated_unix_ms?: number;
  last_tick_unix_ms?: number;
  next_tick_unix_ms?: number;
  last_error?: string;
  metadata?: WorkflowJsonObject;
};

export type WorkflowScheduleRunRow = WorkflowJsonObject & {
  schedule_id: string;
  tick_unix_ms?: number;
  workflow_id: string;
  created_unix_ms?: number;
  status?: string;
  error?: string;
};

export type MemberSession = {
  memberId: string;
  sessionId: string;
};

export type TeamRunHandoffEventRecord = {
  handoff_id?: string;
  kind?: string;
  state?: string;
  from_role?: string;
  to_role?: string;
  reason?: string;
  message?: string;
  source_deployment_id?: string;
  source_session_id?: string;
  target_deployment_id?: string;
  target_session_id?: string;
  ts_unix_ms?: number;
  event_index?: number;
  data?: Record<string, unknown>;
};

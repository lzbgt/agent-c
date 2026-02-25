package main

import (
	"fmt"
	"time"
)

type config struct {
	brokerBase     string
	oidcToken      string
	oidcTokenFile  string
	insecureTLS    bool
	pollInterval   time.Duration
	once           bool
	limit          int
	status         string
	includePlanned bool
	orchestratorID string
}

type teamRow struct {
	TeamID string `json:"team_id"`
}

type teamListResponse struct {
	OK    bool      `json:"ok"`
	Teams []teamRow `json:"teams"`
}

type orchestratorRun struct {
	OrchestratorRunID string         `json:"orchestrator_run_id"`
	TeamID            string         `json:"team_id"`
	Status            string         `json:"status"`
	Goal              string         `json:"goal"`
	GoalContract      map[string]any `json:"goal_contract"`
	RolePlanSnapshot  map[string]any `json:"role_plan_snapshot"`
	Meta              map[string]any `json:"meta"`
	LastHeartbeatUnix *int64         `json:"last_heartbeat_unix_ms"`
	HeartbeatAgeMS    *int64         `json:"heartbeat_age_ms"`
	LeaseTimeoutMS    *int64         `json:"lease_timeout_ms"`
	LeaseStatus       string         `json:"lease_status"`
}

type orchestratorRunListResponse struct {
	OK    bool              `json:"ok"`
	Team  string            `json:"team_id"`
	Runs  []orchestratorRun `json:"runs"`
	Error string            `json:"error"`
	Err   string            `json:"err"`
	Code  string            `json:"code"`
	Meta  map[string]any    `json:"meta"`
	Extra map[string]any    `json:"extra"`
	Raw   map[string]any    `json:"-"`
	Other map[string]any    `json:"-"`
}

type orchestratorRunResponse struct {
	OK   bool            `json:"ok"`
	Team string          `json:"team_id"`
	Run  orchestratorRun `json:"run"`
}

type teamRunResponse struct {
	OK                      bool           `json:"ok"`
	TeamID                  string         `json:"team_id"`
	TeamRunID               string         `json:"team_run_id"`
	Status                  string         `json:"status"`
	Mode                    string         `json:"mode"`
	CreatedUnixMS           int64          `json:"created_unix_ms"`
	AutoAllocateMissing     any            `json:"auto_allocate_missing_roles"`
	AutoAllocateWarning     string         `json:"auto_allocate_warning"`
	GoalContract            any            `json:"goal_contract"`
	HandoffEvents           any            `json:"handoff_events"`
	MemberJobs              any            `json:"member_jobs"`
	RuntimeMembers          any            `json:"runtime_members"`
	DispatchErrors          any            `json:"dispatch_errors"`
	MemberJobSummary        any            `json:"member_job_summary"`
	MemberOverridesApplied  any            `json:"member_overrides_applied"`
	RoleOverridesApplied    any            `json:"role_overrides_applied"`
	RunOverridesMode        any            `json:"run_overrides_mode"`
	SharedMemoryScopeID     any            `json:"shared_memory_scope_id"`
	SharedMemoryMode        any            `json:"shared_memory_mode"`
	AutoAllocateRoles       any            `json:"auto_allocate_roles"`
	AutoAllocateAllocated   any            `json:"auto_allocate_allocated_roles"`
	CancelRequestedUnixMS   any            `json:"cancel_requested_unix_ms"`
	CancelResults           any            `json:"cancel_results"`
	GoalEvents              any            `json:"goal_events"`
	MemberSessions          any            `json:"member_sessions"`
	AutoAllocateMaxMembers  any            `json:"auto_allocate_max_members"`
	AutoAllocateWarningText string         `json:"auto_allocate_warning"`
	Extra                   map[string]any `json:"-"`
}

type spawnRequest struct {
	SpawnRequestID string `json:"spawn_request_id"`
	Role           string `json:"role"`
	Status         string `json:"status"`
	Count          int    `json:"count"`
}

type spawnListResponse struct {
	OK            bool           `json:"ok"`
	TeamID        string         `json:"team_id"`
	SpawnRequests []spawnRequest `json:"spawn_requests"`
}

type spawnCreateResponse struct {
	OK           bool         `json:"ok"`
	TeamID       string       `json:"team_id"`
	SpawnRequest spawnRequest `json:"spawn_request"`
}

type runtimeAllocateResponse struct {
	OK             bool             `json:"ok"`
	TeamID         string           `json:"team_id"`
	RuntimeMembers []map[string]any `json:"runtime_members"`
	AllocatedRoles []string         `json:"allocated_roles"`
	MissingRoles   []string         `json:"missing_roles"`
	Warnings       []string         `json:"warnings"`
}

type guidanceEvent struct {
	GuidanceID         string         `json:"guidance_id"`
	TeamID             string         `json:"team_id"`
	TeamRunID          string         `json:"team_run_id"`
	Kind               string         `json:"kind"`
	Priority           string         `json:"priority"`
	Message            string         `json:"message"`
	Payload            map[string]any `json:"payload"`
	TargetRoles        []string       `json:"target_roles"`
	TargetMemberIDs    []string       `json:"target_member_ids"`
	TargetAgentIDs     []string       `json:"target_agent_ids"`
	TargetOrchestrator string         `json:"target_orchestrator_id"`
	CreatedUnixMS      int64          `json:"created_unix_ms"`
	ExpiresUnixMS      int64          `json:"expires_unix_ms"`
	Status             string         `json:"status"`
	AckedBy            string         `json:"acked_by"`
	AckedUnixMS        int64          `json:"acked_unix_ms"`
	AckNote            string         `json:"ack_note"`
}

type guidanceListResponse struct {
	OK       bool            `json:"ok"`
	TeamID   string          `json:"team_id"`
	TeamRun  string          `json:"team_run_id"`
	Count    int             `json:"count"`
	Guidance []guidanceEvent `json:"guidance"`
}

type guidanceAckResponse struct {
	OK       bool           `json:"ok"`
	TeamID   string         `json:"team_id"`
	Guidance guidanceEvent  `json:"guidance"`
	Receipt  map[string]any `json:"receipt"`
}

type moderatorDispatchResponse struct {
	OK         bool             `json:"ok"`
	TeamID     string           `json:"team_id"`
	TeamRunID  string           `json:"team_run_id"`
	Dispatched []map[string]any `json:"dispatched"`
	Skipped    []map[string]any `json:"skipped"`
}

type httpError struct {
	Status int
	Body   string
}

func (e *httpError) Error() string {
	return fmt.Sprintf("http %d: %s", e.Status, e.Body)
}

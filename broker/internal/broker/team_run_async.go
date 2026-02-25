package broker

import (
	"context"
	"encoding/json"
	"errors"
	"net/url"
	"strings"
	"time"

	"agentd-broker/internal/db"
)

const (
	teamRunDispatchMinTimeoutMS = 1_000
	teamRunDispatchMaxTimeoutMS = 30_000
	teamRunStatusPollTimeout    = 5 * time.Second
)

func (s *Server) executeTeamRunAsync(
	ctx context.Context,
	p *Principal,
	teamID string,
	teamRunID string,
	runMap map[string]any,
	teamMeta map[string]any,
	runMembers []db.TeamMember,
	memberRunBodies [][]byte,
	options teamRunOptions,
	teamRunRules []db.TeamQuorumRule,
	approvals []teamRunApproval,
	membersByID map[string]db.TeamMember,
	quorumEval *teamRunQuorumEval,
	traceID string,
) (map[string]any, error) {
	if p == nil {
		return nil, errors.New("missing principal")
	}
	dispatchTimeout := options.TimeoutMS
	if dispatchTimeout < teamRunDispatchMinTimeoutMS {
		dispatchTimeout = teamRunDispatchMinTimeoutMS
	}
	if dispatchTimeout > teamRunDispatchMaxTimeoutMS {
		dispatchTimeout = teamRunDispatchMaxTimeoutMS
	}

	tasks := make([]agentTaskPrepared, 0, len(runMembers))
	for i, member := range runMembers {
		body := mustJSON(runMap)
		if i < len(memberRunBodies) {
			body = memberRunBodies[i]
		}
		tasks = append(tasks, agentTaskPrepared{
			TaskID:       "member_" + itoa(i),
			AgentID:      member.AgentID,
			DeploymentID: member.DeploymentID,
			Method:       "POST",
			Path:         "/api/v1/run_async",
			Query:        "",
			Headers:      map[string]string{},
			Body:         body,
		})
	}

	results := s.executeAgentTasks(context.Background(), p, tasks, options.MaxConcurrency, dispatchTimeout, traceID)
	nowMs := time.Now().UTC().UnixMilli()
	memberJobs := make([]map[string]any, 0, len(runMembers))
	dispatchErrors := make([]map[string]any, 0)
	started := 0

	for i, member := range runMembers {
		entry := map[string]any{
			"member_id": member.MemberID,
			"agent_id":  member.AgentID,
		}
		if member.DeploymentID != "" {
			entry["deployment_id"] = member.DeploymentID
		}
		if i < len(results) {
			res := results[i]
			if res.HTTPStatus != 0 {
				entry["http_status"] = res.HTTPStatus
			}
			if res.OK {
				if jobID, ok := res.Result["job_id"].(string); ok {
					jobID = strings.TrimSpace(jobID)
					if jobID != "" {
						entry["job_id"] = jobID
						entry["status"] = "queued"
						entry["updated_unix_ms"] = nowMs
						started += 1
					} else {
						entry["dispatch_error"] = "missing job_id"
					}
				} else {
					entry["dispatch_error"] = "missing job_id"
				}
			} else {
				errStr := strings.TrimSpace(res.Error)
				if errStr == "" {
					errStr = "dispatch failed"
				}
				entry["dispatch_error"] = errStr
			}
		} else {
			entry["dispatch_error"] = "missing dispatch result"
		}
		if derr, ok := entry["dispatch_error"].(string); ok && derr != "" {
			dispatchErrors = append(dispatchErrors, map[string]any{
				"member_id": member.MemberID,
				"agent_id":  member.AgentID,
				"error":     derr,
			})
		}
		memberJobs = append(memberJobs, entry)
	}

	if len(memberJobs) > 0 {
		teamMeta["member_jobs"] = memberJobs
	} else {
		delete(teamMeta, "member_jobs")
	}
	if len(dispatchErrors) > 0 {
		teamMeta["dispatch_errors"] = dispatchErrors
	} else {
		delete(teamMeta, "dispatch_errors")
	}

	status := "running"
	if started == 0 {
		status = "failed"
	}

	runPayload := map[string]any{
		"run":  runMap,
		"team": teamMeta,
	}
	run, err := s.cfg.DB.CreateTeamRun(ctx, teamRunID, teamID, status, p.Sub, mustJSON(runPayload))
	if err != nil {
		return nil, errors.New("create team run failed")
	}

	if len(approvals) > 0 {
		if err := s.persistTeamRunApprovals(ctx, teamID, teamRunID, teamRunRules, approvals, membersByID, p.Sub); err != nil {
			_ = s.cfg.DB.UpdateTeamRunStatus(ctx, teamID, teamRunID, "failed")
			return nil, err
		}
	}
	if len(teamRunRules) > 0 {
		publishTeamQuorumRequest(s.cfg.Events, p.Sub, teamID, teamRunID, teamRunRules, traceID)
		if quorumEval != nil {
			publishTeamQuorumResult(s.cfg.Events, p.Sub, teamID, teamRunID, *quorumEval, traceID)
		}
	}

	resp := map[string]any{
		"ok":              true,
		"team_id":         run.TeamID,
		"team_run_id":     run.TeamRunID,
		"status":          status,
		"mode":            options.Mode,
		"created_unix_ms": run.CreatedAt.UnixMilli(),
	}
	if len(memberJobs) > 0 {
		resp["member_jobs"] = memberJobs
	}
	if len(dispatchErrors) > 0 {
		resp["dispatch_errors"] = dispatchErrors
	}
	if summary := teamRunMemberJobSummary(teamMeta); summary != nil {
		resp["member_job_summary"] = summary
	}
	return resp, nil
}

func (s *Server) reconcileTeamRunJobs(ctx context.Context, p *Principal, run *db.TeamRun, runPayload map[string]any, teamMeta map[string]any) (string, error) {
	if run == nil {
		return "", errors.New("missing run")
	}
	if p == nil {
		return run.Status, nil
	}
	items := teamRunMemberJobsFromMeta(teamMeta)
	if len(items) == 0 {
		return run.Status, nil
	}

	hasDispatchErrors := teamRunDispatchErrorCount(teamMeta) > 0
	cancelRequested := false
	if v, ok := teamMeta["cancel_requested_unix_ms"]; ok && v != nil {
		cancelRequested = true
	}

	running := false
	failed := false
	cancelled := false
	changed := false
	updated := make([]map[string]any, 0, len(items))

	for _, item := range items {
		entry := map[string]any{}
		for k, v := range item {
			entry[k] = v
		}
		agentID, _ := entry["agent_id"].(string)
		deploymentID, _ := entry["deployment_id"].(string)
		jobID, _ := entry["job_id"].(string)
		dispatchErr, _ := entry["dispatch_error"].(string)

		if dispatchErr != "" {
			failed = true
			updated = append(updated, entry)
			continue
		}
		if strings.TrimSpace(jobID) == "" {
			failed = true
			entry["dispatch_error"] = "missing job_id"
			changed = true
			updated = append(updated, entry)
			continue
		}
		if agentID != "" {
			if ok, err := s.canAccessAgent(ctx, p, agentID); err != nil {
				entry["error"] = "access check failed"
				running = true
				changed = true
				updated = append(updated, entry)
				continue
			} else if !ok {
				entry["error"] = "forbidden"
				failed = true
				changed = true
				updated = append(updated, entry)
				continue
			}
		}

		q := "job_id=" + url.QueryEscape(jobID)
		jobCtx, cancel := context.WithTimeout(ctx, teamRunStatusPollTimeout)
		ro := s.relayAgentHTTP(jobCtx, p, agentID, deploymentID, "GET", "/api/v1/job", q, map[string]string{}, nil)
		cancel()

		if ro.BrokerStatus != 0 {
			entry["error"] = ro.Err
			entry["status"] = "unknown"
			running = true
			changed = true
			updated = append(updated, entry)
			continue
		}
		if ro.AgentStatus < 200 || ro.AgentStatus >= 300 {
			entry["error"] = "job status failed"
			entry["http_status"] = ro.AgentStatus
			entry["status"] = "unknown"
			running = true
			changed = true
			updated = append(updated, entry)
			continue
		}

		var payload map[string]any
		if err := json.Unmarshal(ro.Body, &payload); err != nil {
			entry["error"] = "invalid job status json"
			entry["status"] = "unknown"
			running = true
			changed = true
			updated = append(updated, entry)
			continue
		}
		status, _ := payload["status"].(string)
		status = strings.ToLower(strings.TrimSpace(status))
		if status == "" {
			status = "unknown"
		}

		updatedUnix := int64(0)
		if v, ok := payload["updated_unix_ms"].(float64); ok {
			updatedUnix = int64(v)
		} else if v, ok := payload["updated_unix_ms"].(int64); ok {
			updatedUnix = v
		}
		if updatedUnix > 0 {
			if prev, ok := entry["updated_unix_ms"].(int64); !ok || prev != updatedUnix {
				entry["updated_unix_ms"] = updatedUnix
				changed = true
			}
		}

		if status == "queued" || status == "running" {
			running = true
		} else if status == "error" {
			failed = true
		} else if status == "cancelled" || status == "interrupted" {
			cancelled = true
		}

		if prev, ok := entry["status"].(string); !ok || prev != status {
			entry["status"] = status
			changed = true
		}

		if status == "done" || status == "error" || status == "cancelled" || status == "interrupted" {
			if res, ok := payload["result"].(map[string]any); ok {
				if okVal, ok := res["ok"].(bool); ok {
					if prev, ok := entry["ok"].(bool); !ok || prev != okVal {
						entry["ok"] = okVal
						changed = true
					}
					if !okVal {
						failed = true
					}
				}
				if errStr, ok := res["error"].(string); ok && errStr != "" {
					if prev, ok := entry["error"].(string); !ok || prev != errStr {
						entry["error"] = errStr
						changed = true
					}
					failed = true
				}
			}
		}

		updated = append(updated, entry)
	}

	overall := run.Status
	if running {
		if cancelRequested {
			overall = "cancelling"
		} else {
			overall = "running"
		}
	} else if failed || hasDispatchErrors {
		overall = "failed"
	} else if cancelled {
		overall = "cancelled"
	} else {
		overall = "succeeded"
	}

	if changed {
		teamMeta["member_jobs"] = updated
		runPayload["team"] = teamMeta
		payloadJSON := mustJSON(runPayload)
		if err := s.cfg.DB.UpdateTeamRunPayload(ctx, run.TeamID, run.TeamRunID, payloadJSON); err == nil {
			run.RunJSON = payloadJSON
		}
	}
	if overall != run.Status {
		_ = s.cfg.DB.UpdateTeamRunStatus(ctx, run.TeamID, run.TeamRunID, overall)
		run.Status = overall
	}

	return overall, nil
}

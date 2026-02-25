package broker

import (
	"context"
	"errors"
	"net/url"
	"strings"
	"time"

	"agentd-broker/internal/db"
)

const teamRunCancelTimeoutMS = 10_000

func (s *Server) cancelTeamRunJobs(
	ctx context.Context,
	p *Principal,
	run *db.TeamRun,
	runPayload map[string]any,
	teamMeta map[string]any,
	maxConcurrency int,
	traceID string,
) ([]map[string]any, error) {
	if run == nil {
		return nil, errors.New("missing run")
	}
	if p == nil {
		return nil, errors.New("missing principal")
	}
	items := teamRunMemberJobsFromMeta(teamMeta)
	if len(items) == 0 {
		return nil, errors.New("no member jobs to cancel")
	}
	if maxConcurrency < 1 {
		maxConcurrency = 4
	}
	nowMs := time.Now().UTC().UnixMilli()
	cancelResults := make([]map[string]any, 0, len(items))
	tasks := make([]agentTaskPrepared, 0, len(items))
	taskIndex := make([]int, 0, len(items))

	for _, item := range items {
		memberID, _ := item["member_id"].(string)
		agentID, _ := item["agent_id"].(string)
		deploymentID, _ := item["deployment_id"].(string)
		jobID, _ := item["job_id"].(string)
		memberID = strings.TrimSpace(memberID)
		agentID = strings.TrimSpace(agentID)
		deploymentID = strings.TrimSpace(deploymentID)
		jobID = strings.TrimSpace(jobID)

		entry := map[string]any{
			"member_id": memberID,
			"agent_id":  agentID,
		}
		if deploymentID != "" {
			entry["deployment_id"] = deploymentID
		}
		if jobID != "" {
			entry["job_id"] = jobID
		}
		cancelResults = append(cancelResults, entry)
		idx := len(cancelResults) - 1

		if jobID == "" {
			entry["ok"] = false
			entry["error"] = "missing job_id"
			continue
		}
		if agentID == "" {
			entry["ok"] = false
			entry["error"] = "missing agent_id"
			continue
		}

		tasks = append(tasks, agentTaskPrepared{
			TaskID:       "cancel_" + itoa(len(tasks)),
			AgentID:      agentID,
			DeploymentID: deploymentID,
			Method:       "POST",
			Path:         "/api/v1/job/cancel",
			Query:        "job_id=" + url.QueryEscape(jobID),
			Headers:      map[string]string{},
		})
		taskIndex = append(taskIndex, idx)
	}

	if len(tasks) > 0 {
		results := s.executeAgentTasks(ctx, p, tasks, maxConcurrency, teamRunCancelTimeoutMS, traceID)
		for i, res := range results {
			idx := taskIndex[i]
			if idx < 0 || idx >= len(cancelResults) {
				continue
			}
			entry := cancelResults[idx]
			if res.HTTPStatus != 0 {
				entry["http_status"] = res.HTTPStatus
			}
			if res.OK {
				entry["ok"] = true
			} else {
				entry["ok"] = false
				if errStr := strings.TrimSpace(res.Error); errStr != "" {
					entry["error"] = errStr
				}
			}
			if res.Result != nil {
				if errStr, ok := res.Result["error"].(string); ok && strings.TrimSpace(errStr) != "" {
					entry["error"] = errStr
				}
			}
			if _, ok := entry["error"]; !ok && !res.OK {
				entry["error"] = "cancel failed"
			}
		}
	}

	teamMeta["cancel_requested_unix_ms"] = nowMs
	teamMeta["cancel_results"] = cancelResults
	runPayload["team"] = teamMeta
	payloadJSON := mustJSON(runPayload)
	if err := s.cfg.DB.UpdateTeamRunPayload(ctx, run.TeamID, run.TeamRunID, payloadJSON); err != nil {
		return cancelResults, err
	}
	run.RunJSON = payloadJSON

	switch run.Status {
	case "running", "queued", "cancelling":
		if err := s.cfg.DB.UpdateTeamRunStatus(ctx, run.TeamID, run.TeamRunID, "cancelling"); err == nil {
			run.Status = "cancelling"
		}
	}

	return cancelResults, nil
}

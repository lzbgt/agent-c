package broker

import (
	"context"
	"encoding/json"
	"strings"
	"sync/atomic"
	"time"
)

type agentTaskPrepared struct {
	TaskID       string
	AgentID      string
	DeploymentID string
	Method       string
	Path         string
	Query        string

	Headers map[string]string
	Body    []byte
}

type agentTaskResult struct {
	TaskID       string         `json:"task_id"`
	AgentID      string         `json:"agent_id"`
	DeploymentID string         `json:"deployment_id,omitempty"`
	OK           bool           `json:"ok"`
	MS           int            `json:"ms"`
	HTTPStatus   int            `json:"http_status,omitempty"`
	Error        string         `json:"error,omitempty"`
	Result       map[string]any `json:"result,omitempty"`
}

func (s *Server) executeAgentTasks(ctx context.Context, p *Principal, tasks []agentTaskPrepared, maxConcurrency, timeoutMS int, traceID string) []agentTaskResult {
	if maxConcurrency < 1 {
		maxConcurrency = 1
	}
	if maxConcurrency > 16 {
		maxConcurrency = 16
	}
	if maxConcurrency > len(tasks) {
		maxConcurrency = len(tasks)
	}
	if maxConcurrency < 1 {
		maxConcurrency = 1
	}

	out := make([]agentTaskResult, len(tasks))
	var next atomic.Uint32

	worker := func() {
		for {
			idx := int(next.Add(1) - 1)
			if idx >= len(tasks) {
				return
			}
			t := tasks[idx]

			fwdHeaders := map[string]string{}
			for k, v := range t.Headers {
				if strings.TrimSpace(k) == "" {
					continue
				}
				kl := strings.ToLower(strings.TrimSpace(k))
				if kl == "authorization" || kl == "host" || kl == "connection" {
					continue
				}
				fwdHeaders[k] = v
			}
			fwdHeaders["X-Agentd-Broker-User"] = p.Sub
			if traceID != "" {
				if _, ok := fwdHeaders["X-Trace-ID"]; !ok {
					fwdHeaders["X-Trace-ID"] = traceID
				}
			}

			taskCtx := ctx
			timeout := time.Duration(timeoutMS) * time.Millisecond
			var cancel context.CancelFunc
			if timeout > 0 {
				taskCtx, cancel = context.WithTimeout(taskCtx, timeout)
			}

			start := time.Now()
			ro := s.relayAgentHTTP(taskCtx, p, t.AgentID, t.DeploymentID, t.Method, t.Path, t.Query, fwdHeaders, t.Body)
			ms := int(time.Since(start).Milliseconds())
			if cancel != nil {
				cancel()
			}

			row := agentTaskResult{
				TaskID:       t.TaskID,
				AgentID:      t.AgentID,
				DeploymentID: t.DeploymentID,
				MS:           ms,
			}
			if ro.BrokerStatus != 0 {
				row.OK = false
				row.Error = ro.Err
				out[idx] = row
				continue
			}
			row.HTTPStatus = ro.AgentStatus

			var resultObj map[string]any
			if err := json.Unmarshal(ro.Body, &resultObj); err == nil {
				row.Result = resultObj
				if v, ok := resultObj["ok"].(bool); ok {
					row.OK = v
				} else {
					row.OK = ro.AgentStatus >= 200 && ro.AgentStatus < 300
				}
			} else {
				row.OK = ro.AgentStatus >= 200 && ro.AgentStatus < 300
				row.Error = "invalid json response"
			}
			out[idx] = row
		}
	}

	done := make(chan struct{}, maxConcurrency)
	for i := 0; i < maxConcurrency; i++ {
		go func() {
			worker()
			done <- struct{}{}
		}()
	}
	for i := 0; i < maxConcurrency; i++ {
		<-done
	}
	return out
}

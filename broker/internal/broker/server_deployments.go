package broker

import (
	"encoding/json"
	"errors"
	"net/http"
	"strings"
)

func deploymentIDFromRequest(r *http.Request) (string, error) {
	if r == nil {
		return "", nil
	}
	raw := strings.TrimSpace(r.Header.Get("X-Agentd-Deployment"))
	if raw == "" {
		raw = strings.TrimSpace(r.URL.Query().Get("deployment_id"))
	}
	if raw == "" {
		raw = strings.TrimSpace(r.URL.Query().Get("deployment"))
	}
	if raw == "" {
		return "", nil
	}
	if !deploymentIDRe.MatchString(raw) {
		return "", errors.New("invalid deployment_id")
	}
	return raw, nil
}

func parseDeploymentIDs(raw []string) ([]string, error) {
	seen := map[string]struct{}{}
	out := make([]string, 0, len(raw))
	for _, id := range raw {
		s := strings.TrimSpace(id)
		if s == "" {
			continue
		}
		if !deploymentIDRe.MatchString(s) {
			return nil, errors.New("invalid deployment_id")
		}
		if _, ok := seen[s]; ok {
			continue
		}
		seen[s] = struct{}{}
		out = append(out, s)
	}
	return out, nil
}

func deploymentIDsFromBody(body []byte) ([]string, error) {
	if len(body) == 0 {
		return nil, nil
	}
	var obj map[string]any
	if err := json.Unmarshal(body, &obj); err != nil {
		return nil, errors.New("invalid json")
	}
	raw := make([]string, 0, 8)
	if v, ok := obj["deployment_ids"]; ok {
		arr, ok := v.([]any)
		if !ok {
			return nil, errors.New("deployment_ids must be an array of strings")
		}
		for _, item := range arr {
			s, ok := item.(string)
			if !ok {
				return nil, errors.New("deployment_ids must be an array of strings")
			}
			raw = append(raw, s)
		}
	}
	if v, ok := obj["deployment_id"]; ok {
		s, ok := v.(string)
		if !ok {
			return nil, errors.New("deployment_id must be a string")
		}
		raw = append(raw, s)
	}
	if v, ok := obj["deployments"]; ok {
		arr, ok := v.([]any)
		if !ok {
			return nil, errors.New("deployments must be an array of strings")
		}
		for _, item := range arr {
			s, ok := item.(string)
			if !ok {
				return nil, errors.New("deployments must be an array of strings")
			}
			raw = append(raw, s)
		}
	}
	return parseDeploymentIDs(raw)
}

func deploymentIDsFromQuery(r *http.Request) ([]string, error) {
	if r == nil {
		return nil, nil
	}
	raw := make([]string, 0, 8)
	if dep, _ := deploymentIDFromRequest(r); dep != "" {
		raw = append(raw, dep)
	}
	if v := strings.TrimSpace(r.URL.Query().Get("deployment_ids")); v != "" {
		parts := strings.Split(v, ",")
		for _, p := range parts {
			raw = append(raw, p)
		}
	}
	if v := strings.TrimSpace(r.URL.Query().Get("deployments")); v != "" {
		parts := strings.Split(v, ",")
		for _, p := range parts {
			raw = append(raw, p)
		}
	}
	return parseDeploymentIDs(raw)
}

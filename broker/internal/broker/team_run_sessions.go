package broker

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
)

const (
	teamRunSessionMaxLen = 200
)

func isSessionIDSafe(s string) bool {
	if s == "" {
		return false
	}
	if len(s) > teamRunSessionMaxLen {
		return false
	}
	if strings.Contains(s, "/") || strings.Contains(s, "\\") {
		return false
	}
	if s == "." || s == ".." || strings.Contains(s, "..") {
		return false
	}
	for _, c := range s {
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' {
			continue
		}
		return false
	}
	return true
}

func sanitizeSessionToken(raw string) string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return "x"
	}
	var b strings.Builder
	b.Grow(len(raw))
	for _, c := range raw {
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' {
			b.WriteRune(c)
		} else {
			b.WriteByte('_')
		}
	}
	out := b.String()
	out = strings.Trim(out, "_")
	if out == "" {
		return "x"
	}
	return out
}

func makeTeamRunSessionID(teamID, teamRunID, memberID string) string {
	base := "team_" + sanitizeSessionToken(teamID) + "_run_" + sanitizeSessionToken(teamRunID) + "_member_" + sanitizeSessionToken(memberID)
	if isSessionIDSafe(base) {
		return base
	}
	sum := sha256.Sum256([]byte(teamID + "|" + teamRunID + "|" + memberID))
	return "teamrun_" + hex.EncodeToString(sum[:8])
}

func teamRunMemberSessionsFromMeta(teamMeta map[string]any) map[string]string {
	if teamMeta == nil {
		return nil
	}
	raw, ok := teamMeta["member_sessions"]
	if !ok || raw == nil {
		return nil
	}
	obj, ok := raw.(map[string]any)
	if !ok {
		return nil
	}
	out := map[string]string{}
	for k, v := range obj {
		if k == "" {
			continue
		}
		s, ok := v.(string)
		if !ok {
			continue
		}
		s = strings.TrimSpace(s)
		if s == "" {
			continue
		}
		out[k] = s
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

package db

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

type IdempotencyStatus string

const (
	IdempotencyCreated    IdempotencyStatus = "created"
	IdempotencyReplay     IdempotencyStatus = "replay"
	IdempotencyInProgress IdempotencyStatus = "in_progress"
	IdempotencyConflict   IdempotencyStatus = "conflict"
)

type IdempotencyRecord struct {
	UserSub       string
	Key           string
	RequestSHA256 string
	Method        string
	Path          string
	Query         string
	AgentID       string
	ExpiresAt     time.Time
	CreatedAt     time.Time
	Completed     bool

	ResponseStatus  int
	ResponseHeaders map[string]string
	ResponseBody    []byte
}

type IdempotencyResult struct {
	Status IdempotencyStatus
	Record *IdempotencyRecord
}

func (d *DB) ClaimIdempotency(ctx context.Context, rec IdempotencyRecord) (IdempotencyResult, error) {
	if d == nil || d.Pool == nil {
		return IdempotencyResult{}, errors.New("db not open")
	}
	rec.UserSub = strings.TrimSpace(rec.UserSub)
	rec.Key = strings.TrimSpace(rec.Key)
	if rec.UserSub == "" || rec.Key == "" {
		return IdempotencyResult{}, errors.New("missing user sub or idempotency key")
	}
	if strings.TrimSpace(rec.RequestSHA256) == "" {
		return IdempotencyResult{}, errors.New("missing request hash")
	}
	if rec.ExpiresAt.IsZero() {
		rec.ExpiresAt = time.Now().Add(24 * time.Hour)
	}
	if err := d.EnsureUser(ctx, rec.UserSub); err != nil {
		return IdempotencyResult{}, err
	}

	tx, err := d.Pool.Begin(ctx)
	if err != nil {
		return IdempotencyResult{}, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	_, err = tx.Exec(ctx, `
		DELETE FROM broker_idempotency_keys
		WHERE user_sub=$1 AND idempotency_key=$2 AND expires_at < now()
	`, rec.UserSub, rec.Key)
	if err != nil {
		return IdempotencyResult{}, err
	}

	tag, err := tx.Exec(ctx, `
		INSERT INTO broker_idempotency_keys(
			user_sub, idempotency_key, request_sha256, method, path, query, agent_id, expires_at
		)
		VALUES($1, $2, $3, $4, $5, $6, $7, $8)
		ON CONFLICT DO NOTHING
	`, rec.UserSub, rec.Key, rec.RequestSHA256, rec.Method, rec.Path, rec.Query, rec.AgentID, rec.ExpiresAt)
	if err != nil {
		return IdempotencyResult{}, err
	}
	if tag.RowsAffected() > 0 {
		if err := tx.Commit(ctx); err != nil {
			return IdempotencyResult{}, err
		}
		return IdempotencyResult{Status: IdempotencyCreated}, nil
	}

	row := tx.QueryRow(ctx, `
		SELECT request_sha256, completed, response_status, response_headers::text, response_body,
			expires_at, ts, method, path, query, agent_id
		FROM broker_idempotency_keys
		WHERE user_sub=$1 AND idempotency_key=$2
		FOR UPDATE
	`, rec.UserSub, rec.Key)
	var reqHash string
	var completed bool
	var respStatus int
	var respHeadersText string
	var respBody []byte
	var expiresAt time.Time
	var createdAt time.Time
	var method, path, query, agentID string
	if err := row.Scan(&reqHash, &completed, &respStatus, &respHeadersText, &respBody, &expiresAt, &createdAt, &method, &path, &query, &agentID); err != nil {
		return IdempotencyResult{}, err
	}
	if time.Now().After(expiresAt) {
		_, err := tx.Exec(ctx, `
			DELETE FROM broker_idempotency_keys
			WHERE user_sub=$1 AND idempotency_key=$2
		`, rec.UserSub, rec.Key)
		if err != nil {
			return IdempotencyResult{}, err
		}
		if err := tx.Commit(ctx); err != nil {
			return IdempotencyResult{}, err
		}
		return IdempotencyResult{Status: IdempotencyCreated}, nil
	}
	if reqHash != rec.RequestSHA256 {
		if err := tx.Commit(ctx); err != nil {
			return IdempotencyResult{}, err
		}
		return IdempotencyResult{Status: IdempotencyConflict}, nil
	}
	if completed {
		headers := map[string]string{}
		if strings.TrimSpace(respHeadersText) != "" {
			_ = json.Unmarshal([]byte(respHeadersText), &headers)
		}
		recOut := &IdempotencyRecord{
			UserSub:         rec.UserSub,
			Key:             rec.Key,
			RequestSHA256:   reqHash,
			Method:          method,
			Path:            path,
			Query:           query,
			AgentID:         agentID,
			ExpiresAt:       expiresAt,
			CreatedAt:       createdAt,
			Completed:       true,
			ResponseStatus:  respStatus,
			ResponseHeaders: headers,
			ResponseBody:    respBody,
		}
		if err := tx.Commit(ctx); err != nil {
			return IdempotencyResult{}, err
		}
		return IdempotencyResult{Status: IdempotencyReplay, Record: recOut}, nil
	}
	if err := tx.Commit(ctx); err != nil {
		return IdempotencyResult{}, err
	}
	return IdempotencyResult{Status: IdempotencyInProgress}, nil
}

func (d *DB) CompleteIdempotency(
	ctx context.Context,
	userSub string,
	key string,
	status int,
	headers map[string]string,
	body []byte,
) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	userSub = strings.TrimSpace(userSub)
	key = strings.TrimSpace(key)
	if userSub == "" || key == "" {
		return errors.New("missing user sub or idempotency key")
	}
	hb, _ := json.Marshal(coalesceStringMap(headers))
	if len(hb) == 0 {
		hb = []byte("{}")
	}
	_, err := d.Pool.Exec(ctx, `
		UPDATE broker_idempotency_keys
		SET completed=true, response_status=$3, response_headers=$4::jsonb, response_body=$5
		WHERE user_sub=$1 AND idempotency_key=$2
	`, userSub, key, status, string(hb), body)
	return err
}

func (d *DB) DeleteIdempotency(ctx context.Context, userSub, key string) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	userSub = strings.TrimSpace(userSub)
	key = strings.TrimSpace(key)
	if userSub == "" || key == "" {
		return errors.New("missing user sub or idempotency key")
	}
	_, err := d.Pool.Exec(ctx, `
		DELETE FROM broker_idempotency_keys
		WHERE user_sub=$1 AND idempotency_key=$2
	`, userSub, key)
	return err
}

func coalesceStringMap(m map[string]string) map[string]string {
	if m == nil {
		return map[string]string{}
	}
	return m
}

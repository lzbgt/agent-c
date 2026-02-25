package db

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

type BrokerEvent struct {
	EventID  string
	UserSub  string
	Type     string
	TSUnixMS int64
	JSON     json.RawMessage
}

func (d *DB) InsertBrokerEvents(ctx context.Context, userSubs []string, eventID, eventType string, tsUnixMS int64, eventJSON []byte) error {
	if d == nil || d.Pool == nil {
		return errors.New("db not open")
	}
	eventID = strings.TrimSpace(eventID)
	eventType = strings.TrimSpace(eventType)
	if eventID == "" || eventType == "" {
		return errors.New("missing event_id or event_type")
	}
	if tsUnixMS == 0 {
		tsUnixMS = time.Now().UnixMilli()
	}
	raw := strings.TrimSpace(string(eventJSON))
	if raw == "" {
		raw = "{}"
	}
	seen := map[string]bool{}
	subs := make([]string, 0, len(userSubs))
	for _, sub := range userSubs {
		sub = strings.TrimSpace(sub)
		if sub == "" || seen[sub] {
			continue
		}
		seen[sub] = true
		subs = append(subs, sub)
	}
	if len(subs) == 0 {
		return nil
	}
	for _, sub := range subs {
		if err := d.EnsureUser(ctx, sub); err != nil {
			return err
		}
	}
	var batch pgx.Batch
	for _, sub := range subs {
		batch.Queue(`
			INSERT INTO broker_events(user_sub, event_id, event_type, ts_unix_ms, event_json)
			VALUES($1, $2, $3, $4, $5::jsonb)
			ON CONFLICT (user_sub, event_id) DO NOTHING
		`, sub, eventID, eventType, tsUnixMS, raw)
	}
	br := d.Pool.SendBatch(ctx, &batch)
	if err := br.Close(); err != nil {
		return err
	}
	return nil
}

func (d *DB) ListBrokerEvents(ctx context.Context, userSub string, sinceTS int64, limit int, types []string) ([]BrokerEvent, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	userSub = strings.TrimSpace(userSub)
	if userSub == "" {
		return nil, errors.New("missing user sub")
	}
	if err := d.EnsureUser(ctx, userSub); err != nil {
		return nil, err
	}
	if limit <= 0 {
		limit = 200
	}
	if limit > 1000 {
		limit = 1000
	}
	if sinceTS < 0 {
		sinceTS = 0
	}
	filtered := make([]string, 0, len(types))
	for _, t := range types {
		v := strings.TrimSpace(t)
		if v != "" {
			filtered = append(filtered, v)
		}
	}
	var rows pgx.Rows
	var err error
	if len(filtered) == 0 {
		rows, err = d.Pool.Query(ctx, `
			SELECT event_id, event_type, ts_unix_ms, event_json::text
			FROM broker_events
			WHERE user_sub=$1 AND ts_unix_ms >= $2
			ORDER BY ts_unix_ms ASC, event_id ASC
			LIMIT $3
		`, userSub, sinceTS, limit)
	} else {
		rows, err = d.Pool.Query(ctx, `
			SELECT event_id, event_type, ts_unix_ms, event_json::text
			FROM broker_events
			WHERE user_sub=$1 AND ts_unix_ms >= $2 AND event_type = ANY($3)
			ORDER BY ts_unix_ms ASC, event_id ASC
			LIMIT $4
		`, userSub, sinceTS, filtered, limit)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := []BrokerEvent{}
	for rows.Next() {
		var row BrokerEvent
		var raw string
		if err := rows.Scan(&row.EventID, &row.Type, &row.TSUnixMS, &raw); err != nil {
			return nil, err
		}
		row.UserSub = userSub
		row.JSON = json.RawMessage(raw)
		out = append(out, row)
	}
	return out, rows.Err()
}

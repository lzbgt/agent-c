package db

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"time"

	"github.com/jackc/pgx/v5"
)

type ClientPrefs struct {
	OwnerSub   string
	ClientKind string
	ClientID   string
	Version    int
	PrefsJSON  json.RawMessage
	UpdatedAt  time.Time
}

func (c ClientPrefs) Prefs() map[string]any {
	if len(c.PrefsJSON) == 0 {
		return map[string]any{}
	}
	var out map[string]any
	_ = json.Unmarshal(c.PrefsJSON, &out)
	if out == nil {
		out = map[string]any{}
	}
	return out
}

func (d *DB) GetClientPrefs(
	ctx context.Context,
	ownerSub string,
	clientKind string,
	clientID string,
) (*ClientPrefs, bool, error) {
	if d == nil || d.Pool == nil {
		return nil, false, errors.New("db not open")
	}
	ownerSub = strings.TrimSpace(ownerSub)
	clientKind = strings.TrimSpace(clientKind)
	clientID = strings.TrimSpace(clientID)
	if ownerSub == "" || clientKind == "" || clientID == "" {
		return nil, false, errors.New("missing owner sub or client tokens")
	}
	var out ClientPrefs
	var prefsText string
	err := d.Pool.QueryRow(ctx, `
    SELECT owner_sub, client_kind, client_id, version, prefs::text, updated_at
    FROM broker_client_prefs
    WHERE owner_sub=$1 AND client_kind=$2 AND client_id=$3
  `, ownerSub, clientKind, clientID).Scan(
		&out.OwnerSub,
		&out.ClientKind,
		&out.ClientID,
		&out.Version,
		&prefsText,
		&out.UpdatedAt,
	)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, false, nil
		}
		return nil, false, err
	}
	out.PrefsJSON = json.RawMessage(prefsText)
	return &out, true, nil
}

func (d *DB) UpsertClientPrefs(
	ctx context.Context,
	ownerSub string,
	clientKind string,
	clientID string,
	version int,
	prefs map[string]any,
) (*ClientPrefs, error) {
	if d == nil || d.Pool == nil {
		return nil, errors.New("db not open")
	}
	ownerSub = strings.TrimSpace(ownerSub)
	clientKind = strings.TrimSpace(clientKind)
	clientID = strings.TrimSpace(clientID)
	if ownerSub == "" || clientKind == "" || clientID == "" {
		return nil, errors.New("missing owner sub or client tokens")
	}
	if version <= 0 {
		version = 1
	}
	prefsJSON, _ := json.Marshal(coalesceMap(prefs))
	var out ClientPrefs
	var prefsText string
	err := d.Pool.QueryRow(ctx, `
    INSERT INTO broker_client_prefs(owner_sub, client_kind, client_id, version, prefs)
    VALUES($1, $2, $3, $4, $5::jsonb)
    ON CONFLICT (owner_sub, client_kind, client_id)
    DO UPDATE SET version=$4, prefs=$5::jsonb, updated_at=NOW()
    RETURNING owner_sub, client_kind, client_id, version, prefs::text, updated_at
  `, ownerSub, clientKind, clientID, version, string(prefsJSON)).Scan(
		&out.OwnerSub,
		&out.ClientKind,
		&out.ClientID,
		&out.Version,
		&prefsText,
		&out.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	out.PrefsJSON = json.RawMessage(prefsText)
	return &out, nil
}

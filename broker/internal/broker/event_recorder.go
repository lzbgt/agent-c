package broker

import (
	"context"
	"encoding/json"

	"agentd-broker/internal/db"
	"agentd-broker/internal/events"
)

type brokerEventRecorder struct {
	db *db.DB
}

func newBrokerEventRecorder(dbx *db.DB) *brokerEventRecorder {
	if dbx == nil {
		return nil
	}
	return &brokerEventRecorder{db: dbx}
}

func (r *brokerEventRecorder) Record(userSubs []string, e events.Event) error {
	if r == nil || r.db == nil {
		return nil
	}
	raw, err := json.Marshal(e)
	if err != nil {
		return err
	}
	return r.db.InsertBrokerEvents(context.Background(), userSubs, e.EventID, e.Type, e.TSUnixMS, raw)
}

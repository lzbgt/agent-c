package gateway

import (
	"context"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"github.com/jackc/pgx/v5/pgxpool"
	shared "github.com/lzbgt/hubstack-payments/sdk/identity"
	"time"
)

//go:embed schema.sql
var schema string

type attempt struct {
	StateHash, BrowserHash, Provider, Verifier string
	Expires                                    time.Time
}
type session struct {
	TokenHash, AccountID string
	Revision             int64
	Expires              time.Time
	Proof                shared.Proof
}
type store interface {
	AddAttempt(context.Context, attempt) error
	TakeAttempt(context.Context, string, string, string) (string, error)
	PutSession(context.Context, session) error
	Session(context.Context, string) (session, error)
	DeleteSession(context.Context, string) error
	Ping(context.Context) error
}
type postgresStore struct{ pool *pgxpool.Pool }

func openStore(ctx context.Context, dsn string) (*postgresStore, error) {
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		return nil, errors.New("invalid gateway database configuration")
	}
	cfg.MaxConns = 8
	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, errors.New("cannot open gateway database")
	}
	s := &postgresStore{pool}
	if err = s.bootstrap(ctx); err != nil {
		pool.Close()
		return nil, err
	}
	return s, nil
}
func (s *postgresStore) bootstrap(ctx context.Context) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if _, err = tx.Exec(ctx, "SELECT pg_advisory_xact_lock(842906112)"); err != nil {
		return err
	}
	var exists bool
	if err = tx.QueryRow(ctx, "SELECT to_regnamespace('agentc_gateway') IS NOT NULL").Scan(&exists); err != nil {
		return err
	}
	sum := sha256.Sum256([]byte(schema))
	fingerprint := hex.EncodeToString(sum[:])
	if exists {
		var stored string
		if err = tx.QueryRow(ctx, "SELECT fingerprint FROM agentc_gateway.schema_snapshot WHERE id=1").Scan(&stored); err != nil || stored != fingerprint {
			return errors.New("gateway schema differs from this rolling snapshot; deploy a fresh database, no automatic migration is performed")
		}
	} else {
		if _, err = tx.Exec(ctx, schema); err != nil {
			return err
		}
		if _, err = tx.Exec(ctx, "INSERT INTO agentc_gateway.schema_snapshot VALUES(1,$1)", fingerprint); err != nil {
			return err
		}
	}
	return tx.Commit(ctx)
}
func (s *postgresStore) AddAttempt(ctx context.Context, a attempt) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if _, err = tx.Exec(ctx, "SELECT pg_advisory_xact_lock(842906113)"); err != nil {
		return err
	}
	if _, err = tx.Exec(ctx, "DELETE FROM agentc_gateway.login_attempts WHERE expires_at<=now()"); err != nil {
		return err
	}
	var n int
	if err = tx.QueryRow(ctx, "SELECT count(*) FROM agentc_gateway.login_attempts").Scan(&n); err != nil {
		return err
	}
	if n >= 512 {
		return errors.New("login admission limit")
	}
	_, err = tx.Exec(ctx, "INSERT INTO agentc_gateway.login_attempts VALUES($1,$2,$3,$4,$5)", a.StateHash, a.BrowserHash, a.Provider, a.Verifier, a.Expires)
	if err != nil {
		return err
	}
	return tx.Commit(ctx)
}
func (s *postgresStore) TakeAttempt(ctx context.Context, state, browser, provider string) (string, error) {
	var verifier string
	err := s.pool.QueryRow(ctx, "DELETE FROM agentc_gateway.login_attempts WHERE state_hash=$1 AND browser_hash=$2 AND provider=$3 AND expires_at>now() RETURNING verifier", state, browser, provider).Scan(&verifier)
	return verifier, err
}
func (s *postgresStore) PutSession(ctx context.Context, v session) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return err
	}
	defer tx.Rollback(ctx)
	if _, err = tx.Exec(ctx, "DELETE FROM agentc_gateway.sessions WHERE expires_at<=now()"); err != nil {
		return err
	}
	proof, err := json.Marshal(v.Proof)
	if err != nil {
		return err
	}
	if _, err = tx.Exec(ctx, "INSERT INTO agentc_gateway.sessions(token_hash,account_id,revision,expires_at,proof) VALUES($1,$2,$3,$4,$5::jsonb)", v.TokenHash, v.AccountID, v.Revision, v.Expires, string(proof)); err != nil {
		return err
	}
	return tx.Commit(ctx)
}
func (s *postgresStore) Session(ctx context.Context, hash string) (session, error) {
	var v session
	var proof []byte
	err := s.pool.QueryRow(ctx, "SELECT token_hash,account_id,revision,expires_at,proof FROM agentc_gateway.sessions WHERE token_hash=$1 AND expires_at>now()", hash).Scan(&v.TokenHash, &v.AccountID, &v.Revision, &v.Expires, &proof)
	if err == nil {
		err = json.Unmarshal(proof, &v.Proof)
	}
	return v, err
}
func (s *postgresStore) DeleteSession(ctx context.Context, hash string) error {
	_, err := s.pool.Exec(ctx, "DELETE FROM agentc_gateway.sessions WHERE token_hash=$1", hash)
	return err
}
func (s *postgresStore) Ping(ctx context.Context) error { return s.pool.Ping(ctx) }

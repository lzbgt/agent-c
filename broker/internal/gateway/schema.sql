CREATE SCHEMA agentc_gateway;
CREATE TABLE agentc_gateway.schema_snapshot(id integer PRIMARY KEY CHECK(id=1), fingerprint text NOT NULL);
CREATE TABLE agentc_gateway.login_attempts(
 state_hash text PRIMARY KEY, browser_hash text NOT NULL, provider text NOT NULL,
 verifier text NOT NULL, expires_at timestamptz NOT NULL);
CREATE INDEX login_attempt_expiry ON agentc_gateway.login_attempts(expires_at);
CREATE TABLE agentc_gateway.sessions(
 token_hash text PRIMARY KEY, account_id text NOT NULL, revision bigint NOT NULL,
 expires_at timestamptz NOT NULL);
CREATE INDEX session_expiry ON agentc_gateway.sessions(expires_at);

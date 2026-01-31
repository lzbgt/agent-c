-- Initialize multiple databases in one Postgres container.
-- This runs once on first boot (fresh volume) via docker-entrypoint-initdb.d.

CREATE DATABASE agentd_broker;
CREATE DATABASE keycloak;


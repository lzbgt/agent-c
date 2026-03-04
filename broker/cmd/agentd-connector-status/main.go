package main

import (
	"bytes"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

type statusPayload struct {
	Status    string `json:"status"`
	LastError string `json:"last_error,omitempty"`
	TsUnixMs  int64  `json:"ts_unix_ms"`
}

func envString(key string) string {
	return strings.TrimSpace(os.Getenv(key))
}

func envDurationMS(key string) (time.Duration, bool) {
	raw := envString(key)
	if raw == "" {
		return 0, false
	}
	n, err := time.ParseDuration(raw)
	if err == nil {
		return n, true
	}
	if ms, err := time.ParseDuration(raw + "ms"); err == nil {
		return ms, true
	}
	return 0, false
}

func buildClient(tlsCA string) (*http.Client, error) {
	if strings.TrimSpace(tlsCA) == "" {
		return &http.Client{Timeout: 15 * time.Second}, nil
	}
	b, err := os.ReadFile(strings.TrimSpace(tlsCA))
	if err != nil {
		return nil, fmt.Errorf("read tls ca: %w", err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(b) {
		return nil, fmt.Errorf("invalid tls ca pem")
	}
	tr := &http.Transport{TLSClientConfig: &tls.Config{MinVersion: tls.VersionTLS12, RootCAs: pool}}
	return &http.Client{Timeout: 15 * time.Second, Transport: tr}, nil
}

func buildStatusURL(base, connectorID string) (string, error) {
	trimmed := strings.TrimRight(base, "/")
	if trimmed == "" {
		return "", fmt.Errorf("broker base required")
	}
	u, err := url.Parse(trimmed)
	if err != nil {
		return "", fmt.Errorf("invalid broker base: %w", err)
	}
	if u.Scheme == "" {
		u.Scheme = "https"
	}
	if u.Path == "" {
		u.Path = "/v1/connectors/" + url.PathEscape(connectorID) + "/status"
	} else {
		u.Path = strings.TrimRight(u.Path, "/") + "/v1/connectors/" + url.PathEscape(connectorID) + "/status"
	}
	return u.String(), nil
}

func postStatus(client *http.Client, url, token string, payload statusPayload) error {
	body, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	req, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	if strings.TrimSpace(token) != "" {
		req.Header.Set("Authorization", "Bearer "+strings.TrimSpace(token))
	}
	resp, err := client.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fmt.Errorf("status update failed: http %d", resp.StatusCode)
	}
	return nil
}

func main() {
	var brokerBase = flag.String("broker-base", "", "broker base url (https://host:port)")
	var authToken = flag.String("auth-token", "", "broker bearer token (admin)")
	var connectorID = flag.String("connector-id", "", "connector id to update")
	var status = flag.String("status", "ready", "status label")
	var lastError = flag.String("last-error", "", "last error string")
	var interval = flag.Duration("interval", 0, "repeat interval (0=send once)")
	var tlsCA = flag.String("tls-ca", "", "CA PEM to verify broker TLS")
	flag.Parse()

	if strings.TrimSpace(*brokerBase) == "" {
		*brokerBase = envString("AGENTD_BROKER_BASE")
	}
	if strings.TrimSpace(*authToken) == "" {
		*authToken = envString("AGENTD_BROKER_AUTH_TOKEN")
	}
	if strings.TrimSpace(*connectorID) == "" {
		*connectorID = envString("AGENTD_BROKER_CONNECTOR_ID")
	}
	if strings.TrimSpace(*status) == "" {
		if v := envString("AGENTD_BROKER_CONNECTOR_STATUS"); v != "" {
			*status = v
		}
	}
	if strings.TrimSpace(*lastError) == "" {
		*lastError = envString("AGENTD_BROKER_CONNECTOR_LAST_ERROR")
	}
	if strings.TrimSpace(*tlsCA) == "" {
		*tlsCA = envString("AGENTD_BROKER_TLS_CA")
	}
	if *interval == 0 {
		if v, ok := envDurationMS("AGENTD_BROKER_CONNECTOR_INTERVAL"); ok {
			*interval = v
		}
	}
	if strings.TrimSpace(*connectorID) == "" {
		log.Fatalf("missing connector id")
	}

	client, err := buildClient(*tlsCA)
	if err != nil {
		log.Fatalf("http client: %v", err)
	}
	statusURL, err := buildStatusURL(strings.TrimSpace(*brokerBase), strings.TrimSpace(*connectorID))
	if err != nil {
		log.Fatalf("status url: %v", err)
	}

	sendOnce := func() {
		payload := statusPayload{Status: strings.TrimSpace(*status), LastError: strings.TrimSpace(*lastError), TsUnixMs: time.Now().UnixMilli()}
		if err := postStatus(client, statusURL, strings.TrimSpace(*authToken), payload); err != nil {
			log.Printf("status update failed: %v", err)
			return
		}
		log.Printf("connector status updated: %s", statusURL)
	}

	if *interval <= 0 {
		sendOnce()
		return
	}

	ticker := time.NewTicker(*interval)
	defer ticker.Stop()
	for {
		sendOnce()
		<-ticker.C
	}
}

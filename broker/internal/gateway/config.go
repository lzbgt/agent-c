package gateway

import (
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	shared "github.com/lzbgt/hubstack-payments/sdk/identity"
	"io"
	"net"
	"net/url"
	"os"
	"strings"
	"syscall"
)

type Config struct {
	SourceRevision     string         `json:"source_revision"`
	Listen             string         `json:"listen"`
	PublicURL          string         `json:"public_url"`
	WebDir             string         `json:"web_dir"`
	DatabaseURL        string         `json:"database_url"`
	DaemonURL          string         `json:"daemon_url"`
	DaemonToken        string         `json:"daemon_token"`
	BrokerURL          string         `json:"broker_url"`
	BrokerToken        string         `json:"broker_token"`
	BrokerCAFile       string         `json:"broker_ca_file"`
	OperatorToken      string         `json:"operator_token"`
	ApplicationID      string         `json:"application_id"`
	InstanceID         string         `json:"instance_id"`
	IdentityCredential string         `json:"identity_credential"`
	AdminAccountIDs    []string       `json:"admin_account_ids"`
	AdminIdentities    []shared.Proof `json:"admin_identities"`
	AdminEmail         string         `json:"admin_email"`
}

func LoadConfig(path string) (Config, error) {
	var cfg Config
	f, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW|syscall.O_NONBLOCK, 0)
	if err != nil {
		return cfg, errors.New("cannot read private gateway configuration")
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil || !st.Mode().IsRegular() || st.Mode().Perm()&0077 != 0 || st.Size() > 16384 {
		return cfg, errors.New("gateway configuration must be an owner-only regular file, at most 16 KiB")
	}
	d := json.NewDecoder(io.LimitReader(f, 16385))
	d.DisallowUnknownFields()
	if d.Decode(&cfg) != nil || d.Decode(new(any)) != io.EOF {
		return cfg, errors.New("invalid gateway configuration")
	}
	return cfg, cfg.Validate()
}

func hexID(value string, n int) bool {
	b, err := hex.DecodeString(value)
	return err == nil && len(b) == n && strings.ToLower(value) == value
}
func (c Config) Validate() error {
	u, err := url.Parse(c.PublicURL)
	if err != nil || u.Scheme != "https" || u.Hostname() == "" || u.User != nil || u.Path != "" || u.RawQuery != "" || u.ForceQuery || u.Fragment != "" {
		return errors.New("public_url must be an HTTPS origin")
	}
	host, _, listenErr := net.SplitHostPort(c.Listen)
	address := net.ParseIP(host)
	if listenErr != nil || address == nil || (!address.IsPrivate() && !address.IsLoopback()) {
		return errors.New("listen requires a private/loopback IP address and port")
	}
	if c.ApplicationID != "agent-c" || !hexID(c.IdentityCredential, 32) || !hexID(c.OperatorToken, 32) || c.InstanceID == "" || c.DatabaseURL == "" || c.WebDir == "" || c.DaemonToken == "" || (len(c.AdminAccountIDs) == 0 && len(c.AdminIdentities) == 0) {
		return errors.New("missing gateway identity, administrator, storage or upstream configuration")
	}
	for _, id := range c.AdminAccountIDs {
		if !strings.HasPrefix(id, "usr_") || !hexID(strings.TrimPrefix(id, "usr_"), 16) {
			return errors.New("administrator must be a verified immutable shared account ID")
		}
	}
	for _, proof := range c.AdminIdentities {
		if providerNamespace(proof.Provider) == "" || proof.Namespace != providerNamespace(proof.Provider) || proof.Subject == "" || len(proof.Subject) > 255 {
			return errors.New("administrator proof must pin an exact verified provider, namespace and subject")
		}
	}
	for _, raw := range []string{c.DaemonURL, c.BrokerURL} {
		if raw == "" {
			continue
		}
		target, e := url.Parse(raw)
		if e != nil || (target.Scheme != "http" && target.Scheme != "https") || target.Hostname() == "" || target.User != nil || target.Path != "" || target.RawQuery != "" || target.Fragment != "" {
			return errors.New("upstreams must be explicit HTTP(S) origins")
		}
	}
	if c.DaemonURL == "" || (c.BrokerURL != "" && c.BrokerToken == "") {
		return errors.New("missing upstream authentication")
	}
	return nil
}
func (c Config) isAdmin(account string, proof shared.Proof) bool {
	for _, id := range c.AdminAccountIDs {
		if subtle.ConstantTimeCompare([]byte(id), []byte(account)) == 1 {
			return true
		}
	}
	for _, trusted := range c.AdminIdentities {
		if trusted == proof {
			return true
		}
	}
	return false
}

package localui

import (
	"errors"
	"net"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Config describes an on-demand, loopback-only frontend to an existing agentd.
// It does not start an agent, store model credentials or contact a cloud service.
type Config struct {
	Listen               string
	DaemonURL            string
	DaemonTokenFile      string
	LoginKeyFile         string
	WebDir               string
	Model                string
	ModelBaseURL         string
	MaxOutputTokens      int
	MaxChars             int
	TimeoutMS            int
	MaxSteps             int
	MaxToolCallsTotal    int
	MaxRepeatedToolCalls int
	MaxToolCallArgsChars int
	MaxToolResultChars   int
}

func loopbackURL(raw string, allowPath bool) (*url.URL, error) {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "http" || u.User != nil || u.RawQuery != "" || u.Fragment != "" || u.Opaque != "" {
		return nil, errors.New("upstream URL must be plain HTTP on a loopback IP")
	}
	ip := net.ParseIP(u.Hostname())
	port, err := strconv.Atoi(u.Port())
	if ip == nil || !ip.IsLoopback() || err != nil || port < 1 || port > 65535 || (!allowPath && u.Path != "" && u.Path != "/") {
		return nil, errors.New("upstream URL must use an explicit loopback IP and port")
	}
	return u, nil
}

func (c Config) Validate() error {
	host, port, err := net.SplitHostPort(c.Listen)
	ip := net.ParseIP(host)
	n, e := strconv.Atoi(port)
	if err != nil || ip == nil || !ip.IsLoopback() || e != nil || n < 1 || n > 65535 {
		return errors.New("listen must be an explicit loopback IP and port")
	}
	if _, err := loopbackURL(c.DaemonURL, false); err != nil {
		return err
	}
	if _, err := loopbackURL(c.ModelBaseURL, true); err != nil {
		return err
	}
	if strings.TrimSpace(c.Model) == "" || len(c.Model) > 256 {
		return errors.New("model must be set")
	}
	if c.MaxOutputTokens < 1 || c.MaxOutputTokens > 32768 || c.MaxChars < 1024 || c.MaxChars > 1048576 || c.TimeoutMS < 1000 || c.TimeoutMS > 1800000 {
		return errors.New("invalid local model limits")
	}
	for _, limit := range []int{c.MaxSteps, c.MaxToolCallsTotal, c.MaxRepeatedToolCalls, c.MaxToolCallArgsChars, c.MaxToolResultChars} {
		if limit < 1 || limit > 1048576 {
			return errors.New("local tool limits must be between 1 and 1048576")
		}
	}
	if !filepath.IsAbs(c.WebDir) {
		return errors.New("web-dir must be absolute")
	}
	st, err := os.Stat(filepath.Join(c.WebDir, "index.html"))
	if err != nil || !st.Mode().IsRegular() {
		return errors.New("web-dir must contain a built index.html")
	}
	return nil
}

func readPrivateKey(name string) (string, error) {
	st, err := os.Stat(name)
	if err != nil || !st.Mode().IsRegular() || st.Mode().Perm()&0077 != 0 || st.Size() > 4096 {
		return "", errors.New("key file must be a private regular file (mode 0600)")
	}
	b, err := os.ReadFile(name)
	if err != nil {
		return "", errors.New("cannot read private key file")
	}
	s := strings.TrimSpace(string(b))
	if len(s) < 16 || len(s) > 1024 || strings.ContainsAny(s, "\r\n\t ") {
		return "", errors.New("invalid private key file")
	}
	return s, nil
}

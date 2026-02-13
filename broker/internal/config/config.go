package config

import (
	"encoding/json"
	"errors"
	"os"
	"strings"

	"agentd-broker/internal/auth"
)

type ClientSpec struct {
	ClientID      string   `json:"client_id"`
	Token         string   `json:"token"`
	Admin         bool     `json:"admin"`
	AllowedAgents []string `json:"allowed_agents"`
}

type AuthFile struct {
	Clients []ClientSpec `json:"clients"`
}

func LoadClientAuthFromFile(path string) (*auth.ClientAuth, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var af AuthFile
	if err := json.Unmarshal(b, &af); err != nil {
		return nil, err
	}
	return BuildClientAuth(af.Clients)
}

func BuildClientAuth(specs []ClientSpec) (*auth.ClientAuth, error) {
	ca := &auth.ClientAuth{ByToken: map[string]*auth.ClientPolicy{}}
	for _, s := range specs {
		tok := strings.TrimSpace(s.Token)
		if tok == "" {
			return nil, errors.New("client token empty")
		}
		if _, ok := ca.ByToken[tok]; ok {
			return nil, errors.New("duplicate client token")
		}
		id := strings.TrimSpace(s.ClientID)
		if id == "" {
			return nil, errors.New("client id empty")
		}
		p := &auth.ClientPolicy{
			ClientID:      id,
			Token:         tok,
			Admin:         s.Admin,
			AllowedAgents: map[string]bool{},
		}
		for _, a := range s.AllowedAgents {
			aa := strings.TrimSpace(a)
			if aa == "" {
				continue
			}
			p.AllowedAgents[aa] = true
		}
		ca.ByToken[tok] = p
	}
	return ca, nil
}

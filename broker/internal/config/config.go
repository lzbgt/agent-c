package config

import (
	"encoding/json"
	"errors"
	"os"
	"strings"

	"agentd-broker/internal/auth"
	"agentd-broker/internal/connectors"
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

type ConnectorSpec struct {
	ID          string         `json:"id"`
	Name        string         `json:"name,omitempty"`
	Kind        string         `json:"kind,omitempty"`
	Status      string         `json:"status,omitempty"`
	Description string         `json:"description,omitempty"`
	Meta        map[string]any `json:"meta,omitempty"`
}

type ConnectorFile struct {
	Connectors []ConnectorSpec `json:"connectors"`
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

func LoadConnectorsFromFile(path string) ([]connectors.Connector, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var list []ConnectorSpec
	if err := json.Unmarshal(b, &list); err == nil {
		return BuildConnectors(list)
	}
	var wrapper ConnectorFile
	if err := json.Unmarshal(b, &wrapper); err == nil && wrapper.Connectors != nil {
		return BuildConnectors(wrapper.Connectors)
	}
	return nil, errors.New("invalid connectors file")
}

func BuildConnectors(specs []ConnectorSpec) ([]connectors.Connector, error) {
	out := make([]connectors.Connector, 0, len(specs))
	for _, spec := range specs {
		id := strings.TrimSpace(spec.ID)
		if id == "" {
			return nil, errors.New("connector id empty")
		}
		out = append(out, connectors.Connector{
			ID:          id,
			Name:        strings.TrimSpace(spec.Name),
			Kind:        strings.TrimSpace(spec.Kind),
			Status:      strings.TrimSpace(spec.Status),
			Description: strings.TrimSpace(spec.Description),
			Meta:        spec.Meta,
		})
	}
	return out, nil
}

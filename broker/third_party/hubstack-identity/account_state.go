package identity

import "context"

// AccountState is authority for product-local authentication only. It carries no
// grant and cannot substitute for the fresh shared session required for linking,
// billing or all-product account deletion. Missing membership is inactive/zero.
type AccountState struct {
	ApplicationID string `json:"application_id"`
	InstanceID    string `json:"instance_id"`
	AccountID     string `json:"account_id"`
	Active        bool   `json:"active"`
	Revision      int64  `json:"revision"`
}

func (c *Client) AccountState(ctx context.Context, account string) (AccountState, error) {
	if !accountID(account) {
		return AccountState{}, ErrRejected
	}
	var out struct {
		ApplicationID string `json:"application_id"`
		InstanceID    string `json:"instance_id"`
		AccountID     string `json:"account_id"`
		Active        *bool  `json:"active"`
		Revision      *int64 `json:"revision"`
	}
	if err := c.request(ctx, "POST", "/v1/identity/account-state", map[string]string{"account_id": account}, &out); err != nil {
		return AccountState{}, err
	}
	if !productApp.MatchString(out.ApplicationID) || !productInstance.MatchString(out.InstanceID) || out.InstanceID == "00000000-0000-0000-0000-000000000000" || out.AccountID != account || out.Active == nil || out.Revision == nil || *out.Revision < 0 || *out.Active && *out.Revision == 0 {
		return AccountState{}, ErrUnavailable
	}
	return AccountState{out.ApplicationID, out.InstanceID, out.AccountID, *out.Active, *out.Revision}, nil
}

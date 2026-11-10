package identity

import (
	"context"
	"encoding/json"
	"regexp"
	"strings"
	"time"
)

type Deletion struct {
	ID    string `json:"id"`
	State string `json:"state"`
}
type DeletionJob struct {
	ApplicationID string    `json:"application_id"`
	InstanceID    string    `json:"instance_id"`
	ID            string    `json:"id"`
	AccountID     string    `json:"account_id"`
	LeaseToken    string    `json:"lease_token"`
	LeaseUntil    time.Time `json:"lease_until"`
	Attempts      int       `json:"attempts"`
}

var productApp = regexp.MustCompile(`^[a-z][a-z0-9-]{2,47}$`)
var productInstance = regexp.MustCompile(`^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$`)

func deletionID(id string) bool {
	return strings.HasPrefix(id, "del_") && hexToken(strings.TrimPrefix(id, "del_"), 16)
}
func validDeletion(v Deletion) bool {
	return deletionID(v.ID) && (v.State == "products" || v.State == "provider" || v.State == "attention" || v.State == "completed")
}

// DeleteAllProducts must follow explicit user confirmation of the shared scope
// and a fresh sign-in. Persist requestID before calling; ambiguous responses may
// be retried only with that same ID, account and original shared session token.
// Persist a separate random receiptToken with requestID before sending. That
// immutable pair permits read-only recovery after all login grants are revoked.
// Completion still requires every product's durable cleanup acknowledgement.
func (c *Client) DeleteAllProducts(ctx context.Context, sessionToken, account, requestID, receiptToken string) (Deletion, error) {
	if !hexToken(sessionToken, 32) || !accountID(account) || !hexToken(requestID, 32) || !hexToken(receiptToken, 32) || requestID == receiptToken {
		return Deletion{}, ErrRejected
	}
	var out Deletion
	err := c.request(ctx, "POST", "/v1/identity/deletions", map[string]string{"scope": "all_products", "session_token": sessionToken, "account_id": account, "request_id": requestID, "receipt_token": receiptToken}, &out)
	if err != nil {
		return Deletion{}, err
	}
	if !validDeletion(out) {
		return Deletion{}, ErrUnavailable
	}
	return out, nil
}

type DeletionReceipt struct {
	ApplicationID string `json:"application_id"`
	InstanceID    string `json:"instance_id"`
	Found         bool   `json:"found"`
	ID            string `json:"id,omitempty"`
	State         string `json:"state,omitempty"`
}

func (c *Client) DeletionReceipt(ctx context.Context, requestID, receiptToken string) (DeletionReceipt, error) {
	if !hexToken(requestID, 32) || !hexToken(receiptToken, 32) || requestID == receiptToken {
		return DeletionReceipt{}, ErrRejected
	}
	var wire struct {
		ApplicationID string `json:"application_id"`
		InstanceID    string `json:"instance_id"`
		Found         *bool  `json:"found"`
		ID            string `json:"id"`
		State         string `json:"state"`
	}
	if err := c.request(ctx, "POST", "/v1/identity/deletions/receipt", map[string]string{"request_id": requestID, "receipt_token": receiptToken}, &wire); err != nil {
		return DeletionReceipt{}, err
	}
	if wire.Found == nil || !productApp.MatchString(wire.ApplicationID) || !productInstance.MatchString(wire.InstanceID) || wire.InstanceID == "00000000-0000-0000-0000-000000000000" || *wire.Found && !validDeletion(Deletion{ID: wire.ID, State: wire.State}) || !*wire.Found && (wire.ID != "" || wire.State != "") {
		return DeletionReceipt{}, ErrUnavailable
	}
	return DeletionReceipt{ApplicationID: wire.ApplicationID, InstanceID: wire.InstanceID, Found: *wire.Found, ID: wire.ID, State: wire.State}, nil
}
func (c *Client) Deletion(ctx context.Context, id string) (Deletion, error) {
	if !deletionID(id) {
		return Deletion{}, ErrRejected
	}
	var out Deletion
	if err := c.request(ctx, "GET", "/v1/identity/deletions/"+id, nil, &out); err != nil {
		return Deletion{}, err
	}
	if out.ID != id || !validDeletion(out) {
		return Deletion{}, ErrUnavailable
	}
	return out, nil
}

// ClaimDeletion returns one product-local account and a five-minute lease. The
// product worker must be idempotent by deletion ID and use bounded cleanup steps.
func (c *Client) ClaimDeletion(ctx context.Context) (*DeletionJob, error) {
	var raw struct {
		Job json.RawMessage `json:"job"`
	}
	if err := c.request(ctx, "POST", "/v1/identity/deletions/claim", nil, &raw); err != nil {
		return nil, err
	}
	if string(raw.Job) == "null" {
		return nil, nil
	}
	var job DeletionJob
	if json.Unmarshal(raw.Job, &job) != nil || !productApp.MatchString(job.ApplicationID) || !productInstance.MatchString(job.InstanceID) || job.InstanceID == "00000000-0000-0000-0000-000000000000" || !deletionID(job.ID) || !accountID(job.AccountID) || !hexToken(job.LeaseToken, 32) || !job.LeaseUntil.After(time.Now()) || job.LeaseUntil.After(time.Now().Add(6*time.Minute)) || job.Attempts < 1 || job.Attempts > 1000000 {
		return nil, ErrUnavailable
	}
	return &job, nil
}
func (c *Client) CompleteDeletion(ctx context.Context, job DeletionJob, receiptSHA256 string) error {
	if !hexToken(receiptSHA256, 32) {
		return ErrRejected
	}
	return c.finishDeletion(ctx, job, "complete", receiptSHA256, "")
}
func (c *Client) DeferDeletion(ctx context.Context, job DeletionJob, code string) error {
	if code != "STORAGE_UNAVAILABLE" && code != "SUBSCRIPTION_PENDING" && code != "CLEANUP_INCOMPLETE" {
		return ErrRejected
	}
	return c.finishDeletion(ctx, job, "defer", "", code)
}
func (c *Client) finishDeletion(ctx context.Context, job DeletionJob, action, receipt, code string) error {
	if !deletionID(job.ID) || !accountID(job.AccountID) || !hexToken(job.LeaseToken, 32) {
		return ErrRejected
	}
	var out Deletion
	if err := c.request(ctx, "POST", "/v1/identity/deletions/"+job.ID+"/"+action, map[string]string{"account_id": job.AccountID, "lease_token": job.LeaseToken, "receipt_sha256": receipt, "error_code": code}, &out); err != nil {
		return err
	}
	want := "pending"
	if action == "complete" {
		want = "completed"
	}
	if out.ID != job.ID || out.State != want {
		return ErrUnavailable
	}
	return nil
}

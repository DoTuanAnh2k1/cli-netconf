package api

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"time"
)

// --- Request / Response types (matching mgt-service OpenAPI spec) ---

type LoginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type TokenResponse struct {
	Status       string `json:"status"`
	ResponseData string `json:"response_data"`
	ResponseCode string `json:"response_code"`
	SystemType   string `json:"system_type"`
}

type NeListResponse struct {
	Status     string       `json:"status"`
	Code       string       `json:"code"`
	Message    string       `json:"message"`
	NeDataList []NeDataItem `json:"neDataList"`
}

type NeDataItem struct {
	Site        string `json:"site"`
	Ne          string `json:"ne"`
	IP          string `json:"ip"`
	Description string `json:"description"`
	Namespace   string `json:"namespace"`
	Port        int    `json:"port"`
}

type HistorySaveRequest struct {
	CmdName        string `json:"cmd_name"`
	NeName         string `json:"ne_name"`
	NeIP           string `json:"ne_ip,omitempty"`
	NeID           int    `json:"ne_id,omitempty"`
	Scope          string `json:"scope,omitempty"`
	Result         string `json:"result,omitempty"`
	InputType      string `json:"input_type,omitempty"`
	Session        string `json:"session,omitempty"`
	BatchID        string `json:"batch_id,omitempty"`
	TimeToComplete int64  `json:"time_to_complete,omitempty"`
}

type ChangePasswordRequest struct {
	Username    string `json:"username"`
	OldPassword string `json:"old_password"`
	NewPassword string `json:"new_password"`
}

// --- Config backup ---

// BackupSaveRequest is sent to POST /aa/config-backup/save.
type BackupSaveRequest struct {
	NeName    string `json:"ne_name"`
	NeIP      string `json:"ne_ip"`
	ConfigXML string `json:"config_xml"` // raw XML content extracted from get-config reply
}

// BackupSaveResponse is the response from POST /aa/config-backup/save.
type BackupSaveResponse struct {
	Status string `json:"status"`
	ID     int    `json:"id"` // server-assigned backup ID
}

// BackupItem is one entry in the list returned by GET /aa/config-backup/list.
type BackupItem struct {
	ID        int    `json:"id"`
	NeName    string `json:"ne_name"`
	NeIP      string `json:"ne_ip"`
	CreatedAt string `json:"created_at"` // RFC3339
	Size      int    `json:"size"`        // byte length of config_xml
}

// BackupListResponse is the response from GET /aa/config-backup/list.
type BackupListResponse struct {
	Status  string       `json:"status"`
	Backups []BackupItem `json:"backups"`
}

// BackupDetailResponse is the response from GET /aa/config-backup/{id}.
// Includes the full config XML so the client can perform a restore.
type BackupDetailResponse struct {
	Status    string `json:"status"`
	ID        int    `json:"id"`
	NeName    string `json:"ne_name"`
	NeIP      string `json:"ne_ip"`
	CreatedAt string `json:"created_at"` // RFC3339
	ConfigXML string `json:"config_xml"`
}

// --- Client ---

type Client struct {
	baseURL string
	http    *http.Client
}

func NewClient(baseURL string) *Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	return &Client{
		baseURL: baseURL,
		http: &http.Client{
			Timeout:   15 * time.Second,
			Transport: transport,
			// mgt-service uses HTTP 302 as a success code (not a redirect).
			// Go's http.Client errors on 302 without Location header.
			// Return the response as-is instead of following redirects.
			CheckRedirect: func(req *http.Request, via []*http.Request) error {
				return http.ErrUseLastResponse
			},
		},
	}
}

// tokenPreview returns the first 20 chars of a token followed by "..." for safe logging.
func tokenPreview(token string) string {
	if len(token) <= 20 {
		return token
	}
	return token[:20] + "..."
}

func (c *Client) Authenticate(username, password string) (*TokenResponse, error) {
	url := c.baseURL + "/aa/authenticate"
	slog.Info("mgt-svc request", "method", "POST", "url", url, "user", username)

	body, err := json.Marshal(LoginRequest{Username: username, Password: password})
	if err != nil {
		return nil, err
	}

	resp, err := c.http.Post(url, "application/json", bytes.NewReader(body))
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return nil, fmt.Errorf("connect to mgt-service: %w", err)
	}
	defer resp.Body.Close()

	slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)

	if resp.StatusCode != http.StatusOK {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc auth error", "status", resp.StatusCode, "body", string(msg))
		return nil, fmt.Errorf("authentication failed (%d): %s", resp.StatusCode, string(msg))
	}

	var result TokenResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		slog.Error("mgt-svc decode error", "url", url, "error", err)
		return nil, fmt.Errorf("decode auth response: %w", err)
	}
	preview := tokenPreview(result.ResponseData)
	slog.Info("mgt-svc auth ok",
		"status_field", result.Status,
		"response_code", result.ResponseCode,
		"system_type", result.SystemType,
		"token_preview", preview,
		"token_len", len(result.ResponseData),
	)
	return &result, nil
}

func (c *Client) ListNE(token string) (*NeListResponse, error) {
	url := c.baseURL + "/aa/list/ne"
	slog.Info("mgt-svc request", "method", "GET", "url", url, "token_preview", tokenPreview(token))

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Authorization", token)

	resp, err := c.http.Do(req)
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return nil, fmt.Errorf("list NE request: %w", err)
	}
	defer resp.Body.Close()

	slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)

	// API spec uses 302 for successful list responses
	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusFound {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc list NE error", "status", resp.StatusCode, "body", string(msg))
		return nil, fmt.Errorf("list NE failed (%d): %s", resp.StatusCode, string(msg))
	}

	var result NeListResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		slog.Error("mgt-svc decode error", "url", url, "error", err)
		return nil, fmt.Errorf("decode NE list: %w", err)
	}
	slog.Info("mgt-svc list NE ok",
		"status_field", result.Status,
		"code", result.Code,
		"message", result.Message,
		"ne_count", len(result.NeDataList),
	)
	return &result, nil
}

func (c *Client) SaveHistory(token string, req *HistorySaveRequest) error {
	url := c.baseURL + "/aa/history/save"
	slog.Info("mgt-svc request", "method", "POST", "url", url,
		"cmd_name", req.CmdName, "ne_name", req.NeName, "result", req.Result,
		"time_to_complete_ms", req.TimeToComplete, "token_preview", tokenPreview(token),
	)

	body, err := json.Marshal(req)
	if err != nil {
		return err
	}

	httpReq, err := http.NewRequest("POST", url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	httpReq.Header.Set("Authorization", token)
	httpReq.Header.Set("Content-Type", "application/json")

	resp, err := c.http.Do(httpReq)
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode >= 400 {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc save history error", "status", resp.StatusCode, "body", string(msg))
	} else {
		slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)
	}
	return nil
}

// SaveBackup persists a config snapshot to the mgt-service.
// Returns the server-assigned backup ID on success.
func (c *Client) SaveBackup(token string, req *BackupSaveRequest) (*BackupSaveResponse, error) {
	url := c.baseURL + "/aa/config-backup/save"
	slog.Info("mgt-svc request", "method", "POST", "url", url,
		"ne_name", req.NeName, "ne_ip", req.NeIP, "xml_len", len(req.ConfigXML),
		"token_preview", tokenPreview(token),
	)

	body, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}

	httpReq, err := http.NewRequest("POST", url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Authorization", token)
	httpReq.Header.Set("Content-Type", "application/json")

	resp, err := c.http.Do(httpReq)
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return nil, fmt.Errorf("save backup: %w", err)
	}
	defer resp.Body.Close()

	slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)

	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusCreated {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc save backup error", "status", resp.StatusCode, "body", string(msg))
		return nil, fmt.Errorf("save backup failed (%d): %s", resp.StatusCode, string(msg))
	}

	var result BackupSaveResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		slog.Error("mgt-svc decode error", "url", url, "error", err)
		return nil, fmt.Errorf("decode save backup response: %w", err)
	}
	slog.Info("mgt-svc save backup ok", "assigned_id", result.ID, "status_field", result.Status)
	return &result, nil
}

// ListBackups fetches the list of saved snapshots for a given NE name.
func (c *Client) ListBackups(token, neName string) (*BackupListResponse, error) {
	url := c.baseURL + "/aa/config-backup/list?ne_name=" + neName
	slog.Info("mgt-svc request", "method", "GET", "url", url, "token_preview", tokenPreview(token))

	httpReq, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Authorization", token)

	resp, err := c.http.Do(httpReq)
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return nil, fmt.Errorf("list backups: %w", err)
	}
	defer resp.Body.Close()

	slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)

	if resp.StatusCode != http.StatusOK && resp.StatusCode != http.StatusFound {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc list backups error", "status", resp.StatusCode, "body", string(msg))
		return nil, fmt.Errorf("list backups failed (%d): %s", resp.StatusCode, string(msg))
	}

	var result BackupListResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		slog.Error("mgt-svc decode error", "url", url, "error", err)
		return nil, fmt.Errorf("decode backup list: %w", err)
	}
	slog.Info("mgt-svc list backups ok",
		"ne_name", neName,
		"backup_count", len(result.Backups),
		"status_field", result.Status,
	)
	return &result, nil
}

// GetBackup fetches the full detail (including config XML) of a backup by its server ID.
func (c *Client) GetBackup(token string, id int) (*BackupDetailResponse, error) {
	url := fmt.Sprintf("%s/aa/config-backup/%d", c.baseURL, id)
	slog.Info("mgt-svc request", "method", "GET", "url", url, "token_preview", tokenPreview(token))

	httpReq, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return nil, err
	}
	httpReq.Header.Set("Authorization", token)

	resp, err := c.http.Do(httpReq)
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return nil, fmt.Errorf("get backup: %w", err)
	}
	defer resp.Body.Close()

	slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)

	if resp.StatusCode != http.StatusOK {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc get backup error", "status", resp.StatusCode, "body", string(msg))
		return nil, fmt.Errorf("get backup failed (%d): %s", resp.StatusCode, string(msg))
	}

	var result BackupDetailResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		slog.Error("mgt-svc decode error", "url", url, "error", err)
		return nil, fmt.Errorf("decode backup detail: %w", err)
	}
	slog.Info("mgt-svc get backup ok", "id", result.ID, "ne_name", result.NeName, "xml_len", len(result.ConfigXML))
	return &result, nil
}

func (c *Client) ChangePassword(token string, req *ChangePasswordRequest) error {
	url := c.baseURL + "/aa/change-password"
	slog.Info("mgt-svc request", "method", "POST", "url", url,
		"username", req.Username, "token_preview", tokenPreview(token),
	)

	body, err := json.Marshal(req)
	if err != nil {
		return err
	}

	httpReq, err := http.NewRequest("POST", url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	httpReq.Header.Set("Authorization", token)
	httpReq.Header.Set("Content-Type", "application/json")

	resp, err := c.http.Do(httpReq)
	if err != nil {
		slog.Error("mgt-svc request failed", "url", url, "error", err)
		return err
	}
	defer resp.Body.Close()

	slog.Info("mgt-svc response", "url", url, "status", resp.StatusCode)

	if resp.StatusCode != http.StatusOK {
		msg, _ := io.ReadAll(resp.Body)
		slog.Warn("mgt-svc change password error", "status", resp.StatusCode, "body", string(msg))
		return fmt.Errorf("change password failed (%d): %s", resp.StatusCode, string(msg))
	}
	return nil
}

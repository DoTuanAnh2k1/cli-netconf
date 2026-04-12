package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// Types matching mgt-service OpenAPI spec
// ---------------------------------------------------------------------------

type loginRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type tokenResponse struct {
	Status       string `json:"status"`
	ResponseData string `json:"response_data"`
	ResponseCode string `json:"response_code"`
	SystemType   string `json:"system_type"`
}

type neListResponse struct {
	Status     string       `json:"status"`
	Code       string       `json:"code"`
	Message    string       `json:"message"`
	NeDataList []neDataItem `json:"neDataList"`
}

type neDataItem struct {
	Site        string `json:"site"`
	Ne          string `json:"ne"`
	IP          string `json:"ip"`
	Description string `json:"description"`
	Namespace   string `json:"namespace"`
	Port        int    `json:"port"`
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

var (
	users = map[string]string{
		"admin":    "admin",
		"operator": "operator123",
	}
	netconfPort = 8830
)

func init() {
	if p := os.Getenv("NETCONF_PORT"); p != "" {
		fmt.Sscanf(p, "%d", &netconfPort)
	}
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

func handleHealth(w http.ResponseWriter, r *http.Request) {
	json.NewEncoder(w).Encode(map[string]string{"message": "OK"})
}

func handleAuthenticate(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req loginRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"message":"invalid request body"}`, http.StatusBadRequest)
		return
	}

	expected, ok := users[req.Username]
	if !ok || expected != req.Password {
		w.WriteHeader(http.StatusUnauthorized)
		json.NewEncoder(w).Encode(map[string]string{"message": "invalid credentials"})
		return
	}

	// Return a fake JWT token (base64 of username + timestamp)
	token := fmt.Sprintf("mock-jwt-%s-%d", req.Username, time.Now().Unix())

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(tokenResponse{
		Status:       "success",
		ResponseData: token,
		ResponseCode: "200",
		SystemType:   "5GC",
	})
	log.Printf("auth: user=%s -> token issued", req.Username)
}

func handleListNE(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	auth := r.Header.Get("Authorization")
	if auth == "" || !strings.HasPrefix(auth, "Bearer mock-jwt-") {
		w.WriteHeader(http.StatusUnauthorized)
		json.NewEncoder(w).Encode(map[string]string{"message": "unauthorized"})
		return
	}

	neList := []neDataItem{
		{
			Site:        "HCM",
			Ne:          "ne-amf-01",
			IP:          "127.0.0.1",
			Description: "AMF Node - Ho Chi Minh",
			Namespace:   "5gc-hcm",
			Port:        netconfPort,
		},
		{
			Site:        "HCM",
			Ne:          "ne-smf-01",
			IP:          "127.0.0.1",
			Description: "SMF Node - Ho Chi Minh",
			Namespace:   "5gc-hcm",
			Port:        netconfPort,
		},
		{
			Site:        "HNI",
			Ne:          "ne-upf-01",
			IP:          "127.0.0.1",
			Description: "UPF Node - Ha Noi",
			Namespace:   "5gc-hni",
			Port:        netconfPort,
		},
	}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusFound) // API spec uses 302
	json.NewEncoder(w).Encode(neListResponse{
		Status:     "success",
		Code:       "302",
		Message:    "NE list retrieved",
		NeDataList: neList,
	})
	log.Printf("list-ne: returned %d NEs", len(neList))
}

func handleHistorySave(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	auth := r.Header.Get("Authorization")
	if auth == "" || !strings.HasPrefix(auth, "Bearer mock-jwt-") {
		w.WriteHeader(http.StatusUnauthorized)
		return
	}

	var body map[string]any
	json.NewDecoder(r.Body).Decode(&body)
	log.Printf("history: cmd=%v ne=%v", body["cmd_name"], body["ne_name"])

	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(map[string]string{"message": "saved"})
}

func handleChangePassword(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var req struct {
		Username    string `json:"username"`
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, `{"message":"invalid body"}`, http.StatusBadRequest)
		return
	}

	current, ok := users[req.Username]
	if !ok || current != req.OldPassword {
		w.WriteHeader(http.StatusForbidden)
		json.NewEncoder(w).Encode(map[string]string{"message": "wrong old password"})
		return
	}

	users[req.Username] = req.NewPassword
	log.Printf("password changed: user=%s", req.Username)
	json.NewEncoder(w).Encode(map[string]string{"message": "password changed"})
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	port := "3000"
	if p := os.Getenv("MGT_PORT"); p != "" {
		port = p
	}

	http.HandleFunc("/health", handleHealth)
	http.HandleFunc("/aa/authenticate", handleAuthenticate)
	http.HandleFunc("/aa/list/ne", handleListNE)
	http.HandleFunc("/aa/history/save", handleHistorySave)
	http.HandleFunc("/aa/change-password", handleChangePassword)

	log.Printf("Mock mgt-service listening on :%s", port)
	log.Printf("  Users: admin/admin, operator/operator123")
	log.Printf("  NE NETCONF port: %d", netconfPort)
	log.Fatal(http.ListenAndServe(":"+port, nil))
}

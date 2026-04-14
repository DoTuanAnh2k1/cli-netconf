package ccli_test

import (
	"fmt"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/ccli"
)

// ExampleHandle_GetConfig minh hoạ cách dùng ccli package để kết nối NE
// và lấy config. Ví dụ này không chạy được trong test suite tự động
// (cần mock servers) — chỉ dùng làm tài liệu.
func ExampleHandle_GetConfig() {
	h := ccli.New("http://127.0.0.1:3000")
	if h == nil {
		fmt.Println("out of memory")
		return
	}
	defer h.Close()

	if err := h.Login("admin", "admin"); err != nil {
		fmt.Println("login:", err)
		return
	}

	if err := h.ListNE(); err != nil {
		fmt.Println("list NE:", err)
		return
	}

	fmt.Printf("Found %d NEs\n", h.NECount())
	for _, ne := range h.NEs() {
		fmt.Printf("  %s  %s:%d\n", ne.Name, ne.IP, ne.Port)
	}

	opts := ccli.ConnectOpts{Mode: "tcp", Host: "127.0.0.1", Port: 2023}
	if err := h.ConnectNE("ne-smf-01", opts); err != nil {
		fmt.Println("connect:", err)
		return
	}
	defer h.Disconnect()

	fmt.Println("session:", h.SessionID())

	h.LoadSchema()

	cfg, err := h.GetConfig("running", "")
	if err != nil {
		fmt.Println("get-config:", err)
		return
	}
	fmt.Printf("config length: %d bytes\n", len(cfg))

	// Edit + commit workflow
	xml := `<system xmlns="urn:example:system"><hostname>new-host</hostname></system>`
	if err := h.EditConfig("candidate", xml); err != nil {
		fmt.Println("edit-config:", err)
		return
	}
	if err := h.Commit(); err != nil {
		fmt.Println("commit:", err)
		return
	}
	fmt.Println("committed")
}

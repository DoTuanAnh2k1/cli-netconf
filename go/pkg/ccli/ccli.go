// Package ccli wraps the C libclinetconf library via CGo.
//
// Build requirements:
//   - libclinetconf.a (or .so) built from c/ directory:
//       make -C ../../c lib
//
// Usage:
//
//	h := ccli.New("http://mgt-service:3000")
//	defer h.Close()
//	if err := h.Login("admin", "admin"); err != nil { ... }
//	if err := h.ListNE(); err != nil { ... }
//	opts := ccli.ConnectOpts{Mode: "tcp", Host: "127.0.0.1", Port: 2023}
//	if err := h.ConnectNE("ne-smf-01", opts); err != nil { ... }
//	cfg, err := h.GetConfig("running", "")
//	h.Commit()

package ccli

/*
#cgo CFLAGS: -I${SRCDIR}/../../../c/include
#cgo LDFLAGS: -L${SRCDIR}/../../../c -lclinetconf -lcurl -lxml2

#include "libclinetconf.h"
#include <stdlib.h>
*/
import "C"
import (
	"errors"
	"fmt"
	"unsafe"
)

// Handle là wrapper Go cho cli_handle_t*.
type Handle struct {
	h *C.cli_handle_t
}

// ConnectOpts cấu hình kết nối NETCONF.
type ConnectOpts struct {
	// Mode: "tcp" (default) hoặc "ssh"
	Mode string
	// Host override; "" = dùng IP từ NE list
	Host string
	// Port override; 0 = dùng port từ NE list
	Port int
	// NCUser và NCPass chỉ dùng khi Mode == "ssh"
	NCUser string
	NCPass string
}

// NEInfo thông tin một Network Element.
type NEInfo struct {
	Name        string
	IP          string
	Port        int
	Site        string
	Description string
}

// New tạo handle mới với mgt_url ("http://host:port").
// Trả về nil nếu cấp phát thất bại (hiếm).
func New(mgtURL string) *Handle {
	cs := C.CString(mgtURL)
	defer C.free(unsafe.Pointer(cs))
	h := C.cli_create(cs)
	if h == nil {
		return nil
	}
	return &Handle{h: h}
}

// Close giải phóng toàn bộ tài nguyên (NETCONF session, schema, backups).
// Phải gọi khi không còn dùng nữa (thường qua defer).
func (h *Handle) Close() {
	if h == nil || h.h == nil {
		return
	}
	C.cli_destroy(h.h)
	h.h = nil
}

// ─────────────────────────────────────────────
// Auth & NE list
// ─────────────────────────────────────────────

// Login xác thực với mgt-service.
func (h *Handle) Login(username, password string) error {
	cu := C.CString(username)
	cp := C.CString(password)
	defer C.free(unsafe.Pointer(cu))
	defer C.free(unsafe.Pointer(cp))
	if C.cli_login(h.h, cu, cp) != 0 {
		return errors.New("authentication failed")
	}
	return nil
}

// ListNE tải danh sách Network Elements từ mgt-service.
// Phải gọi sau Login.
func (h *Handle) ListNE() error {
	if C.cli_list_ne(h.h) != 0 {
		return errors.New("failed to list NEs")
	}
	return nil
}

// NECount trả về số lượng NE đã tải.
func (h *Handle) NECount() int {
	return int(C.cli_ne_count(h.h))
}

// NE trả về thông tin NE tại index (0-based); nil nếu out-of-range.
func (h *Handle) NE(idx int) *NEInfo {
	n := int(C.cli_ne_count(h.h))
	if idx < 0 || idx >= n {
		return nil
	}
	i := C.int(idx)
	return &NEInfo{
		Name:        C.GoString(C.cli_ne_name(h.h, i)),
		IP:          C.GoString(C.cli_ne_ip(h.h, i)),
		Port:        int(C.cli_ne_port(h.h, i)),
		Site:        C.GoString(C.cli_ne_site(h.h, i)),
		Description: C.GoString(C.cli_ne_description(h.h, i)),
	}
}

// NEs trả về tất cả NE dưới dạng slice.
func (h *Handle) NEs() []NEInfo {
	n := h.NECount()
	result := make([]NEInfo, n)
	for i := range result {
		if info := h.NE(i); info != nil {
			result[i] = *info
		}
	}
	return result
}

// ─────────────────────────────────────────────
// Connection
// ─────────────────────────────────────────────

// ConnectNE kết nối tới NE.
// ne: tên NE (case-insensitive) hoặc số thứ tự ("1", "2", ...).
func (h *Handle) ConnectNE(ne string, opts ConnectOpts) error {
	cne := C.CString(ne)
	defer C.free(unsafe.Pointer(cne))

	mode := opts.Mode
	if mode == "" {
		mode = "tcp"
	}
	cmode := C.CString(mode)
	defer C.free(unsafe.Pointer(cmode))

	var chost *C.char
	if opts.Host != "" {
		chost = C.CString(opts.Host)
		defer C.free(unsafe.Pointer(chost))
	}

	ncUser := opts.NCUser
	if ncUser == "" {
		ncUser = "admin"
	}
	ncPass := opts.NCPass
	if ncPass == "" {
		ncPass = "admin"
	}
	cuser := C.CString(ncUser)
	cpass := C.CString(ncPass)
	defer C.free(unsafe.Pointer(cuser))
	defer C.free(unsafe.Pointer(cpass))

	if C.cli_connect(h.h, cne, cmode, chost, C.int(opts.Port), cuser, cpass) != 0 {
		return fmt.Errorf("connect to NE '%s' failed", ne)
	}
	return nil
}

// Disconnect ngắt kết nối NETCONF hiện tại.
func (h *Handle) Disconnect() {
	C.cli_disconnect(h.h)
}

// IsConnected trả về true nếu đang có kết nối NETCONF.
func (h *Handle) IsConnected() bool {
	return C.cli_is_connected(h.h) != 0
}

// CurrentNE trả về tên NE đang kết nối; "" nếu chưa kết nối.
func (h *Handle) CurrentNE() string {
	s := C.cli_current_ne(h.h)
	if s == nil {
		return ""
	}
	return C.GoString(s)
}

// SessionID trả về NETCONF session-id của kết nối hiện tại.
func (h *Handle) SessionID() string {
	s := C.cli_session_id(h.h)
	if s == nil {
		return ""
	}
	return C.GoString(s)
}

// ─────────────────────────────────────────────
// Schema
// ─────────────────────────────────────────────

// LoadSchema tải YANG schema từ NE (get-schema hoặc XML fallback).
// Phải gọi sau ConnectNE nếu muốn dùng SchemaChildren.
func (h *Handle) LoadSchema() {
	C.cli_load_schema(h.h)
}

// SchemaChildren trả về tên các child node của node tại path.
// path = nil hoặc len=0 → children của root.
func (h *Handle) SchemaChildren(path []string) []string {
	if len(path) == 0 {
		var count C.int
		raw := C.cli_schema_children(h.h, nil, 0, &count)
		return goStringSliceAndFree(raw, int(count))
	}

	// Chuyển []string → **char
	cstrs := make([]*C.char, len(path))
	for i, p := range path {
		cstrs[i] = C.CString(p)
	}
	defer func() {
		for _, cs := range cstrs {
			C.free(unsafe.Pointer(cs))
		}
	}()

	var count C.int
	raw := C.cli_schema_children(h.h,
		(**C.char)(unsafe.Pointer(&cstrs[0])),
		C.int(len(path)),
		&count)
	return goStringSliceAndFree(raw, int(count))
}

// goStringSliceAndFree chuyển **char (C malloc'd) thành []string rồi free.
func goStringSliceAndFree(raw **C.char, count int) []string {
	if raw == nil || count == 0 {
		return nil
	}
	result := make([]string, count)
	// Interpret raw as a Go slice of C pointers
	ptrs := unsafe.Slice(raw, count)
	for i, p := range ptrs {
		result[i] = C.GoString(p)
		C.free(unsafe.Pointer(p))
	}
	C.free(unsafe.Pointer(raw))
	return result
}

// ─────────────────────────────────────────────
// NETCONF operations
// ─────────────────────────────────────────────

// GetConfig gửi get-config và trả về XML reply.
// datastore: "running" | "candidate".
// filter: subtree XML filter; "" = lấy toàn bộ.
func (h *Handle) GetConfig(datastore, filter string) (string, error) {
	cds := C.CString(datastore)
	defer C.free(unsafe.Pointer(cds))

	var cf *C.char
	if filter != "" {
		cf = C.CString(filter)
		defer C.free(unsafe.Pointer(cf))
	}

	raw := C.cli_get_config(h.h, cds, cf)
	if raw == nil {
		return "", errors.New("get-config failed")
	}
	defer C.free(unsafe.Pointer(raw))
	return C.GoString(raw), nil
}

// GetConfigText trả về config dạng text đã indent.
// path = nil → toàn bộ config.
func (h *Handle) GetConfigText(datastore string, path []string) (string, error) {
	cds := C.CString(datastore)
	defer C.free(unsafe.Pointer(cds))

	var rawPath **C.char
	if len(path) > 0 {
		cstrs := make([]*C.char, len(path))
		for i, p := range path {
			cstrs[i] = C.CString(p)
		}
		defer func() {
			for _, cs := range cstrs {
				C.free(unsafe.Pointer(cs))
			}
		}()
		rawPath = (**C.char)(unsafe.Pointer(&cstrs[0]))
	}

	raw := C.cli_get_config_text(h.h, cds, rawPath, C.int(len(path)))
	if raw == nil {
		return "", errors.New("get-config-text failed")
	}
	defer C.free(unsafe.Pointer(raw))
	return C.GoString(raw), nil
}

// EditConfig gửi edit-config với XML config.
func (h *Handle) EditConfig(datastore, xml string) error {
	cds := C.CString(datastore)
	cx := C.CString(xml)
	defer C.free(unsafe.Pointer(cds))
	defer C.free(unsafe.Pointer(cx))
	if C.cli_edit_config(h.h, cds, cx) != 0 {
		return errors.New("edit-config failed")
	}
	return nil
}

// Commit áp dụng candidate vào running.
func (h *Handle) Commit() error {
	if C.cli_commit(h.h) != 0 {
		return errors.New("commit failed")
	}
	return nil
}

// Validate kiểm tra candidate.
func (h *Handle) Validate() error {
	if C.cli_validate(h.h) != 0 {
		return errors.New("validate failed")
	}
	return nil
}

// Discard huỷ bỏ thay đổi candidate.
func (h *Handle) Discard() error {
	if C.cli_discard(h.h) != 0 {
		return errors.New("discard failed")
	}
	return nil
}

// Lock khoá datastore ("running" | "candidate").
func (h *Handle) Lock(datastore string) error {
	cds := C.CString(datastore)
	defer C.free(unsafe.Pointer(cds))
	if C.cli_lock(h.h, cds) != 0 {
		return fmt.Errorf("lock %s failed", datastore)
	}
	return nil
}

// Unlock mở khoá datastore.
func (h *Handle) Unlock(datastore string) error {
	cds := C.CString(datastore)
	defer C.free(unsafe.Pointer(cds))
	if C.cli_unlock(h.h, cds) != 0 {
		return fmt.Errorf("unlock %s failed", datastore)
	}
	return nil
}

// CopyConfig gửi copy-config.
func (h *Handle) CopyConfig(target, sourceXML string) error {
	ct := C.CString(target)
	cx := C.CString(sourceXML)
	defer C.free(unsafe.Pointer(ct))
	defer C.free(unsafe.Pointer(cx))
	if C.cli_copy_config(h.h, ct, cx) != 0 {
		return errors.New("copy-config failed")
	}
	return nil
}

// SendRPC gửi RPC body tự do và trả về XML reply.
func (h *Handle) SendRPC(body string) (string, error) {
	cb := C.CString(body)
	defer C.free(unsafe.Pointer(cb))
	raw := C.cli_send_rpc(h.h, cb)
	if raw == nil {
		return "", errors.New("send-rpc failed")
	}
	defer C.free(unsafe.Pointer(raw))
	return C.GoString(raw), nil
}

// ─────────────────────────────────────────────
// Reply helpers (static — không cần handle)
// ─────────────────────────────────────────────

// ReplyIsOK trả về true nếu XML reply chứa <ok/>.
func ReplyIsOK(reply string) bool {
	cr := C.CString(reply)
	defer C.free(unsafe.Pointer(cr))
	return C.cli_reply_is_ok(cr) != 0
}

// ReplyIsError trả về true nếu reply chứa <rpc-error>.
func ReplyIsError(reply string) bool {
	cr := C.CString(reply)
	defer C.free(unsafe.Pointer(cr))
	return C.cli_reply_is_error(cr) != 0
}

// ReplyErrorMsg trích xuất error-message từ reply; "" nếu không có.
func ReplyErrorMsg(reply string) string {
	cr := C.CString(reply)
	defer C.free(unsafe.Pointer(cr))
	raw := C.cli_reply_error_msg(cr)
	if raw == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(raw))
	return C.GoString(raw)
}

// ReplyDataXML trích xuất <data>...</data> từ reply.
func ReplyDataXML(reply string) string {
	cr := C.CString(reply)
	defer C.free(unsafe.Pointer(cr))
	raw := C.cli_reply_data_xml(cr)
	if raw == nil {
		return ""
	}
	defer C.free(unsafe.Pointer(raw))
	return C.GoString(raw)
}

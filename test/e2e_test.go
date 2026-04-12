package test

import (
	"bytes"
	"fmt"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
)

const (
	sshAddr  = "127.0.0.1:2222"
	sshUser  = "admin"
	sshPass  = "admin"
	cmdDelay = 200 * time.Millisecond
)

// asyncReader continuously reads from an io.Reader into a buffer
type asyncReader struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func newAsyncReader(r io.Reader) *asyncReader {
	ar := &asyncReader{}
	go func() {
		tmp := make([]byte, 4096)
		for {
			n, err := r.Read(tmp)
			if n > 0 {
				ar.mu.Lock()
				ar.buf.Write(tmp[:n])
				ar.mu.Unlock()
			}
			if err != nil {
				return
			}
		}
	}()
	return ar
}

// collect waits for data to settle (stable for 500ms), then returns and clears the buffer
func (ar *asyncReader) collect(timeout time.Duration) string {
	deadline := time.After(timeout)
	stableCount := 0
	lastLen := 0

	for {
		select {
		case <-deadline:
			ar.mu.Lock()
			s := ar.buf.String()
			ar.buf.Reset()
			ar.mu.Unlock()
			return s
		case <-time.After(150 * time.Millisecond):
			ar.mu.Lock()
			curLen := ar.buf.Len()
			ar.mu.Unlock()

			if curLen > 0 && curLen == lastLen {
				stableCount++
				if stableCount >= 3 {
					ar.mu.Lock()
					s := ar.buf.String()
					ar.buf.Reset()
					ar.mu.Unlock()
					return s
				}
			} else {
				stableCount = 0
			}
			lastLen = curLen
		}
	}
}

type testSession struct {
	client  *ssh.Client
	session *ssh.Session
	stdin   io.WriteCloser
	reader  *asyncReader
	t       *testing.T
}

// newTestSession creates an SSH session, responds to the auto NE selection
// prompt by selecting NE 1, and returns a ready-to-use session.
func newTestSession(t *testing.T) *testSession {
	t.Helper()
	cfg := &ssh.ClientConfig{
		User:            sshUser,
		Auth:            []ssh.AuthMethod{ssh.Password(sshPass)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         5 * time.Second,
	}
	client, err := ssh.Dial("tcp", sshAddr, cfg)
	if err != nil {
		t.Fatalf("ssh dial: %v", err)
	}

	session, err := client.NewSession()
	if err != nil {
		client.Close()
		t.Fatalf("ssh session: %v", err)
	}

	modes := ssh.TerminalModes{ssh.ECHO: 0}
	if err := session.RequestPty("xterm", 80, 200, modes); err != nil {
		session.Close()
		client.Close()
		t.Fatalf("request pty: %v", err)
	}

	stdin, _ := session.StdinPipe()
	stdout, _ := session.StdoutPipe()

	if err := session.Shell(); err != nil {
		t.Fatalf("shell: %v", err)
	}

	ts := &testSession{
		client:  client,
		session: session,
		stdin:   stdin,
		reader:  newAsyncReader(stdout),
		t:       t,
	}

	// Read welcome + NE list + "Select NE" prompt
	ts.reader.collect(2 * time.Second)

	// Select NE 1 to connect
	fmt.Fprintf(ts.stdin, "1\r")
	time.Sleep(2 * time.Second)
	output := ts.reader.collect(3 * time.Second)
	if !strings.Contains(stripANSI(output), "Connected") {
		t.Fatalf("auto-connect failed: %s", stripANSI(output))
	}

	return ts
}

func (ts *testSession) send(cmd string) {
	fmt.Fprintf(ts.stdin, "%s\r", cmd)
	time.Sleep(cmdDelay)
}

func (ts *testSession) read() string {
	return ts.reader.collect(5 * time.Second)
}

func (ts *testSession) close() {
	ts.send("disconnect")
	ts.reader.collect(time.Second)
	ts.send("exit")
	ts.session.Close()
	ts.client.Close()
}

// stripANSI removes ANSI escape sequences
func stripANSI(s string) string {
	var result strings.Builder
	i := 0
	for i < len(s) {
		if s[i] == '\033' {
			for i < len(s) && !isTerminator(s[i]) {
				i++
			}
			if i < len(s) {
				i++
			}
		} else {
			result.WriteByte(s[i])
			i++
		}
	}
	return result.String()
}

func isTerminator(b byte) bool {
	return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~'
}

// --- Tests ---

func TestWelcomeBanner(t *testing.T) {
	cfg := &ssh.ClientConfig{
		User:            sshUser,
		Auth:            []ssh.AuthMethod{ssh.Password(sshPass)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         5 * time.Second,
	}
	client, err := ssh.Dial("tcp", sshAddr, cfg)
	if err != nil {
		t.Fatalf("ssh dial: %v", err)
	}
	defer client.Close()

	session, _ := client.NewSession()
	defer session.Close()

	modes := ssh.TerminalModes{ssh.ECHO: 0}
	session.RequestPty("xterm", 80, 200, modes)
	stdin, _ := session.StdinPipe()
	stdout, _ := session.StdoutPipe()
	session.Shell()

	reader := newAsyncReader(stdout)
	output := reader.collect(3 * time.Second)
	plain := stripANSI(output)

	if !strings.Contains(plain, "VHT CLI - NETCONF Console") {
		t.Errorf("missing welcome banner, got:\n%s", plain)
	}
	// Should auto-show NE list and selection prompt
	if !strings.Contains(plain, "ne-amf-01") {
		t.Errorf("missing NE list in welcome, got:\n%s", plain)
	}
	if !strings.Contains(plain, "Select NE") {
		t.Errorf("missing NE selection prompt, got:\n%s", plain)
	}

	fmt.Fprintf(stdin, "1\r")
	time.Sleep(time.Second)
	fmt.Fprintf(stdin, "exit\r")
}

func TestShowNE(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("show ne")
	output := ts.read()
	plain := stripANSI(output)

	for _, ne := range []string{"ne-amf-01", "ne-smf-01", "ne-upf-01"} {
		if !strings.Contains(plain, ne) {
			t.Errorf("missing %s in NE list", ne)
		}
	}
	if !strings.Contains(plain, "HCM") {
		t.Errorf("missing site HCM")
	}
}

func TestShowRunningConfig(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("show running-config")
	output := ts.read()
	plain := stripANSI(output)

	if !strings.Contains(plain, "hostname") {
		t.Errorf("missing hostname in config")
	}
	if !strings.Contains(plain, "ne-amf-01") {
		t.Errorf("missing ne-amf-01 value")
	}
	if !strings.Contains(plain, "eth0") {
		t.Errorf("missing eth0 interface")
	}
}

func TestShowRunningConfigFiltered(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("show running-config system contact")
	output := ts.read()
	plain := stripANSI(output)

	if !strings.Contains(plain, "contact") {
		t.Errorf("missing contact in filtered output")
	}
	if !strings.Contains(plain, "noc@vht.com.vn") {
		t.Errorf("missing contact value")
	}
}

func TestSetAndCommit(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	// Set via multiline XML
	ts.send("set")
	time.Sleep(200 * time.Millisecond)
	ts.send(`<system xmlns="urn:vht:params:xml:ns:yang:vht-system">`)
	ts.send(`  <hostname>ne-amf-01-updated</hostname>`)
	ts.send(`</system>`)
	ts.send(".")
	output := ts.read()
	if !strings.Contains(stripANSI(output), "OK") {
		t.Errorf("set did not return OK: %s", stripANSI(output))
	}

	// Commit
	ts.send("commit")
	output = ts.read()
	if !strings.Contains(stripANSI(output), "Commit successful") {
		t.Errorf("commit failed: %s", stripANSI(output))
	}

	// Verify
	ts.send("show running-config system hostname")
	output = ts.read()
	if !strings.Contains(stripANSI(output), "ne-amf-01-updated") {
		t.Errorf("config not updated after commit")
	}
}

func TestSetInline(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	// Set via inline path + value
	ts.send("set system hostname inline-test-host")
	output := ts.read()
	if !strings.Contains(stripANSI(output), "OK") {
		t.Errorf("inline set did not return OK: %s", stripANSI(output))
	}
}

func TestLockUnlock(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("lock")
	output := ts.read()
	if !strings.Contains(stripANSI(output), "Locked") {
		t.Errorf("lock failed: %s", stripANSI(output))
	}

	ts.send("unlock")
	output = ts.read()
	if !strings.Contains(stripANSI(output), "Unlocked") {
		t.Errorf("unlock failed: %s", stripANSI(output))
	}
}

func TestDiscardChanges(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("discard")
	output := ts.read()
	if !strings.Contains(stripANSI(output), "discarded") {
		t.Errorf("discard failed: %s", stripANSI(output))
	}
}

func TestHelp(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("help")
	output := ts.read()
	plain := stripANSI(output)

	for _, kw := range []string{"show ne", "connect", "show running-config", "set", "commit", "disconnect", "exit"} {
		if !strings.Contains(plain, kw) {
			t.Errorf("help missing: %s", kw)
		}
	}
}

func TestUnknownCommand(t *testing.T) {
	ts := newTestSession(t)
	defer ts.close()

	ts.send("foobar")
	output := ts.read()
	plain := stripANSI(output)

	if !strings.Contains(plain, "Unknown command") {
		t.Errorf("expected unknown command error: %s", plain)
	}
}

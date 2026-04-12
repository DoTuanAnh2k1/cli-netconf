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

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

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

// newSession creates an SSH session, auto-selects NE 1, ready to use.
func newSession(t *testing.T) *testSession {
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
	session.RequestPty("xterm", 80, 200, modes)
	stdin, _ := session.StdinPipe()
	stdout, _ := session.StdoutPipe()
	session.Shell()

	ts := &testSession{client: client, session: session, stdin: stdin, reader: newAsyncReader(stdout), t: t}
	// consume welcome + NE list
	ts.reader.collect(2 * time.Second)
	// select NE 1
	fmt.Fprintf(ts.stdin, "1\r")
	time.Sleep(2 * time.Second)
	out := ts.reader.collect(3 * time.Second)
	if !strings.Contains(stripANSI(out), "Connected") {
		t.Fatalf("auto-connect failed: %s", stripANSI(out))
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

func stripANSI(s string) string {
	var result strings.Builder
	i := 0
	for i < len(s) {
		if s[i] == '\033' {
			for i < len(s) && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '~') {
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

// ---------------------------------------------------------------------------
// Login & Navigation
// ---------------------------------------------------------------------------

func TestWelcomeBannerAndNEList(t *testing.T) {
	cfg := &ssh.ClientConfig{
		User: sshUser, Auth: []ssh.AuthMethod{ssh.Password(sshPass)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(), Timeout: 5 * time.Second,
	}
	client, _ := ssh.Dial("tcp", sshAddr, cfg)
	defer client.Close()
	session, _ := client.NewSession()
	defer session.Close()
	session.RequestPty("xterm", 80, 200, ssh.TerminalModes{ssh.ECHO: 0})
	stdin, _ := session.StdinPipe()
	stdout, _ := session.StdoutPipe()
	session.Shell()

	reader := newAsyncReader(stdout)
	out := stripANSI(reader.collect(3 * time.Second))

	if !strings.Contains(out, "VHT CLI - NETCONF Console") {
		t.Errorf("missing banner")
	}
	if !strings.Contains(out, "ne-amf-01") {
		t.Errorf("missing NE list")
	}
	if !strings.Contains(out, "Select NE") {
		t.Errorf("missing NE selection prompt")
	}
	fmt.Fprintf(stdin, "1\r")
	time.Sleep(time.Second)
	fmt.Fprintf(stdin, "exit\r")
}

func TestConnectByName(t *testing.T) {
	cfg := &ssh.ClientConfig{
		User: sshUser, Auth: []ssh.AuthMethod{ssh.Password(sshPass)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(), Timeout: 5 * time.Second,
	}
	client, _ := ssh.Dial("tcp", sshAddr, cfg)
	defer client.Close()
	session, _ := client.NewSession()
	defer session.Close()
	session.RequestPty("xterm", 80, 200, ssh.TerminalModes{ssh.ECHO: 0})
	stdin, _ := session.StdinPipe()
	stdout, _ := session.StdoutPipe()
	session.Shell()

	reader := newAsyncReader(stdout)
	reader.collect(2 * time.Second)

	// Enter invalid name first
	fmt.Fprintf(stdin, "nonexistent\r")
	time.Sleep(500 * time.Millisecond)
	out := stripANSI(reader.collect(2 * time.Second))
	if !strings.Contains(out, "not found") {
		t.Errorf("expected error for invalid name, got: %s", out)
	}

	// Now enter valid name
	fmt.Fprintf(stdin, "ne-amf-01\r")
	time.Sleep(2 * time.Second)
	out = stripANSI(reader.collect(3 * time.Second))
	if !strings.Contains(out, "Connected") {
		t.Errorf("connect by name failed: %s", out)
	}

	fmt.Fprintf(stdin, "exit\r")
}

func TestShowNE(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("show ne")
	time.Sleep(time.Second)
	out := stripANSI(ts.reader.collect(8 * time.Second))
	for _, ne := range []string{"ne-amf-01", "ne-smf-01", "ne-upf-01", "HCM", "HNI"} {
		if !strings.Contains(out, ne) {
			t.Errorf("missing %s in NE list", ne)
		}
	}
}

// ---------------------------------------------------------------------------
// Show config
// ---------------------------------------------------------------------------

func TestShowRunningConfig(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("show running-config")
	out := stripANSI(ts.read())
	for _, kw := range []string{"hostname", "ne-amf-01", "eth0", "ntp"} {
		if !strings.Contains(out, kw) {
			t.Errorf("missing %s in running config", kw)
		}
	}
}

func TestShowRunningConfigPathHeader(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	// Single leaf — should show "system hostname value"
	ts.send("show running-config system hostname")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "system hostname") {
		t.Errorf("missing path prefix, got: %s", out)
	}

	// Container — should show "system ntp\n  children..."
	ts.send("show running-config system ntp")
	out = stripANSI(ts.read())
	if !strings.Contains(out, "system ntp") {
		t.Errorf("missing path prefix for container, got: %s", out)
	}
	if !strings.Contains(out, "enabled") {
		t.Errorf("missing ntp children")
	}
}

// ---------------------------------------------------------------------------
// Set config
// ---------------------------------------------------------------------------

func TestSetInline(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("set system hostname inline-test")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "OK") {
		t.Errorf("set inline failed: %s", out)
	}
}

func TestSetXML(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("set")
	time.Sleep(200 * time.Millisecond)
	ts.send(`<system xmlns="urn:vht:params:xml:ns:yang:vht-system">`)
	ts.send(`  <hostname>xml-test</hostname>`)
	ts.send(`</system>`)
	ts.send(".")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "OK") {
		t.Errorf("set XML failed: %s", out)
	}
}

func TestSetTextConfigPaste(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	// Paste text config format (output of show running-config)
	ts.send("set")
	time.Sleep(200 * time.Millisecond)
	ts.send("system")
	ts.send("  hostname                 paste-test-host")
	ts.send("  location                 Test Location")
	ts.send(".")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "OK") {
		t.Errorf("set text paste failed: %s", out)
	}

	// Commit and verify
	ts.send("commit")
	out = stripANSI(ts.read())
	if !strings.Contains(out, "Commit successful") {
		t.Errorf("commit failed: %s", out)
	}

	ts.send("show running-config system hostname")
	out = stripANSI(ts.read())
	if !strings.Contains(out, "paste-test-host") {
		t.Errorf("config not applied: %s", out)
	}
}

func TestSetAndCommit(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("set system hostname commit-test")
	ts.read()

	ts.send("commit")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "Commit successful") {
		t.Errorf("commit failed: %s", out)
	}
}

// ---------------------------------------------------------------------------
// Unset config
// ---------------------------------------------------------------------------

func TestUnset(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("unset system contact")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "OK") {
		t.Errorf("unset failed: %s", out)
	}
}

// ---------------------------------------------------------------------------
// Config operations
// ---------------------------------------------------------------------------

func TestValidate(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("validate")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "Validation OK") {
		t.Errorf("validate failed: %s", out)
	}
}

func TestDiscard(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("discard")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "discarded") {
		t.Errorf("discard failed: %s", out)
	}
}

func TestLockUnlock(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("lock")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "Locked") {
		t.Errorf("lock failed: %s", out)
	}

	ts.send("unlock")
	out = stripANSI(ts.read())
	if !strings.Contains(out, "Unlocked") {
		t.Errorf("unlock failed: %s", out)
	}
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

func TestDumpText(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("dump text /tmp/test-dump.txt")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "Saved to") {
		t.Errorf("dump text failed: %s", out)
	}
}

func TestDumpXML(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("dump xml /tmp/test-dump.xml")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "Saved to") {
		t.Errorf("dump xml failed: %s", out)
	}
}

func TestDumpToTerminal(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("dump text")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "hostname") {
		t.Errorf("dump to terminal missing content: %s", out[:min(200, len(out))])
	}
}

// ---------------------------------------------------------------------------
// Tab completion
// ---------------------------------------------------------------------------

func TestTabCompleteCommand(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	fmt.Fprintf(ts.stdin, "sh\t")
	time.Sleep(300 * time.Millisecond)
	out := stripANSI(ts.reader.collect(2 * time.Second))
	if !strings.Contains(out, "show") {
		t.Errorf("tab complete command failed: %s", out)
	}
	fmt.Fprintf(ts.stdin, "\x15") // Ctrl+U
}

func TestTabCompleteShowSubcommand(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	fmt.Fprintf(ts.stdin, "show r\t")
	time.Sleep(300 * time.Millisecond)
	out := stripANSI(ts.reader.collect(2 * time.Second))
	if !strings.Contains(out, "running-config") {
		t.Errorf("tab complete subcommand failed: %s", out)
	}
	fmt.Fprintf(ts.stdin, "\x15")
}

func TestTabCompleteConfigPath(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	fmt.Fprintf(ts.stdin, "show running-config system n\t")
	time.Sleep(300 * time.Millisecond)
	out := stripANSI(ts.reader.collect(3 * time.Second))
	if !strings.Contains(out, "ntp") {
		t.Errorf("tab complete path failed: %s", out)
	}
	fmt.Fprintf(ts.stdin, "\x15")
}

func TestTabCompleteSet(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	fmt.Fprintf(ts.stdin, "set sys\t")
	time.Sleep(300 * time.Millisecond)
	out := stripANSI(ts.reader.collect(2 * time.Second))
	if !strings.Contains(out, "system") {
		t.Errorf("tab complete set failed: %s", out)
	}
	fmt.Fprintf(ts.stdin, "\x15")
}

func TestTabCompleteUnset(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	fmt.Fprintf(ts.stdin, "unset sys\t")
	time.Sleep(300 * time.Millisecond)
	out := stripANSI(ts.reader.collect(2 * time.Second))
	if !strings.Contains(out, "system") {
		t.Errorf("tab complete unset failed: %s", out)
	}
	fmt.Fprintf(ts.stdin, "\x15")
}

func TestTabCompleteMultipleOptions(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	// system has multiple children: hostname, location, contact, ntp, dns, logging
	fmt.Fprintf(ts.stdin, "show running-config system \t\t")
	time.Sleep(500 * time.Millisecond)
	out := stripANSI(ts.reader.collect(3 * time.Second))
	found := 0
	for _, child := range []string{"hostname", "ntp", "dns", "logging", "contact"} {
		if strings.Contains(out, child) {
			found++
		}
	}
	if found < 3 {
		t.Errorf("expected multiple options, found %d: %s", found, out)
	}
	fmt.Fprintf(ts.stdin, "\x15")
}

// ---------------------------------------------------------------------------
// Help & error handling
// ---------------------------------------------------------------------------

func TestHelp(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("help")
	out := stripANSI(ts.read())
	for _, kw := range []string{"show ne", "connect", "show running-config", "set", "unset", "commit", "dump", "exit"} {
		if !strings.Contains(out, kw) {
			t.Errorf("help missing: %s", kw)
		}
	}
}

func TestUnknownCommand(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	ts.send("foobar")
	out := stripANSI(ts.read())
	if !strings.Contains(out, "Unknown command") {
		t.Errorf("expected error: %s", out)
	}
}

// ---------------------------------------------------------------------------
// Stress tests
// ---------------------------------------------------------------------------

func generateLargeConfig(count int) string {
	var buf strings.Builder
	buf.WriteString(`<interfaces xmlns="urn:vht:params:xml:ns:yang:vht-system">`)
	for i := 0; i < count; i++ {
		fmt.Fprintf(&buf, `
  <interface>
    <name>veth%d</name>
    <description>Virtual Interface %d</description>
    <enabled>true</enabled>
    <mtu>1500</mtu>
    <ipv4>
      <address>10.%d.%d.%d</address>
      <prefix-length>24</prefix-length>
    </ipv4>
  </interface>`, i, i, (i/65536)%256, (i/256)%256, i%256)
	}
	buf.WriteString("\n</interfaces>")
	return buf.String()
}

func TestStress500Configs(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	config := generateLargeConfig(500)
	t.Logf("config size: %d bytes", len(config))

	ts.send("set")
	time.Sleep(300 * time.Millisecond)
	ts.reader.collect(2 * time.Second)
	for _, line := range strings.Split(config, "\n") {
		fmt.Fprintf(ts.stdin, "%s\r", line)
	}
	fmt.Fprintf(ts.stdin, ".\r")
	time.Sleep(3 * time.Second)
	out := stripANSI(ts.reader.collect(10 * time.Second))
	if !strings.Contains(out, "OK") {
		t.Fatalf("set 500 failed: %s", out[:min(500, len(out))])
	}

	ts.send("commit")
	time.Sleep(2 * time.Second)
	out = stripANSI(ts.reader.collect(5 * time.Second))
	if !strings.Contains(out, "Commit successful") {
		t.Fatalf("commit failed")
	}

	ts.send("show running-config interfaces")
	time.Sleep(2 * time.Second)
	out = stripANSI(ts.reader.collect(15 * time.Second))
	if c := strings.Count(out, "veth"); c < 100 {
		t.Errorf("expected many veth, got %d", c)
	}
	t.Logf("show output: %d bytes", len(out))
}

func TestStress1000Configs(t *testing.T) {
	ts := newSession(t)
	defer ts.close()

	config := generateLargeConfig(1000)
	t.Logf("config size: %d bytes", len(config))

	ts.send("set")
	time.Sleep(300 * time.Millisecond)
	ts.reader.collect(2 * time.Second)
	for _, line := range strings.Split(config, "\n") {
		fmt.Fprintf(ts.stdin, "%s\r", line)
	}
	fmt.Fprintf(ts.stdin, ".\r")
	time.Sleep(5 * time.Second)
	out := stripANSI(ts.reader.collect(15 * time.Second))
	if !strings.Contains(out, "OK") {
		t.Fatalf("set 1000 failed")
	}

	ts.send("commit")
	time.Sleep(3 * time.Second)
	out = stripANSI(ts.reader.collect(10 * time.Second))
	if !strings.Contains(out, "Commit successful") {
		t.Fatalf("commit failed")
	}

	ts.send("show running-config")
	time.Sleep(3 * time.Second)
	out = stripANSI(ts.reader.collect(30 * time.Second))
	if c := strings.Count(out, "veth"); c < 200 {
		t.Errorf("expected many veth, got %d", c)
	}
	t.Logf("show output: %d bytes", len(out))
}

// ---------------------------------------------------------------------------
// Multi-session
// ---------------------------------------------------------------------------

func TestMultiSession(t *testing.T) {
	const count = 5
	var wg sync.WaitGroup
	errors := make([]error, count)

	for i := 0; i < count; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			cfg := &ssh.ClientConfig{
				User: sshUser, Auth: []ssh.AuthMethod{ssh.Password(sshPass)},
				HostKeyCallback: ssh.InsecureIgnoreHostKey(), Timeout: 10 * time.Second,
			}
			client, err := ssh.Dial("tcp", sshAddr, cfg)
			if err != nil {
				errors[idx] = fmt.Errorf("session %d dial: %w", idx, err)
				return
			}
			defer client.Close()
			session, _ := client.NewSession()
			defer session.Close()
			session.RequestPty("xterm", 80, 200, ssh.TerminalModes{ssh.ECHO: 0})
			stdin, _ := session.StdinPipe()
			stdout, _ := session.StdoutPipe()
			session.Shell()

			reader := newAsyncReader(stdout)
			reader.collect(3 * time.Second)
			fmt.Fprintf(stdin, "1\r")
			time.Sleep(6 * time.Second)
			out := stripANSI(reader.collect(10 * time.Second))
			if !strings.Contains(out, "Connected") {
				errors[idx] = fmt.Errorf("session %d connect failed", idx)
				return
			}
			fmt.Fprintf(stdin, "show running-config system hostname\r")
			time.Sleep(5 * time.Second)
			out = stripANSI(reader.collect(15 * time.Second))
			if !strings.Contains(out, "hostname") {
				errors[idx] = fmt.Errorf("session %d: missing hostname", idx)
				return
			}
			fmt.Fprintf(stdin, "disconnect\r")
			time.Sleep(300 * time.Millisecond)
			fmt.Fprintf(stdin, "exit\r")
		}(i)
	}
	wg.Wait()
	for i, err := range errors {
		if err != nil {
			t.Errorf("%v", err)
		}
		_ = i
	}
	t.Logf("all %d sessions OK", count)
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

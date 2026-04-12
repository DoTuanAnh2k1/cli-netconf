package test

import (
	"fmt"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
)

// connectAndSelectNE creates a session with NE already auto-selected
func connectAndSelectNE(t *testing.T) (*ssh.Client, *ssh.Session, io.WriteCloser, *asyncReader) {
	t.Helper()
	return newCompletionSession(t) // already handles auto NE selection
}

func generateLargeConfig(count int) string {
	var buf strings.Builder
	buf.WriteString(`<interfaces xmlns="urn:vht:params:xml:ns:yang:vht-system">`)
	for i := 0; i < count; i++ {
		buf.WriteString(fmt.Sprintf(`
  <interface>
    <name>veth%d</name>
    <description>Virtual Interface %d - auto generated for stress testing</description>
    <enabled>true</enabled>
    <mtu>1500</mtu>
    <ipv4>
      <address>10.%d.%d.%d</address>
      <prefix-length>24</prefix-length>
    </ipv4>
  </interface>`, i, i, (i/65536)%256, (i/256)%256, i%256))
	}
	buf.WriteString("\n</interfaces>")
	return buf.String()
}

func TestStressSet500Configs(t *testing.T) {
	client, session, stdin, reader := connectAndSelectNE(t)
	defer client.Close()
	defer session.Close()

	config := generateLargeConfig(500)
	t.Logf("Config size: %d bytes (%d interfaces)", len(config), 500)

	// Send "set" command
	fmt.Fprintf(stdin, "set\r")
	time.Sleep(500 * time.Millisecond)
	reader.collect(2 * time.Second) // consume prompt

	// Paste the large config - send in chunks to avoid SSH buffer issues
	lines := strings.Split(config, "\n")
	for _, line := range lines {
		fmt.Fprintf(stdin, "%s\r", line)
	}
	// End multiline input
	fmt.Fprintf(stdin, ".\r")

	// Wait for response
	time.Sleep(3 * time.Second)
	output := reader.collect(10 * time.Second)
	plain := stripANSI(output)

	if !strings.Contains(plain, "OK") {
		t.Fatalf("set 500 configs failed, got: %s", plain[:min(len(plain), 500)])
	}
	t.Logf("set 500 configs: OK")

	// Commit
	fmt.Fprintf(stdin, "commit\r")
	time.Sleep(2 * time.Second)
	output = reader.collect(5 * time.Second)
	if !strings.Contains(stripANSI(output), "Commit successful") {
		t.Fatalf("commit failed: %s", stripANSI(output)[:min(len(stripANSI(output)), 500)])
	}
	t.Logf("commit: OK")

	// Now show running-config and verify it doesn't hang
	fmt.Fprintf(stdin, "show running-config interfaces\r")
	time.Sleep(2 * time.Second)
	output = reader.collect(15 * time.Second)
	plain = stripANSI(output)

	// Should contain many veth interfaces
	vethCount := strings.Count(plain, "veth")
	t.Logf("show running-config: received %d bytes, found %d veth references", len(output), vethCount)

	if vethCount < 100 {
		t.Errorf("expected many veth interfaces in output, only found %d", vethCount)
	}

	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(500 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestStressSet1000Configs(t *testing.T) {
	client, session, stdin, reader := connectAndSelectNE(t)
	defer client.Close()
	defer session.Close()

	config := generateLargeConfig(1000)
	t.Logf("Config size: %d bytes (%d interfaces)", len(config), 1000)

	fmt.Fprintf(stdin, "set\r")
	time.Sleep(500 * time.Millisecond)
	reader.collect(2 * time.Second)

	// Send in chunks
	lines := strings.Split(config, "\n")
	for _, line := range lines {
		fmt.Fprintf(stdin, "%s\r", line)
	}
	fmt.Fprintf(stdin, ".\r")

	time.Sleep(5 * time.Second)
	output := reader.collect(15 * time.Second)
	plain := stripANSI(output)

	if !strings.Contains(plain, "OK") {
		t.Fatalf("set 1000 configs failed, got: %s", plain[:min(len(plain), 500)])
	}
	t.Logf("set 1000 configs: OK")

	fmt.Fprintf(stdin, "commit\r")
	time.Sleep(3 * time.Second)
	output = reader.collect(10 * time.Second)
	if !strings.Contains(stripANSI(output), "Commit successful") {
		t.Fatalf("commit failed")
	}
	t.Logf("commit 1000: OK")

	// Show full config
	fmt.Fprintf(stdin, "show running-config\r")
	time.Sleep(3 * time.Second)
	output = reader.collect(30 * time.Second)
	plain = stripANSI(output)

	vethCount := strings.Count(plain, "veth")
	t.Logf("show running-config: received %d bytes, found %d veth references", len(output), vethCount)

	if vethCount < 200 {
		t.Errorf("expected many veth interfaces, only found %d", vethCount)
	}

	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(500 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestDumpText(t *testing.T) {
	client, session, stdin, reader := connectAndSelectNE(t)
	defer client.Close()
	defer session.Close()

	fmt.Fprintf(stdin, "dump text /tmp/test-dump.txt\r")
	time.Sleep(2 * time.Second)
	output := reader.collect(5 * time.Second)
	plain := stripANSI(output)

	if !strings.Contains(plain, "Saved to") {
		t.Fatalf("dump text failed: %s", plain)
	}
	t.Logf("dump text: %s", strings.TrimSpace(plain))

	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(300 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestDumpXML(t *testing.T) {
	client, session, stdin, reader := connectAndSelectNE(t)
	defer client.Close()
	defer session.Close()

	fmt.Fprintf(stdin, "dump xml /tmp/test-dump.xml\r")
	time.Sleep(2 * time.Second)
	output := reader.collect(5 * time.Second)
	plain := stripANSI(output)

	if !strings.Contains(plain, "Saved to") {
		t.Fatalf("dump xml failed: %s", plain)
	}
	t.Logf("dump xml: %s", strings.TrimSpace(plain))

	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(300 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestMultiSession(t *testing.T) {
	const sessionCount = 5
	var wg sync.WaitGroup
	results := make([]string, sessionCount)
	errors := make([]error, sessionCount)

	for i := 0; i < sessionCount; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()

			cfg := &ssh.ClientConfig{
				User:            sshUser,
				Auth:            []ssh.AuthMethod{ssh.Password(sshPass)},
				HostKeyCallback: ssh.InsecureIgnoreHostKey(),
				Timeout:         10 * time.Second,
			}
			client, err := ssh.Dial("tcp", sshAddr, cfg)
			if err != nil {
				errors[idx] = fmt.Errorf("session %d dial: %w", idx, err)
				return
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
			// Read welcome + NE list + Select prompt
			reader.collect(3 * time.Second)

			// Select NE 1
			fmt.Fprintf(stdin, "1\r")
			time.Sleep(4 * time.Second)
			output := reader.collect(5 * time.Second)

			if !strings.Contains(stripANSI(output), "Connected") {
				errors[idx] = fmt.Errorf("session %d connect failed: %s", idx, stripANSI(output))
				return
			}

			// show running-config
			fmt.Fprintf(stdin, "show running-config system hostname\r")
			time.Sleep(3 * time.Second)
			output = reader.collect(8 * time.Second)
			results[idx] = stripANSI(output)

			fmt.Fprintf(stdin, "disconnect\r")
			time.Sleep(300 * time.Millisecond)
			fmt.Fprintf(stdin, "exit\r")
		}(i)
	}

	wg.Wait()

	for i := 0; i < sessionCount; i++ {
		if errors[i] != nil {
			t.Errorf("%v", errors[i])
			continue
		}
		if !strings.Contains(results[i], "hostname") {
			t.Errorf("session %d: missing hostname in output: %s", i, results[i])
		}
	}
	t.Logf("All %d concurrent sessions completed successfully", sessionCount)
}

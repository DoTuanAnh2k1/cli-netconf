package test

import (
	"fmt"
	"io"
	"strings"
	"testing"
	"time"

	"golang.org/x/crypto/ssh"
)

// newCompletionSession creates an SSH session with NE auto-selected
func newCompletionSession(t *testing.T) (*ssh.Client, *ssh.Session, io.WriteCloser, *asyncReader) {
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
	session, _ := client.NewSession()
	modes := ssh.TerminalModes{ssh.ECHO: 0}
	session.RequestPty("xterm", 80, 200, modes)
	stdin, _ := session.StdinPipe()
	stdout, _ := session.StdoutPipe()
	session.Shell()

	reader := newAsyncReader(stdout)
	// consume welcome + NE list + Select prompt
	reader.collect(2 * time.Second)

	// Select NE 1
	fmt.Fprintf(stdin, "1\r")
	time.Sleep(2 * time.Second)
	output := reader.collect(3 * time.Second)
	if !strings.Contains(stripANSI(output), "Connected") {
		t.Fatalf("auto-connect failed: %s", stripANSI(output))
	}

	return client, session, stdin, reader
}

func sendTab(stdin io.WriteCloser) {
	fmt.Fprintf(stdin, "\t")
	time.Sleep(300 * time.Millisecond)
}

func sendText(stdin io.WriteCloser, text string) {
	fmt.Fprintf(stdin, "%s", text)
	time.Sleep(200 * time.Millisecond)
}

func TestTabCompleteCommand(t *testing.T) {
	client, session, stdin, reader := newCompletionSession(t)
	defer client.Close()
	defer session.Close()

	sendText(stdin, "sh")
	sendTab(stdin)
	output := reader.collect(2 * time.Second)
	plain := stripANSI(output)
	t.Logf("sh<TAB> → %q", plain)

	if !strings.Contains(plain, "show") {
		t.Errorf("expected 'show' completion, got: %q", plain)
	}

	fmt.Fprintf(stdin, "\x15") // Ctrl+U clear
	time.Sleep(100 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestTabCompleteShowNe(t *testing.T) {
	client, session, stdin, reader := newCompletionSession(t)
	defer client.Close()
	defer session.Close()

	sendText(stdin, "show ")
	sendTab(stdin)
	output := reader.collect(2 * time.Second)
	plain := stripANSI(output)
	t.Logf("show <TAB> → %q", plain)

	// Multiple completions: ne, running-config, candidate-config
	if !strings.Contains(plain, "ne") {
		t.Errorf("expected 'ne' in completions, got: %q", plain)
	}

	fmt.Fprintf(stdin, "\x15")
	time.Sleep(100 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestTabCompleteXpath(t *testing.T) {
	client, session, stdin, reader := newCompletionSession(t)
	defer client.Close()
	defer session.Close()

	// "show running-config " then Tab → should show system, interfaces
	sendText(stdin, "show running-config ")
	sendTab(stdin)
	output := reader.collect(3 * time.Second)
	plain := stripANSI(output)
	t.Logf("show running-config <TAB> → %q", plain)

	if !strings.Contains(plain, "system") && !strings.Contains(plain, "interfaces") {
		t.Errorf("expected path completions, got: %q", plain)
	}

	fmt.Fprintf(stdin, "\x15")
	time.Sleep(200 * time.Millisecond)
	reader.collect(time.Second)

	// "show running-config system n" then Tab → should complete to system ntp
	sendText(stdin, "show running-config system n")
	sendTab(stdin)
	output = reader.collect(3 * time.Second)
	plain = stripANSI(output)
	t.Logf("show running-config system n<TAB> → %q", plain)

	if !strings.Contains(plain, "ntp") {
		t.Errorf("expected 'ntp' completion, got: %q", plain)
	}

	fmt.Fprintf(stdin, "\x15")
	time.Sleep(100 * time.Millisecond)
	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(500 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestTabMultipleCompletions(t *testing.T) {
	client, session, stdin, reader := newCompletionSession(t)
	defer client.Close()
	defer session.Close()

	// "show running-config system " then Tab twice → should show children
	sendText(stdin, "show running-config system ")
	sendTab(stdin) // first tab: common prefix
	sendTab(stdin) // second tab: show options
	output := reader.collect(3 * time.Second)
	plain := stripANSI(output)
	t.Logf("system <TAB><TAB> → %q", plain)

	found := 0
	for _, child := range []string{"hostname", "location", "ntp", "dns", "logging", "contact"} {
		if strings.Contains(plain, child) {
			found++
		}
	}
	if found < 3 {
		t.Errorf("expected multiple children listed, only found %d, output: %q", found, plain)
	}

	fmt.Fprintf(stdin, "\x15")
	time.Sleep(100 * time.Millisecond)
	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(500 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

func TestTabCompleteSet(t *testing.T) {
	client, session, stdin, reader := newCompletionSession(t)
	defer client.Close()
	defer session.Close()

	// "set sys" then Tab → should complete to "set system"
	sendText(stdin, "set sys")
	sendTab(stdin)
	output := reader.collect(2 * time.Second)
	plain := stripANSI(output)
	t.Logf("set sys<TAB> → %q", plain)

	if !strings.Contains(plain, "system") {
		t.Errorf("expected 'system' completion, got: %q", plain)
	}

	fmt.Fprintf(stdin, "\x15")
	time.Sleep(100 * time.Millisecond)
	fmt.Fprintf(stdin, "disconnect\r")
	time.Sleep(500 * time.Millisecond)
	fmt.Fprintf(stdin, "exit\r")
}

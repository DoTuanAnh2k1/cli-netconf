package netconf

import (
	"bytes"
	"context"
	"encoding/xml"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/crypto/ssh"
)

const delimiter = "]]>]]>"

type Client struct {
	sshClient    *ssh.Client
	session      *ssh.Session
	tcpConn      net.Conn // set when using DialTCP
	stdin        io.WriteCloser
	stdout       io.Reader
	msgID        atomic.Uint64
	mu           sync.Mutex
	Capabilities []string
	SessionID    string
}

// XML structures for hello exchange
type helloMsg struct {
	XMLName      xml.Name `xml:"hello"`
	SessionID    string   `xml:"session-id"`
	Capabilities struct {
		Capability []string `xml:"capability"`
	} `xml:"capabilities"`
}

func Dial(ctx context.Context, host string, port int, username, password string) (*Client, error) {
	cfg := &ssh.ClientConfig{
		User:            username,
		Auth:            []ssh.AuthMethod{ssh.Password(password)},
		HostKeyCallback: ssh.InsecureIgnoreHostKey(),
		Timeout:         30 * time.Second,
	}

	addr := fmt.Sprintf("%s:%d", host, port)

	sshClient, err := ssh.Dial("tcp", addr, cfg)
	if err != nil {
		return nil, fmt.Errorf("ssh dial %s: %w", addr, err)
	}

	session, err := sshClient.NewSession()
	if err != nil {
		sshClient.Close()
		return nil, fmt.Errorf("ssh session: %w", err)
	}

	stdin, err := session.StdinPipe()
	if err != nil {
		session.Close()
		sshClient.Close()
		return nil, err
	}

	stdout, err := session.StdoutPipe()
	if err != nil {
		session.Close()
		sshClient.Close()
		return nil, err
	}

	if err := session.RequestSubsystem("netconf"); err != nil {
		session.Close()
		sshClient.Close()
		return nil, fmt.Errorf("request netconf subsystem: %w", err)
	}

	c := &Client{
		sshClient: sshClient,
		session:   session,
		stdin:     stdin,
		stdout:    stdout,
	}

	if err := c.exchangeHello(ctx); err != nil {
		c.Close()
		return nil, fmt.Errorf("hello exchange: %w", err)
	}

	return c, nil
}

// DialTCP connects to a NETCONF server over plain TCP (no SSH).
// ConfD must enable TCP transport in confd.conf.
func DialTCP(ctx context.Context, host string, port int) (*Client, error) {
	addr := fmt.Sprintf("%s:%d", host, port)

	d := net.Dialer{Timeout: 30 * time.Second}
	conn, err := d.DialContext(ctx, "tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("tcp dial %s: %w", addr, err)
	}

	// net.Conn implements both io.Reader and io.Writer directly.
	// Use connWriter to satisfy io.WriteCloser without closing the underlying conn here.
	c := &Client{
		tcpConn: conn,
		stdin:   &connWriter{conn},
		stdout:  conn,
	}

	if err := c.exchangeHello(ctx); err != nil {
		conn.Close()
		return nil, fmt.Errorf("hello exchange: %w", err)
	}

	return c, nil
}

// connWriter wraps net.Conn into an io.WriteCloser.
// Close is a no-op here; the actual connection is closed via tcpConn.
type connWriter struct{ net.Conn }

func (w *connWriter) Write(p []byte) (int, error) { return w.Conn.Write(p) }
func (w *connWriter) Close() error                { return nil }

func (c *Client) exchangeHello(ctx context.Context) error {
	serverHello, err := c.readMessage(ctx)
	if err != nil {
		return fmt.Errorf("read server hello: %w", err)
	}

	var hello helloMsg
	if err := xml.Unmarshal([]byte(serverHello), &hello); err == nil {
		c.SessionID = hello.SessionID
		c.Capabilities = hello.Capabilities.Capability
	}

	clientHello := `<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <capabilities>
    <capability>urn:ietf:params:netconf:base:1.0</capability>
  </capabilities>
</hello>`

	return c.writeMessage(clientHello)
}

type readResult struct {
	msg string
	err error
}

func (c *Client) readMessage(ctx context.Context) (string, error) {
	ch := make(chan readResult, 1)
	go func() {
		var buf bytes.Buffer
		tmp := make([]byte, 65536) // 64KB read buffer
		delimBytes := []byte(delimiter)
		delimLen := len(delimBytes)

		for {
			n, err := c.stdout.Read(tmp)
			if n > 0 {
				prevLen := buf.Len()
				buf.Write(tmp[:n])

				// Only search the newly written region (+ delimiter overlap)
				searchStart := prevLen - delimLen + 1
				if searchStart < 0 {
					searchStart = 0
				}
				region := buf.Bytes()[searchStart:]
				if idx := bytes.Index(region, delimBytes); idx >= 0 {
					msg := bytes.TrimSpace(buf.Bytes()[:searchStart+idx])
					ch <- readResult{string(msg), nil}
					return
				}
			}
			if err != nil {
				if buf.Len() > 0 {
					ch <- readResult{string(bytes.TrimSpace(buf.Bytes())), nil}
				} else {
					ch <- readResult{"", fmt.Errorf("read: %w", err)}
				}
				return
			}
		}
	}()

	select {
	case <-ctx.Done():
		return "", ctx.Err()
	case r := <-ch:
		return r.msg, r.err
	}
}

func (c *Client) writeMessage(msg string) error {
	_, err := fmt.Fprintf(c.stdin, "%s\n%s\n", msg, delimiter)
	return err
}

func (c *Client) SendRPC(ctx context.Context, rpcBody string) (string, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	id := c.msgID.Add(1)
	rpc := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<rpc message-id="%d" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
%s
</rpc>`, id, rpcBody)

	if err := c.writeMessage(rpc); err != nil {
		return "", fmt.Errorf("write rpc: %w", err)
	}

	reply, err := c.readMessage(ctx)
	if err != nil {
		return "", fmt.Errorf("read rpc-reply: %w", err)
	}

	return reply, nil
}

func (c *Client) GetConfig(ctx context.Context, source, filter string) (string, error) {
	body := fmt.Sprintf("  <get-config>\n    <source><%s/></source>", source)
	if filter != "" {
		body += fmt.Sprintf("\n    <filter type=\"xpath\" select=\"%s\"/>", escapeXMLAttr(filter))
	}
	body += "\n  </get-config>"
	return c.SendRPC(ctx, body)
}

func (c *Client) Get(ctx context.Context, filter string) (string, error) {
	body := "  <get>"
	if filter != "" {
		body += fmt.Sprintf("\n    <filter type=\"xpath\" select=\"%s\"/>", escapeXMLAttr(filter))
	}
	body += "\n  </get>"
	return c.SendRPC(ctx, body)
}

func (c *Client) EditConfig(ctx context.Context, target, config string) (string, error) {
	body := fmt.Sprintf(`  <edit-config>
    <target><%s/></target>
    <config>
%s
    </config>
  </edit-config>`, target, config)
	return c.SendRPC(ctx, body)
}

func (c *Client) Commit(ctx context.Context) (string, error) {
	return c.SendRPC(ctx, "  <commit/>")
}

func (c *Client) Validate(ctx context.Context, source string) (string, error) {
	body := fmt.Sprintf("  <validate>\n    <source><%s/></source>\n  </validate>", source)
	return c.SendRPC(ctx, body)
}

func (c *Client) DiscardChanges(ctx context.Context) (string, error) {
	return c.SendRPC(ctx, "  <discard-changes/>")
}

func (c *Client) Lock(ctx context.Context, target string) (string, error) {
	body := fmt.Sprintf("  <lock>\n    <target><%s/></target>\n  </lock>", target)
	return c.SendRPC(ctx, body)
}

func (c *Client) Unlock(ctx context.Context, target string) (string, error) {
	body := fmt.Sprintf("  <unlock>\n    <target><%s/></target>\n  </unlock>", target)
	return c.SendRPC(ctx, body)
}

// CopyConfig replaces an entire datastore with the given inline config XML.
// Used by the restore command to roll back to a saved snapshot.
func (c *Client) CopyConfig(ctx context.Context, target, configData string) (string, error) {
	body := fmt.Sprintf(`  <copy-config>
    <target><%s/></target>
    <source>
      <config>
%s
      </config>
    </source>
  </copy-config>`, target, configData)
	return c.SendRPC(ctx, body)
}

func (c *Client) GetSchema(ctx context.Context, identifier string) (string, error) {
	body := fmt.Sprintf(`  <get-schema xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring">
    <identifier>%s</identifier>
    <format>yang</format>
  </get-schema>`, identifier)
	return c.SendRPC(ctx, body)
}

// ExtractModules returns YANG module names from server capabilities.
func (c *Client) ExtractModules() []string {
	var modules []string
	for _, cap := range c.Capabilities {
		if idx := strings.Index(cap, "?module="); idx >= 0 {
			rest := cap[idx+8:]
			if amp := strings.Index(rest, "&"); amp >= 0 {
				rest = rest[:amp]
			}
			modules = append(modules, rest)
		}
	}
	return modules
}

func (c *Client) Close() error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c.SendRPC(ctx, "  <close-session/>")
	if c.tcpConn != nil {
		return c.tcpConn.Close()
	}
	c.session.Close()
	return c.sshClient.Close()
}

func escapeXMLAttr(s string) string {
	s = strings.ReplaceAll(s, "&", "&amp;")
	s = strings.ReplaceAll(s, "\"", "&quot;")
	s = strings.ReplaceAll(s, "<", "&lt;")
	s = strings.ReplaceAll(s, ">", "&gt;")
	return s
}

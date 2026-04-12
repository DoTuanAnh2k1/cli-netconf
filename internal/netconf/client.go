package netconf

import (
	"bytes"
	"context"
	"encoding/xml"
	"fmt"
	"io"
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
		tmp := make([]byte, 8192)
		for {
			n, err := c.stdout.Read(tmp)
			if n > 0 {
				buf.Write(tmp[:n])
				if idx := strings.Index(buf.String(), delimiter); idx >= 0 {
					msg := strings.TrimSpace(buf.String()[:idx])
					ch <- readResult{msg, nil}
					return
				}
			}
			if err != nil {
				if buf.Len() > 0 {
					ch <- readResult{strings.TrimSpace(buf.String()), nil}
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

func (c *Client) Close() error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	c.SendRPC(ctx, "  <close-session/>")
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

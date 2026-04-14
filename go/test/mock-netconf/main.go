package main

import (
	"bufio"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/xml"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"

	"golang.org/x/crypto/ssh"
)

// ---------------------------------------------------------------------------
// Data store — running + candidate config as raw XML strings
// ---------------------------------------------------------------------------

type dataStore struct {
	mu        sync.RWMutex
	running   string
	candidate string
	locked    map[string]bool // datastore name -> locked
}

func newDataStore() *dataStore {
	cfg := defaultRunningConfig()
	return &dataStore{
		running:   cfg,
		candidate: cfg,
		locked:    make(map[string]bool),
	}
}

func defaultRunningConfig() string {
	// NOTE: the <smf> block intentionally omits the <vsmf> wrapper element for
	// <qosConf> — this reproduces the ConfD behaviour where intermediate
	// containers whose leaves all carry default values are elided from the XML.
	// The schema merger fix must prevent qosConf from appearing directly under
	// nsmfPduSession in tab completion; it should only be reachable via vsmf.
	return `<smf xmlns="urn:5gc:smf-config">
    <nsmfPduSession>
      <common>
        <hostname>smf-01</hostname>
      </common>
      <qosConf>
        <useLocalQos>true</useLocalQos>
      </qosConf>
    </nsmfPduSession>
  </smf>
  <system xmlns="urn:params:xml:ns:yang:ne-system">
    <hostname>ne-amf-01</hostname>
    <location>HCM Data Center, Rack A3</location>
    <contact>noc@5gc.local</contact>
    <ntp>
      <enabled>true</enabled>
      <server>
        <address>10.0.0.1</address>
        <prefer>true</prefer>
      </server>
      <server>
        <address>10.0.0.2</address>
        <prefer>false</prefer>
      </server>
    </ntp>
    <dns>
      <search>5gc.local</search>
      <search>5gc.example</search>
      <server>
        <address>8.8.8.8</address>
      </server>
      <server>
        <address>8.8.4.4</address>
      </server>
    </dns>
    <logging>
      <level>info</level>
      <remote-server>
        <address>10.0.10.50</address>
        <port>514</port>
        <protocol>udp</protocol>
      </remote-server>
    </logging>
  </system>
  <interfaces xmlns="urn:params:xml:ns:yang:ne-system">
    <interface>
      <name>eth0</name>
      <description>Management Interface</description>
      <enabled>true</enabled>
      <mtu>1500</mtu>
      <ipv4>
        <address>10.0.1.10</address>
        <prefix-length>24</prefix-length>
        <gateway>10.0.1.1</gateway>
      </ipv4>
    </interface>
    <interface>
      <name>eth1</name>
      <description>N2 Interface (AMF-RAN)</description>
      <enabled>true</enabled>
      <mtu>9000</mtu>
      <ipv4>
        <address>172.16.0.10</address>
        <prefix-length>24</prefix-length>
      </ipv4>
    </interface>
    <interface>
      <name>eth2</name>
      <description>N11 Interface (AMF-SMF)</description>
      <enabled>true</enabled>
      <mtu>1500</mtu>
      <ipv4>
        <address>172.16.1.10</address>
        <prefix-length>24</prefix-length>
      </ipv4>
    </interface>
  </interfaces>`
}

// ---------------------------------------------------------------------------
// NETCONF XML RPC structures
// ---------------------------------------------------------------------------

const netconfDelimiter = "]]>]]>"

type rpcMsg struct {
	XMLName   xml.Name `xml:"rpc"`
	MessageID string   `xml:"message-id,attr"`
	Body      rpcBody  `xml:",any"`
}

type rpcBody struct {
	XMLName xml.Name
	Inner   string `xml:",innerxml"`
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	port := "8830"
	if p := os.Getenv("NETCONF_PORT"); p != "" {
		port = p
	}
	tcpPort := "2023"
	if p := os.Getenv("NETCONF_TCP_PORT"); p != "" {
		tcpPort = p
	}
	user := "admin"
	if u := os.Getenv("NETCONF_USER"); u != "" {
		user = u
	}
	pass := "admin"
	if p := os.Getenv("NETCONF_PASS"); p != "" {
		pass = p
	}

	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		log.Fatal(err)
	}
	signer, err := ssh.NewSignerFromKey(priv)
	if err != nil {
		log.Fatal(err)
	}

	sshCfg := &ssh.ServerConfig{
		PasswordCallback: func(conn ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
			if conn.User() == user && string(password) == pass {
				return nil, nil
			}
			return nil, fmt.Errorf("auth failed")
		},
	}
	sshCfg.AddHostKey(signer)

	ds := newDataStore()
	var sessionCounter atomic.Int64

	// SSH listener
	sshListener, err := net.Listen("tcp", ":"+port)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("Mock NETCONF/SSH listening on :%s (user=%s)", port, user)

	// TCP listener (no auth)
	tcpListener, err := net.Listen("tcp", ":"+tcpPort)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("Mock NETCONF/TCP listening on :%s (no auth)", tcpPort)

	go func() {
		for {
			conn, err := tcpListener.Accept()
			if err != nil {
				log.Println("tcp accept:", err)
				continue
			}
			go handleTCPConn(conn, ds, sessionCounter.Add(1))
		}
	}()

	for {
		conn, err := sshListener.Accept()
		if err != nil {
			log.Println("ssh accept:", err)
			continue
		}
		go handleSSHConn(conn, sshCfg, ds, sessionCounter.Add(1))
	}
}

// ---------------------------------------------------------------------------
// TCP connection (NETCONF over TCP, no SSH)
// ---------------------------------------------------------------------------

func handleTCPConn(conn net.Conn, ds *dataStore, sessionID int64) {
	defer conn.Close()
	log.Printf("[session %d] tcp connected from %s", sessionID, conn.RemoteAddr())
	handleNetconf(conn, ds, sessionID)
}

// ---------------------------------------------------------------------------
// SSH connection / channel handling
// ---------------------------------------------------------------------------

func handleSSHConn(nConn net.Conn, config *ssh.ServerConfig, ds *dataStore, sessionID int64) {
	defer nConn.Close()

	srvConn, chans, reqs, err := ssh.NewServerConn(nConn, config)
	if err != nil {
		log.Println("ssh handshake:", err)
		return
	}
	defer srvConn.Close()
	log.Printf("[session %d] user=%s connected", sessionID, srvConn.User())

	go ssh.DiscardRequests(reqs)

	for newCh := range chans {
		if newCh.ChannelType() != "session" {
			newCh.Reject(ssh.UnknownChannelType, "unknown channel type")
			continue
		}
		ch, chReqs, err := newCh.Accept()
		if err != nil {
			log.Println("channel accept:", err)
			continue
		}
		go handleChannel(ch, chReqs, ds, sessionID)
	}
}

func handleChannel(ch ssh.Channel, reqs <-chan *ssh.Request, ds *dataStore, sessionID int64) {
	defer ch.Close()

	for req := range reqs {
		if req.Type != "subsystem" {
			if req.WantReply {
				req.Reply(false, nil)
			}
			continue
		}

		subsystem := ""
		if len(req.Payload) > 4 {
			subsystem = string(req.Payload[4:])
		}
		if subsystem != "netconf" {
			req.Reply(false, nil)
			continue
		}
		req.Reply(true, nil)

		handleNetconf(ch, ds, sessionID)
		return
	}
}

// ---------------------------------------------------------------------------
// NETCONF session
// ---------------------------------------------------------------------------

func handleNetconf(ch io.ReadWriter, ds *dataStore, sessionID int64) {
	// Send server hello
	hello := fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<hello xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <capabilities>
    <capability>urn:ietf:params:netconf:base:1.0</capability>
    <capability>urn:ietf:params:netconf:capability:candidate:1.0</capability>
    <capability>urn:ietf:params:netconf:capability:confirmed-commit:1.0</capability>
    <capability>urn:ietf:params:netconf:capability:validate:1.0</capability>
    <capability>urn:ietf:params:netconf:capability:xpath:1.0</capability>
    <capability>urn:params:xml:ns:yang:ne-system?module=ne-system&amp;revision=2024-01-01</capability>
    <capability>urn:5gc:smf-config?module=smf-config&amp;revision=2024-01-01</capability>
  </capabilities>
  <session-id>%d</session-id>
</hello>`, sessionID)

	writeMsg(ch, hello)

	// Use a buffered reader so bytes read past one ]]>]]> are not lost
	br := bufio.NewReader(ch)

	// Read client hello (and discard)
	_, err := readMsg(br)
	if err != nil {
		log.Printf("[session %d] read client hello: %v", sessionID, err)
		return
	}
	log.Printf("[session %d] hello exchanged", sessionID)

	// RPC loop
	for {
		msg, err := readMsg(br)
		if err != nil {
			if err != io.EOF {
				log.Printf("[session %d] read rpc: %v", sessionID, err)
			}
			return
		}

		reply := processRPC(msg, ds, sessionID)
		if reply == "" {
			return // close-session
		}
		writeMsg(ch, reply)
	}
}

// ---------------------------------------------------------------------------
// RPC processing
// ---------------------------------------------------------------------------

func processRPC(raw string, ds *dataStore, sessionID int64) string {
	var rpc rpcMsg
	if err := xml.Unmarshal([]byte(raw), &rpc); err != nil {
		return rpcErrorReply("0", "rpc-error", "malformed-message", "Could not parse RPC: "+err.Error())
	}

	msgID := rpc.MessageID
	op := rpc.Body.XMLName.Local
	inner := rpc.Body.Inner

	log.Printf("[session %d] rpc: %s (msg-id=%s)", sessionID, op, msgID)

	switch op {
	case "get-config":
		return handleGetConfig(msgID, inner, ds)
	case "get":
		return handleGet(msgID, inner, ds)
	case "edit-config":
		return handleEditConfig(msgID, inner, ds)
	case "commit":
		return handleCommit(msgID, ds)
	case "validate":
		return rpcOKReply(msgID)
	case "discard-changes":
		return handleDiscard(msgID, ds)
	case "lock":
		return handleLock(msgID, inner, ds, true)
	case "unlock":
		return handleLock(msgID, inner, ds, false)
	case "get-schema":
		return handleGetSchema(msgID, inner)
	case "close-session":
		writeMsg(nil, "") // signal handled by caller
		log.Printf("[session %d] close-session", sessionID)
		return ""
	default:
		return rpcErrorReply(msgID, "protocol", "operation-not-supported",
			fmt.Sprintf("Operation '%s' not supported", op))
	}
}

// --- get-config ---

func handleGetConfig(msgID, inner string, ds *dataStore) string {
	source := extractDatastore(inner, "source")
	ds.mu.RLock()
	defer ds.mu.RUnlock()

	var data string
	switch source {
	case "candidate":
		data = ds.candidate
	default:
		data = ds.running
	}

	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="%s" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <data>
  %s
  </data>
</rpc-reply>`, msgID, data)
}

// --- get (returns same as running + operational placeholders) ---

func handleGet(msgID, inner string, ds *dataStore) string {
	ds.mu.RLock()
	data := ds.running
	ds.mu.RUnlock()

	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="%s" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <data>
  %s
  </data>
</rpc-reply>`, msgID, data)
}

// --- edit-config ---

func handleEditConfig(msgID, inner string, ds *dataStore) string {
	target := extractDatastore(inner, "target")
	if target != "candidate" {
		return rpcErrorReply(msgID, "protocol", "invalid-value",
			"Only candidate datastore supports edit-config")
	}

	// Extract <config>...</config> content
	configData := extractTag(inner, "config")
	if configData == "" {
		return rpcErrorReply(msgID, "protocol", "missing-element", "Missing <config> element")
	}

	ds.mu.Lock()
	defer ds.mu.Unlock()

	if ds.locked["candidate"] {
		// locked by someone — in real impl check session ownership
	}

	// Simple merge: replace candidate with the new config content
	// In a real implementation this would do a proper XML merge
	ds.candidate = configData

	return rpcOKReply(msgID)
}

// --- commit ---

func handleCommit(msgID string, ds *dataStore) string {
	ds.mu.Lock()
	defer ds.mu.Unlock()
	ds.running = ds.candidate
	log.Printf("commit: candidate -> running applied")
	return rpcOKReply(msgID)
}

// --- discard-changes ---

func handleDiscard(msgID string, ds *dataStore) string {
	ds.mu.Lock()
	defer ds.mu.Unlock()
	ds.candidate = ds.running
	return rpcOKReply(msgID)
}

// --- lock / unlock ---

func handleLock(msgID, inner string, ds *dataStore, lock bool) string {
	target := extractDatastore(inner, "target")
	if target == "" {
		target = "candidate"
	}

	ds.mu.Lock()
	defer ds.mu.Unlock()

	if lock {
		if ds.locked[target] {
			return rpcErrorReply(msgID, "protocol", "lock-denied",
				fmt.Sprintf("Datastore '%s' is already locked", target))
		}
		ds.locked[target] = true
	} else {
		delete(ds.locked, target)
	}
	return rpcOKReply(msgID)
}

// --- get-schema (RFC 6022) ---

func handleGetSchema(msgID, inner string) string {
	// Extract identifier from <identifier>ne-system</identifier>
	identifier := extractTag(inner, "identifier")
	if identifier == "" {
		return rpcErrorReply(msgID, "protocol", "missing-element", "Missing <identifier>")
	}

	schema, ok := yangSchemas[identifier]
	if !ok {
		return rpcErrorReply(msgID, "data", "invalid-value",
			fmt.Sprintf("Schema '%s' not found", identifier))
	}

	// Escape XML special chars in YANG text
	escaped := strings.ReplaceAll(schema, "&", "&amp;")
	escaped = strings.ReplaceAll(escaped, "<", "&lt;")
	escaped = strings.ReplaceAll(escaped, ">", "&gt;")

	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="%s" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <data xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring">%s</data>
</rpc-reply>`, msgID, escaped)
}

var yangSchemas = map[string]string{
	"ne-system":  neSystemYang,
	"smf-config": smfConfigYang,
}

const neSystemYang = `module ne-system {
  namespace "urn:params:xml:ns:yang:ne-system";
  prefix sys;

  grouping ntp-server-config {
    leaf address { type string; }
    leaf prefer { type boolean; }
  }

  grouping ipv4-config {
    leaf address { type string; }
    leaf prefix-length { type uint8; }
    leaf gateway { type string; }
  }

  container system {
    leaf hostname { type string; }
    leaf location { type string; }
    leaf contact { type string; }
    container ntp {
      leaf enabled { type boolean; }
      list server {
        key "address";
        uses ntp-server-config;
      }
    }
    container dns {
      leaf-list search { type string; }
      list server {
        key "address";
        leaf address { type string; }
      }
    }
    container logging {
      leaf level { type string; }
      list remote-server {
        key "address";
        leaf address { type string; }
        leaf port { type uint16; }
        leaf protocol { type string; }
      }
    }
  }

  container interfaces {
    list interface {
      key "name";
      leaf name { type string; }
      leaf description { type string; }
      leaf enabled { type boolean; }
      leaf mtu { type uint16; }

      container ipv4 {
        uses ipv4-config;
      }
    }
  }
}`

// smfConfigYang is a representative subset of the real smf-config YANG module.
// It captures the exact nesting that triggers the qosConf tab-completion bug:
//   grouping gpNsmfPduSession → container vsmf → container qosConf
// The running config XML omits the <vsmf> wrapper (ConfD elides containers
// whose leaves all use default values), so parseSchemaFromXML would place
// qosConf one level too high without the schema-merge fix.
const smfConfigYang = `module smf-config {
  namespace "urn:5gc:smf-config";
  prefix smf;

  grouping gpNsmfPduSession {
    container common {
      leaf hostname {
        type string;
      }
    }
    container vsmf {
      container httpProtocol {
        leaf host {
          type string;
        }
        leaf port {
          type uint16;
        }
      }
      container gtpProtocol {
        leaf listenPort {
          type uint16;
        }
      }
      container features {
        leaf enableFeatureA {
          type boolean;
          default false;
        }
        leaf enableFeatureB {
          type boolean;
          default false;
        }
      }
      container qosConf {
        leaf useLocalQos {
          type boolean;
          default true;
        }
        container predefined {
          leaf enabled {
            type boolean;
            default false;
          }
        }
      }
    }
  }

  container smf {
    container nsmfPduSession {
      uses gpNsmfPduSession;
    }
  }
}`

// ---------------------------------------------------------------------------
// XML helpers
// ---------------------------------------------------------------------------

func rpcOKReply(msgID string) string {
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="%s" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <ok/>
</rpc-reply>`, msgID)
}

func rpcErrorReply(msgID, errType, errTag, errMsg string) string {
	return fmt.Sprintf(`<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="%s" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <rpc-error>
    <error-type>%s</error-type>
    <error-tag>%s</error-tag>
    <error-severity>error</error-severity>
    <error-message xml:lang="en">%s</error-message>
  </rpc-error>
</rpc-reply>`, msgID, errType, errTag, errMsg)
}

// extractDatastore finds <source><running/></source> or <target><candidate/></target>
func extractDatastore(xmlStr, wrapper string) string {
	start := strings.Index(xmlStr, "<"+wrapper+">")
	if start < 0 {
		return "running"
	}
	end := strings.Index(xmlStr, "</"+wrapper+">")
	if end < 0 {
		return "running"
	}
	content := xmlStr[start+len(wrapper)+2 : end]
	content = strings.TrimSpace(content)

	// Find <running/>, <candidate/>, <startup/>
	for _, ds := range []string{"running", "candidate", "startup"} {
		if strings.Contains(content, "<"+ds+"/>") || strings.Contains(content, "<"+ds+">") {
			return ds
		}
	}
	return "running"
}

// extractTag extracts inner content of a given tag
func extractTag(xmlStr, tag string) string {
	start := strings.Index(xmlStr, "<"+tag+">")
	if start < 0 {
		// try self-closing with attributes
		start = strings.Index(xmlStr, "<"+tag+" ")
		if start < 0 {
			return ""
		}
	}
	// Find the content after the opening tag
	tagEnd := strings.Index(xmlStr[start:], ">")
	if tagEnd < 0 {
		return ""
	}
	contentStart := start + tagEnd + 1

	endTag := "</" + tag + ">"
	end := strings.Index(xmlStr[contentStart:], endTag)
	if end < 0 {
		return ""
	}

	return strings.TrimSpace(xmlStr[contentStart : contentStart+end])
}

// ---------------------------------------------------------------------------
// NETCONF framing (base:1.0 — delimiter ]]>]]>)
// ---------------------------------------------------------------------------

// readMsg reads one NETCONF message (terminated by ]]>]]>) from a bufio.Reader.
// Using bufio.Reader ensures that bytes consumed past the delimiter in a single
// Read call are retained in the buffer for the next readMsg call.
func readMsg(r *bufio.Reader) (string, error) {
	var buf strings.Builder
	for {
		b, err := r.ReadByte()
		if err != nil {
			return "", err
		}
		buf.WriteByte(b)
		s := buf.String()
		if idx := strings.Index(s, netconfDelimiter); idx >= 0 {
			return strings.TrimSpace(s[:idx]), nil
		}
	}
}

func writeMsg(w io.Writer, msg string) {
	if w == nil {
		return
	}
	fmt.Fprintf(w, "%s\n%s\n", msg, netconfDelimiter)
}

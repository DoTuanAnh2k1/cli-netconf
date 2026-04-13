package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"strconv"
	"syscall"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/api"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/netconf"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/server"
)

// Connection parameters — override via environment variables.
//
//	NETCONF_HOST  target host          (default: 127.0.0.1)
//	NETCONF_PORT  target port          (default: 2023)
//	NETCONF_MODE  "tcp" or "ssh"       (default: tcp)
//	NETCONF_USER  SSH username         (default: admin, ssh mode only)
//	NETCONF_PASS  SSH password         (default: admin, ssh mode only)
//	NE_NAME       label shown in prompt (default: confd)
const (
	defaultHost = "127.0.0.1"
	defaultPort = 2023
	defaultMode = "tcp"
	defaultUser = "admin"
	defaultPass = "admin"
	defaultName = "confd"
)

// stdioRW combines stdin and stdout into a single io.ReadWriter for term.Terminal.
type stdioRW struct {
	io.Reader
	io.Writer
}

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func main() {
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelWarn, // suppress info logs so they don't mix into terminal output
	})))

	host := envOr("NETCONF_HOST", defaultHost)
	mode := envOr("NETCONF_MODE", defaultMode)
	neName := envOr("NE_NAME", defaultName)
	user := envOr("NETCONF_USER", defaultUser)
	pass := envOr("NETCONF_PASS", defaultPass)

	port := defaultPort
	if p := os.Getenv("NETCONF_PORT"); p != "" {
		if n, err := strconv.Atoi(p); err == nil {
			port = n
		}
	}

	fmt.Fprintf(os.Stderr, "Connecting to %s:%d (mode=%s) ...\n", host, port, mode)

	var (
		nc  *netconf.Client
		err error
	)
	switch mode {
	case "ssh":
		nc, err = netconf.Dial(context.Background(), host, port, user, pass)
	default:
		nc, err = netconf.DialTCP(context.Background(), host, port)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "Connected. NETCONF session ID: %s\n", nc.SessionID)

	// Graceful shutdown on SIGINT / SIGTERM
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		nc.Close()
		os.Exit(0)
	}()

	ne := &api.NeDataItem{
		Ne:   neName,
		IP:   host,
		Port: port,
	}

	server.RunDirect(nc, ne, stdioRW{os.Stdin, os.Stdout})
}

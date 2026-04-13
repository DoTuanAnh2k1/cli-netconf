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
//	NETCONF_HOST  target host           (default: 127.0.0.1)
//	NETCONF_PORT  target port           (default: 2023)
//	NE_NAME       label shown in prompt (default: confd)

const (
	defaultHost = "127.0.0.1"
	defaultPort = 2023
	defaultName = "confd"
)

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
		Level: slog.LevelWarn,
	})))

	host := envOr("NETCONF_HOST", defaultHost)
	neName := envOr("NE_NAME", defaultName)

	port := defaultPort
	if p := os.Getenv("NETCONF_PORT"); p != "" {
		if n, err := strconv.Atoi(p); err == nil {
			port = n
		}
	}

	fmt.Fprintf(os.Stderr, "Connecting to %s:%d (TCP) ...\n", host, port)

	nc, err := netconf.DialTCP(context.Background(), host, port)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "Connected. NETCONF session ID: %s\n", nc.SessionID)

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

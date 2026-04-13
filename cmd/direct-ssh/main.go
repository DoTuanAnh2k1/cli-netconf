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
	"golang.org/x/term"
)

// Connection parameters — override via environment variables.
//
//	NETCONF_HOST  target host           (default: 171.16.25.131)
//	NETCONF_PORT  target port           (default: 2075)
//	NETCONF_USER  SSH username          (default: admin)
//	NETCONF_PASS  SSH password          (default: admin)
//	NE_NAME       label shown in prompt (default: confd)

const (
	defaultHost = "171.16.25.131"
	defaultPort = 2075
	defaultUser = "admin"
	defaultPass = "admin"
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
	user := envOr("NETCONF_USER", defaultUser)
	pass := envOr("NETCONF_PASS", defaultPass)
	neName := envOr("NE_NAME", defaultName)

	port := defaultPort
	if p := os.Getenv("NETCONF_PORT"); p != "" {
		if n, err := strconv.Atoi(p); err == nil {
			port = n
		}
	}

	fmt.Fprintf(os.Stderr, "Connecting to %s:%d (SSH, user=%s) ...\n", host, port, user)

	nc, err := netconf.Dial(context.Background(), host, port, user, pass)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "Connected. NETCONF session ID: %s\n", nc.SessionID)

	// Put stdin into raw mode so Tab and other control keys reach the program.
	// Must be restored before exit to avoid leaving the terminal broken.
	oldState, err := term.MakeRaw(int(os.Stdin.Fd()))
	if err != nil {
		fmt.Fprintf(os.Stderr, "Warning: could not set raw mode: %v\n", err)
	} else {
		defer term.Restore(int(os.Stdin.Fd()), oldState)
	}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sigCh
		if oldState != nil {
			term.Restore(int(os.Stdin.Fd()), oldState)
		}
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

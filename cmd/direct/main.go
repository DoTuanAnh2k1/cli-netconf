package main

import (
	"context"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/api"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/netconf"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/server"

)

// ---------------------------------------------------------------------------
// Hard-coded ConfD NETCONF over TCP endpoint.
// ConfD must enable TCP transport in confd.conf (default port 2023).
// ---------------------------------------------------------------------------

const (
	netconfHost = "127.0.0.1"
	netconfPort = 2023
	neName      = "confd-local"
)

// stdioRW combines stdin and stdout into a single io.ReadWriter for term.Terminal.
type stdioRW struct {
	io.Reader
	io.Writer
}

func main() {
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelWarn, // suppress info logs so they don't mix into terminal output
	})))

	fmt.Fprintf(os.Stderr, "Connecting to %s:%d ...\n", netconfHost, netconfPort)

	nc, err := netconf.DialTCP(context.Background(), netconfHost, netconfPort)
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
		IP:   netconfHost,
		Port: netconfPort,
	}

	server.RunDirect(nc, ne, stdioRW{os.Stdin, os.Stdout})
}

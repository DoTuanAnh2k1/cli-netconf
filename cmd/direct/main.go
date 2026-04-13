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
// Hard-coded ConfD NETCONF endpoint — chỉnh sửa theo môi trường thực tế
// ---------------------------------------------------------------------------

const (
	netconfHost = "127.0.0.1"
	netconfPort = 830
	netconfUser = "admin"
	netconfPass = "admin"
	neName      = "confd-local"
)

// stdioRW ghép stdin + stdout thành một io.ReadWriter cho term.Terminal.
type stdioRW struct {
	io.Reader
	io.Writer
}

func main() {
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{
		Level: slog.LevelWarn, // ẩn info log để không lẫn vào terminal output
	})))

	fmt.Fprintf(os.Stderr, "Connecting to %s:%d ...\n", netconfHost, netconfPort)

	nc, err := netconf.Dial(context.Background(), netconfHost, netconfPort, netconfUser, netconfPass)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "Connected. NETCONF session ID: %s\n", nc.SessionID)

	// Graceful shutdown khi nhận SIGINT / SIGTERM
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

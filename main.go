package main

import (
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/DoTuanAnh2k1/cli-netconf/internal/api"
	"github.com/DoTuanAnh2k1/cli-netconf/internal/config"
	"github.com/DoTuanAnh2k1/cli-netconf/internal/server"
)

func main() {
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{
		Level: slog.LevelInfo,
	})))

	cfg := config.Load()
	apiClient := api.NewClient(cfg.MgtServiceURL)

	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
		sig := <-sigCh
		slog.Info("received signal, shutting down", "signal", sig)
		os.Exit(0)
	}()

	if err := server.Start(cfg, apiClient); err != nil {
		slog.Error("server failed", "error", err)
		os.Exit(1)
	}
}

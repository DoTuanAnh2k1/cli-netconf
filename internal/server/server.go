package server

import (
	"crypto/ed25519"
	"crypto/rand"
	"fmt"
	"log/slog"
	"os"

	"github.com/DoTuanAnh2k1/cli-netconf/internal/api"
	"github.com/DoTuanAnh2k1/cli-netconf/internal/config"

	gssh "github.com/gliderlabs/ssh"
	gossh "golang.org/x/crypto/ssh"
)

type contextKey string

const (
	keyToken      contextKey = "jwt_token"
	keySystemType contextKey = "system_type"
)

func Start(cfg *config.Config, apiClient *api.Client) error {
	signer, err := loadOrGenerateHostKey(cfg.HostKeyPath)
	if err != nil {
		return fmt.Errorf("host key: %w", err)
	}

	server := &gssh.Server{
		Addr: cfg.SSHAddr,
		Handler: func(s gssh.Session) {
			handleSession(s, apiClient, cfg)
		},
		PasswordHandler: func(ctx gssh.Context, password string) bool {
			resp, err := apiClient.Authenticate(ctx.User(), password)
			if err != nil {
				slog.Warn("auth failed", "user", ctx.User(), "error", err)
				return false
			}
			ctx.SetValue(keyToken, resp.ResponseData)
			ctx.SetValue(keySystemType, resp.SystemType)
			slog.Info("auth success", "user", ctx.User(), "system_type", resp.SystemType)
			return true
		},
	}
	server.AddHostKey(signer)

	slog.Info("SSH server starting", "addr", cfg.SSHAddr)
	return server.ListenAndServe()
}

func loadOrGenerateHostKey(path string) (gossh.Signer, error) {
	if path != "" {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("read host key %s: %w", path, err)
		}
		return gossh.ParsePrivateKey(data)
	}

	slog.Warn("no SSH_HOST_KEY_PATH configured, generating ephemeral host key (not for production)")
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	return gossh.NewSignerFromKey(priv)
}

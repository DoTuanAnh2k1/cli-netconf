package config

import (
	"os"
	"time"
)

type Config struct {
	SSHAddr        string
	HostKeyPath    string
	MgtServiceURL  string
	NetconfUser    string
	NetconfPass    string
	NetconfTimeout time.Duration
	YangDir        string // directory containing .yang files to load as local schema fallback
}

func Load() *Config {
	return &Config{
		SSHAddr:        envOr("SSH_ADDR", ":2222"),
		HostKeyPath:    os.Getenv("SSH_HOST_KEY_PATH"),
		MgtServiceURL:  envOr("MGT_SERVICE_URL", "http://mgt-service:3000"),
		NetconfUser:    envOr("NETCONF_USERNAME", "admin"),
		NetconfPass:    envOr("NETCONF_PASSWORD", "admin"),
		NetconfTimeout: parseDuration(os.Getenv("NETCONF_TIMEOUT"), 30*time.Second),
		YangDir:        envOr("YANG_DIR", "./yang"),
	}
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

func parseDuration(s string, def time.Duration) time.Duration {
	if s == "" {
		return def
	}
	d, err := time.ParseDuration(s)
	if err != nil {
		return def
	}
	return d
}

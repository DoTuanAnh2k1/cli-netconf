package server

import (
	"io"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/api"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/config"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/netconf"
	"golang.org/x/term"
)

// RunDirect runs an interactive NETCONF session directly on rw (e.g. os.Stdin/Stdout),
// bypassing the SSH server and mgt-service. nc must already be connected.
func RunDirect(nc *netconf.Client, ne *api.NeDataItem, rw io.ReadWriter) {
	cfg := &config.Config{
		NetconfTimeout: 30 * time.Second,
	}

	sess := &session{
		term:      term.NewTerminal(rw, ""),
		sshConn:   rw,
		rawReader: rw,
		cfg:       cfg,
		username:  "admin",
		nc:        nc,
		currentNE: ne,
	}

	// Read actual terminal size from stdin so term.Terminal doesn't default to 80 cols.
	if w, h, err := term.GetSize(int(os.Stdin.Fd())); err == nil {
		sess.term.SetSize(w, h)
	}

	// Watch for terminal resize (SIGWINCH) and update term.Terminal accordingly.
	sigWinCh := make(chan os.Signal, 1)
	signal.Notify(sigWinCh, syscall.SIGWINCH)
	go func() {
		for range sigWinCh {
			if w, h, err := term.GetSize(int(os.Stdin.Fd())); err == nil {
				sess.term.SetSize(w, h)
			}
		}
	}()
	defer signal.Stop(sigWinCh)

	sess.updatePrompt()
	sess.term.AutoCompleteCallback = sess.handleComplete
	sess.welcome()
	sess.loadSchema()
	sess.run()
}

package server

import (
	"fmt"
	"io"
	"log/slog"
	"strings"
	"time"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/api"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/config"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/netconf"

	gssh "github.com/gliderlabs/ssh"
	"golang.org/x/term"
)

const (
	colorRed    = "\033[31m"
	colorGreen  = "\033[32m"
	colorYellow = "\033[33m"
	colorCyan   = "\033[36m"
	colorBold   = "\033[1m"
	colorDim    = "\033[90m"
	colorReset  = "\033[0m"
)

// configBackup is a point-in-time snapshot of the running config captured
// automatically after each successful commit.
type configBackup struct {
	id        int       // local sequential ID shown to the user
	remoteID  int       // server-assigned ID from mgt-service (0 = not yet persisted)
	timestamp time.Time
	rawXML    string // full rpc-reply XML from get-config; empty for remote-only entries (lazy-loaded on restore)
}

type session struct {
	term      *term.Terminal
	sshConn   io.Writer // raw SSH channel for completion display
	rawReader io.Reader // underlying reader — used for pager key detection
	api       *api.Client
	cfg       *config.Config
	token     string
	username  string
	promptStr string
	neList    []api.NeDataItem
	neConfigs map[string]*api.CliNeConfig // ne_name → NETCONF config (from /aa/list/ne/config)
	currentNE *api.NeDataItem
	nc        *netconf.Client
	schema    *schemaNode   // YANG schema tree for tab completion
	backups   []configBackup // per-connection config snapshots
	backupSeq int
}

func handleSession(s gssh.Session, apiClient *api.Client, cfg *config.Config) {
	token, _ := s.Context().Value(keyToken).(string)
	slog.Info("session started", "user", s.User(), "remote_addr", s.RemoteAddr())

	sess := &session{
		term:      term.NewTerminal(s, ""),
		sshConn:   s,
		rawReader: s,
		api:       apiClient,
		cfg:       cfg,
		token:     token,
		username:  s.User(),
	}

	// Set terminal width/height from the SSH client's PTY and watch for resizes.
	// Without this, term.Terminal defaults to 80 columns and lines wrap early.
	if pty, winCh, ok := s.Pty(); ok {
		sess.term.SetSize(pty.Window.Width, pty.Window.Height)
		go func() {
			for win := range winCh {
				sess.term.SetSize(win.Width, win.Height)
			}
		}()
	}

	sess.updatePrompt()
	sess.term.AutoCompleteCallback = sess.handleComplete
	sess.welcome()

	// Outer loop: NE selection → command loop → back to NE selection on disconnect.
	// Exit the outer loop when the user explicitly types "exit" at the NE selection
	// prompt, or when the SSH connection drops.
	for {
		if !sess.autoSelectNE() {
			break // user typed "exit" at NE selection, or connection dropped
		}
		if sess.run() {
			break // user typed "exit" while not connected (already disconnected)
		}
		// run() returned false → user typed "exit" while connected, or "disconnect"
		// Loop back to show NE list again.
	}
	slog.Info("session ended", "user", s.User(), "remote_addr", s.RemoteAddr())
	sess.writef("Goodbye.\n")
}

func (s *session) updatePrompt() {
	if s.currentNE != nil {
		s.promptStr = fmt.Sprintf("%s%s%s[%s%s%s]> ",
			colorCyan, s.username, colorReset,
			colorYellow, s.currentNE.Ne, colorReset)
	} else {
		s.promptStr = fmt.Sprintf("%s%s%s> ", colorCyan, s.username, colorReset)
	}
	s.term.SetPrompt(s.promptStr)
}

func (s *session) welcome() {
	s.writef("%s", colorGreen)
	s.writef("============================================\n")
	s.writef("        CLI - NETCONF Console\n")
	s.writef("============================================%s\n\n", colorReset)
}

// run is the main command loop for a connected (or recently disconnected) session.
// Returns true if the SSH session should end, false if control should return to
// the NE selection screen (user typed "exit" while connected, or "disconnect").
func (s *session) run() bool {
	consecutiveErrors := 0
	for {
		line, err := s.term.ReadLine()
		if err != nil {
			consecutiveErrors++
			if consecutiveErrors >= 3 {
				// Real EOF / SSH connection dropped — end the session.
				s.cmdDisconnect()
				return true
			}
			// Ctrl+C — cancel current input and redisplay the prompt.
			s.writef("\n")
			continue
		}
		consecutiveErrors = 0

		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		parts := strings.Fields(line)
		cmd := strings.ToLower(parts[0])
		args := parts[1:]

		ne := ""
		if s.currentNE != nil {
			ne = s.currentNE.Ne
		}
		slog.Info("user command", "user", s.username, "ne", ne, "cmd", cmd, "args", args)

		switch cmd {
		case "show":
			s.cmdShow(args)
		case "connect":
			s.cmdConnect(args)
		case "disconnect":
			// Disconnect and return to NE selection.
			s.cmdDisconnect()
			return false
		case "set":
			s.cmdSet(args)
		case "unset":
			s.cmdUnset(args)
		case "commit":
			s.cmdCommit()
		case "validate":
			s.cmdValidate()
		case "discard":
			s.cmdDiscard()
		case "lock":
			s.cmdLock(args)
		case "unlock":
			s.cmdUnlock(args)
		case "dump":
			s.cmdDump(args)
		case "rpc":
			s.cmdRPC()
		case "restore":
			s.cmdRestore(args)
		case "help":
			s.cmdHelp()
		case "exit", "quit":
			if s.nc != nil {
				// Connected: disconnect and go back to NE selection.
				s.cmdDisconnect()
				return false
			}
			// Not connected: exit the SSH session entirely.
			return true
		default:
			s.writef("%sUnknown command: %s%s\n", colorRed, cmd, colorReset)
			s.writef("Type 'help' for available commands.\n")
		}
	}
}

// --- Helpers ---

func (s *session) writef(format string, args ...any) {
	fmt.Fprintf(s.term, format, args...)
}

func (s *session) readMultiline() string {
	s.term.SetPrompt(fmt.Sprintf("%s...%s ", colorDim, colorReset))
	defer s.updatePrompt()

	var lines []string
	for {
		line, err := s.term.ReadLine()
		if err != nil {
			return ""
		}
		if line == "." {
			break
		}
		lines = append(lines, line)
	}
	return strings.Join(lines, "\n")
}

func (s *session) saveHistory(cmd string, elapsed time.Duration) {
	if s.currentNE == nil || s.api == nil {
		return
	}
	go s.api.SaveHistory(s.token, &api.HistorySaveRequest{
		CmdName:        cmd,
		NeName:         s.currentNE.Ne,
		NeIP:           s.currentNE.IP,
		InputType:      "cli",
		Result:         "success",
		TimeToComplete: elapsed.Milliseconds(),
	})
}

func (s *session) requireConnection() bool {
	if s.nc == nil {
		s.writef("%sNot connected to any NE. Use 'connect <n>'.%s\n", colorRed, colorReset)
		return false
	}
	return true
}

// writePaged writes text to the terminal one page at a time.
// After each page a <MORE> prompt is shown at the bottom of the screen.
// Controls: Enter/Space = next page  a/G/End/PageDown = show all  q/Esc/Ctrl-C = quit
const pageSize = 20

type moreAction int

const (
	moreNext moreAction = iota
	moreAll
	moreQuit
)

func (s *session) writePaged(text string) {
	lines := strings.Split(strings.TrimRight(text, "\n"), "\n")
	total := len(lines)

	for i := 0; i < total; {
		end := i + pageSize
		if end > total {
			end = total
		}
		for _, ln := range lines[i:end] {
			fmt.Fprintf(s.term, "%s\n", ln)
		}
		i = end
		if i >= total {
			break
		}

		remaining := total - i
		prompt := fmt.Sprintf("%s<MORE>%s — %d lines left  [Enter] next  [a/G/End/PgDn] all  [q] quit ",
			colorYellow, colorReset, remaining)

		// Write MORE prompt directly (no newline — stays on same line at bottom)
		if s.sshConn != nil {
			s.sshConn.Write([]byte(prompt))
		}

		action := s.readMoreKey()

		// Erase the <MORE> line entirely before continuing
		if s.sshConn != nil {
			s.sshConn.Write([]byte("\r\033[2K"))
		}

		switch action {
		case moreQuit:
			return
		case moreAll:
			for _, ln := range lines[i:] {
				fmt.Fprintf(s.term, "%s\n", ln)
			}
			return
		// moreNext: fall through to loop
		}
	}
}

// readMoreKey reads a single raw keypress from the underlying reader and
// interprets it as a pager action. It works outside of term.Terminal so
// keystrokes are not echoed and escape sequences can be parsed directly.
func (s *session) readMoreKey() moreAction {
	if s.rawReader == nil {
		return moreQuit
	}
	buf := make([]byte, 16)
	n, _ := s.rawReader.Read(buf)
	if n == 0 {
		return moreQuit
	}
	b := buf[:n]

	// ESC sequence: End (\033[F or \033[4~) and PageDown (\033[6~) → show all
	if b[0] == 0x1b {
		if n >= 3 && b[1] == '[' {
			switch b[2] {
			case 'F': // End key
				return moreAll
			case '4':
				if n >= 4 && b[3] == '~' { // End key (alternate)
					return moreAll
				}
			case '6':
				if n >= 4 && b[3] == '~' { // PageDown
					return moreAll
				}
			case 'A', 'B': // Up/Down arrow — treat as next page
				return moreNext
			}
		}
		// ESC alone or unrecognized sequence → quit
		return moreQuit
	}

	switch b[0] {
	case 'q', 'Q', 3: // q, Q, Ctrl-C
		return moreQuit
	case 'a', 'A', 'G', 'g': // show all remaining
		return moreAll
	}

	// Enter (\r or \n), Space, or any other key → next page
	return moreNext
}

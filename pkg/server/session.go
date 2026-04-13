package server

import (
	"fmt"
	"io"
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

type session struct {
	term      *term.Terminal
	sshConn   io.Writer // raw SSH channel for completion display
	api       *api.Client
	cfg       *config.Config
	token     string
	username  string
	promptStr string
	neList    []api.NeDataItem
	currentNE *api.NeDataItem
	nc        *netconf.Client
	schema    *schemaNode // YANG schema tree for tab completion
}

func handleSession(s gssh.Session, apiClient *api.Client, cfg *config.Config) {
	token, _ := s.Context().Value(keyToken).(string)

	sess := &session{
		term:     term.NewTerminal(s, ""),
		sshConn:  s,
		api:      apiClient,
		cfg:      cfg,
		token:    token,
		username: s.User(),
	}
	sess.updatePrompt()
	sess.term.AutoCompleteCallback = sess.handleComplete
	sess.welcome()
	sess.autoSelectNE()
	sess.run()
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

func (s *session) run() {
	for {
		line, err := s.term.ReadLine()
		if err != nil {
			break
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		parts := strings.Fields(line)
		cmd := strings.ToLower(parts[0])
		args := parts[1:]

		switch cmd {
		case "show":
			s.cmdShow(args)
		case "connect":
			s.cmdConnect(args)
		case "disconnect":
			s.cmdDisconnect()
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
		case "help":
			s.cmdHelp()
		case "exit", "quit":
			s.cmdDisconnect()
			s.writef("Goodbye.\n")
			return
		default:
			s.writef("%sUnknown command: %s%s\n", colorRed, cmd, colorReset)
			s.writef("Type 'help' for available commands.\n")
		}
	}
	s.cmdDisconnect()
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

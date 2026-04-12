package server

import (
	"context"
	"fmt"
	"log/slog"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"

	"github.com/DoTuanAnh2k1/cli-netconf/internal/netconf"
)

// --- auto select NE on login ---

func (s *session) autoSelectNE() {
	resp, err := s.api.ListNE(s.token)
	if err != nil {
		s.writef("%sCould not load NE list: %s%s\n", colorRed, err, colorReset)
		return
	}
	s.neList = resp.NeDataList
	if len(s.neList) == 0 {
		s.writef("No network elements available.\n")
		return
	}

	s.displayNETable()
	s.writef("\n")

	s.term.SetPrompt(fmt.Sprintf("Select NE [1-%d or name]: ", len(s.neList)))
	for {
		line, err := s.term.ReadLine()
		if err != nil {
			return
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		idx := s.resolveNE(line)
		if idx < 0 {
			s.writef("%sNE '%s' not found. Enter 1-%d or NE name.%s\n",
				colorRed, line, len(s.neList), colorReset)
			continue
		}
		s.doConnect(idx)
		break
	}
	s.updatePrompt()
}

// resolveNE returns 1-based index for a number or NE name, or -1 if not found.
func (s *session) resolveNE(input string) int {
	// Try number
	if idx, err := strconv.Atoi(input); err == nil && idx >= 1 && idx <= len(s.neList) {
		return idx
	}
	// Try name (case-insensitive)
	for i, ne := range s.neList {
		if strings.EqualFold(ne.Ne, input) {
			return i + 1
		}
	}
	return -1
}

// --- show ---

func (s *session) cmdShow(args []string) {
	if len(args) == 0 {
		s.writef("Usage: show ne | running-config | candidate-config\n")
		return
	}
	switch strings.ToLower(args[0]) {
	case "ne":
		s.cmdShowNE()
	case "running-config":
		s.cmdShowConfig("running", args[1:])
	case "candidate-config":
		s.cmdShowConfig("candidate", args[1:])
	default:
		s.writef("Unknown: show %s\n", args[0])
	}
}

func (s *session) displayNETable() {
	tw := tabwriter.NewWriter(s.term, 0, 0, 2, ' ', 0)
	fmt.Fprintf(tw, "%s  #\tNE\tSite\tIP\tPort\tNamespace\tDescription%s\n",
		colorBold, colorReset)
	for i, ne := range s.neList {
		fmt.Fprintf(tw, "  %d\t%s\t%s\t%s\t%d\t%s\t%s\n",
			i+1, ne.Ne, ne.Site, ne.IP, ne.Port, ne.Namespace, ne.Description)
	}
	tw.Flush()
}

func (s *session) cmdShowNE() {
	resp, err := s.api.ListNE(s.token)
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}
	s.neList = resp.NeDataList
	if len(s.neList) == 0 {
		s.writef("No network elements available.\n")
		return
	}
	s.displayNETable()
	s.writef("\nUse '%sconnect <number|name>%s' to connect to an NE.\n", colorYellow, colorReset)
}

func (s *session) cmdShowConfig(source string, path []string) {
	if !s.requireConnection() {
		return
	}

	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	reply, err := s.nc.GetConfig(ctx, source, "")
	elapsed := time.Since(start)
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	output := formatXMLResponse(reply, path)
	s.writef("%s", output)
	s.writef("%s(%s)%s\n", colorDim, elapsed.Round(time.Millisecond), colorReset)
	s.saveHistory("show "+source+"-config", elapsed)
}

// --- connect / disconnect ---

func (s *session) cmdConnect(args []string) {
	if len(args) < 1 {
		s.writef("Usage: connect <number|name>\n")
		return
	}
	if s.nc != nil {
		s.writef("Already connected to %s. Use 'disconnect' first.\n", s.currentNE.Ne)
		return
	}
	if len(s.neList) == 0 {
		s.writef("No NE list loaded. Run '%sshow ne%s' first.\n", colorYellow, colorReset)
		return
	}

	input := strings.Join(args, " ")
	idx := s.resolveNE(input)
	if idx < 0 {
		s.writef("%sNE '%s' not found. Use 'show ne' to list available NEs.%s\n",
			colorRed, input, colorReset)
		return
	}
	s.doConnect(idx)
}

func (s *session) doConnect(idx int) {
	ne := s.neList[idx-1]
	s.writef("Connecting to %s (%s:%d)...\n", ne.Ne, ne.IP, ne.Port)

	ctx, cancel := context.WithTimeout(context.Background(), s.cfg.NetconfTimeout)
	defer cancel()

	nc, err := netconf.Dial(ctx, ne.IP, ne.Port, s.cfg.NetconfUser, s.cfg.NetconfPass)
	if err != nil {
		s.writef("%sConnection failed: %s%s\n", colorRed, err, colorReset)
		return
	}

	s.nc = nc
	s.currentNE = &ne
	s.updatePrompt()
	s.writef("%sConnected.%s NETCONF session ID: %s\n", colorGreen, colorReset, nc.SessionID)
	s.loadSchema()
}

func (s *session) cmdDisconnect() {
	if s.nc == nil {
		return
	}
	name := s.currentNE.Ne
	s.nc.Close()
	s.nc = nil
	s.currentNE = nil
	s.schema = nil
	s.updatePrompt()
	s.writef("Disconnected from %s.\n", name)
}

func (s *session) loadSchema() {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	reply, err := s.nc.GetConfig(ctx, "running", "")
	if err != nil {
		slog.Warn("load schema failed", "error", err)
		return
	}
	s.schema = parseSchemaFromXML(reply)
	if s.schema != nil {
		slog.Info("schema loaded", "top_elements", s.schema.childNames())
	}
}

// --- help ---

func (s *session) cmdHelp() {
	s.writef(`%sGeneral commands:%s
  show ne                            List accessible network elements
  connect <number|name>              Connect to NE
  disconnect                         Close current NETCONF session
  help                               Show this help
  exit                               Disconnect and exit

%sConfiguration commands (requires connection):%s
  show running-config [path...]      Show running configuration
  show candidate-config [path...]    Show candidate configuration
  set                                Set candidate config (XML input)
  commit                             Commit candidate to running
  validate                           Validate candidate configuration
  discard                            Discard candidate changes
  lock [datastore]                   Lock datastore (default: candidate)
  unlock [datastore]                 Unlock datastore (default: candidate)
  rpc                                Send raw NETCONF RPC XML

%sExamples:%s
  show running-config
  show running-config system
  show running-config system ntp server
  show running-config interfaces interface

%sTab completion:%s press Tab to auto-complete commands and config paths.
%sMultiline input:%s end with '.' on its own line.
`, colorBold, colorReset, colorBold, colorReset,
		colorBold, colorReset, colorDim, colorReset, colorDim, colorReset)
}

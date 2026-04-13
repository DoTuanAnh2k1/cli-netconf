package server

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"text/tabwriter"
	"time"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/api"
	"github.com/DoTuanAnh2k1/cli-netconf/pkg/netconf"
)

// --- auto select NE on login ---

// autoSelectNE shows the NE list and waits for the user to select one.
// Returns true if an NE was successfully connected, false if the user typed
// "exit" or the connection dropped (caller should end the SSH session).
func (s *session) autoSelectNE() bool {
	resp, err := s.api.ListNE(s.token)
	if err != nil {
		s.writef("%sCould not load NE list: %s%s\n", colorRed, err, colorReset)
		return false
	}
	s.neList = resp.NeDataList
	if len(s.neList) == 0 {
		s.writef("No network elements available.\n")
		return false
	}

	// Load per-NE NETCONF connection configs (IP, port, username, password).
	// Non-critical: fall back to config-file credentials if unavailable.
	s.loadNeConfigs()

	s.displayNETable()
	s.writef("\n")

	s.term.SetPrompt(fmt.Sprintf("Select NE [1-%d or name] (exit to quit): ", len(s.neList)))
	defer s.updatePrompt()

	for {
		line, err := s.term.ReadLine()
		if err != nil {
			// Ctrl+C — cancel current input, redisplay the prompt.
			s.writef("\n")
			continue
		}
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if strings.ToLower(line) == "exit" {
			return false
		}

		idx := s.resolveNE(line)
		if idx < 0 {
			s.writef("%sNE '%s' not found. Enter 1-%d or NE name.%s\n",
				colorRed, line, len(s.neList), colorReset)
			continue
		}
		s.doConnect(idx)
		return true
	}
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
	case "backups":
		s.cmdShowBackups()
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
	s.writePaged(output)
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

// neConfigKeys returns the keys of the neConfigs map for logging.
func neConfigKeys(m map[string]*api.CliNeConfig) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	return keys
}

// loadNeConfigs fetches per-NE NETCONF connection configs from mgt-service
// and caches them in s.neConfigs keyed by NE name (trimmed, case-insensitive key).
// Non-critical — falls back to config-file credentials if unavailable.
func (s *session) loadNeConfigs() {
	groups, err := s.api.ListNeConfig(s.token)
	if err != nil {
		slog.Info("could not load NE configs from mgt-service, using config-file credentials", "error", err)
		return
	}

	m := make(map[string]*api.CliNeConfig, len(groups))
	for i := range groups {
		g := &groups[i]
		neName := strings.TrimSpace(g.NeName)
		if neName == "" || len(g.ConfigList) == 0 {
			continue
		}

		// Prefer NETCONF protocol; if none found, fall back to the first entry.
		var chosen *api.CliNeConfig
		for j := range g.ConfigList {
			if strings.EqualFold(strings.TrimSpace(g.ConfigList[j].Protocol), "NETCONF") {
				c := g.ConfigList[j]
				chosen = &c
				break
			}
		}
		if chosen == nil {
			c := g.ConfigList[0]
			chosen = &c
			slog.Info("NE config: no NETCONF entry, using first entry",
				"ne_name", neName, "protocol", chosen.Protocol)
		}

		// Store under lowercase key for case-insensitive lookup.
		m[strings.ToLower(neName)] = chosen
		slog.Info("NE config cached",
			"ne_name", neName,
			"ip_address", chosen.IPAddress,
			"port", chosen.Port,
			"username", chosen.Username,
			"protocol", chosen.Protocol,
		)
	}
	s.neConfigs = m
	slog.Info("NE configs loaded", "ne_count", len(m))
}

func (s *session) doConnect(idx int) {
	ne := s.neList[idx-1]

	// Resolve connection parameters: prefer per-NE config from mgt-service,
	// fall back to the NE list IP/port and config-file credentials.
	host := ne.IP
	port := ne.Port
	user := s.cfg.NetconfUser
	pass := s.cfg.NetconfPass

	// Lookup is case-insensitive to handle ne_name vs ne field casing differences.
	if cfg, ok := s.neConfigs[strings.ToLower(strings.TrimSpace(ne.Ne))]; ok {
		slog.Info("using mgt-service NE config",
			"ne", ne.Ne,
			"ip_address", cfg.IPAddress,
			"port", cfg.Port,
			"username", cfg.Username,
			"protocol", cfg.Protocol,
		)
		if cfg.IPAddress != "" {
			host = cfg.IPAddress
		}
		if cfg.Port != 0 {
			port = cfg.Port
		}
		if cfg.Username != "" {
			user = cfg.Username
		}
		if cfg.Password != "" {
			pass = cfg.Password
		}
	} else {
		slog.Info("no mgt-service NE config found, using fallback",
			"ne", ne.Ne,
			"fallback_ip", host,
			"fallback_port", port,
			"known_ne_names", neConfigKeys(s.neConfigs),
		)
	}

	s.writef("Connecting to %s (%s:%d)...\n", ne.Ne, host, port)

	ctx, cancel := context.WithTimeout(context.Background(), s.cfg.NetconfTimeout)
	defer cancel()

	nc, err := netconf.Dial(ctx, host, port, user, pass)
	if err != nil {
		s.writef("%sConnection failed: %s%s\n", colorRed, err, colorReset)
		return
	}

	s.nc = nc
	s.currentNE = &ne
	s.updatePrompt()
	s.writef("%sConnected.%s NETCONF session ID: %s\n", colorGreen, colorReset, nc.SessionID)
	s.loadSchema()
	go s.loadBackupsFromAPI()
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
	s.backups = nil
	s.backupSeq = 0
	s.updatePrompt()
	s.writef("Disconnected from %s.\n", name)
}

// loadSchemaFromDir loads all .yang files from the configured yang_dir and
// merges them into the schema. This provides correct deep structure for modules
// that the NE doesn't advertise via get-schema (e.g. proprietary ConfD modules).
func (s *session) loadSchemaFromDir() {
	dir := s.cfg.YangDir
	if dir == "" {
		return
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		if !os.IsNotExist(err) {
			slog.Warn("schema: cannot read yang_dir", "dir", dir, "error", err)
		}
		return
	}
	loaded := 0
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".yang") {
			continue
		}
		path := filepath.Join(dir, e.Name())
		data, err := os.ReadFile(path)
		if err != nil {
			slog.Warn("schema: cannot read yang file", "file", path, "error", err)
			continue
		}
		parsed := parseSchemaFromYANG(string(data), "")
		mergeSchema(s.schema, parsed)
		slog.Info("schema: loaded from local file", "file", e.Name(), "top_elements", parsed.childNames())
		loaded++
	}
	if loaded > 0 {
		slog.Info("schema: local YANG files loaded", "count", loaded, "yang_dir", dir)
	}
}

func (s *session) loadSchema() {
	s.schema = newSchemaNode()

	// 0. Load local .yang files first — gives correct structure for modules
	//    the NE doesn't expose via get-schema (e.g. proprietary ConfD modules).
	s.loadSchemaFromDir()

	// 1. Try loading YANG modules via get-schema (RFC 6022)
	modules := s.nc.ExtractModules()
	slog.Info("schema: discovered YANG modules from capabilities", "count", len(modules), "modules", modules)

	for _, mod := range modules {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		reply, err := s.nc.GetSchema(ctx, mod)
		cancel()
		if err != nil {
			slog.Warn("schema: get-schema failed", "module", mod, "error", err)
			continue
		}
		yangText := extractSchemaText(reply)
		if yangText == "" {
			slog.Warn("schema: empty YANG text", "module", mod)
			continue
		}
		ns := findNamespace(s.nc.Capabilities, mod)
		parsed := parseSchemaFromYANG(yangText, ns)
		mergeSchema(s.schema, parsed)
		slog.Info("schema: YANG loaded", "module", mod, "top_elements", parsed.childNames())
	}

	slog.Info("schema: YANG phase complete", "yang_top_elements", s.schema.childNames())

	// 2. Merge with running config to capture runtime-only nodes.
	//
	// When YANG loaded successfully (schema has children), we only add top-level
	// containers that YANG didn't describe.  This prevents the XML config from
	// corrupting the YANG-derived deep structure: ConfD may omit intermediate
	// containers (e.g. "vsmf") from the XML when none of their direct leaves are
	// explicitly set, which would cause parseSchemaFromXML to place grandchildren
	// (e.g. "qosConf") one level too high relative to the YANG schema.
	//
	// When YANG loading failed entirely, fall back to full XML-based schema.
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	reply, err := s.nc.GetConfig(ctx, "running", "")
	if err == nil {
		configSchema := parseSchemaFromXML(reply)
		slog.Info("schema: XML running-config top_elements", "elements", configSchema.childNames())
		if len(s.schema.children) == 0 {
			// No YANG available — use XML as the sole schema source.
			slog.Warn("schema: no YANG loaded, falling back to XML-only schema (tab completion may show wrong structure for ConfD-omitted intermediate containers)")
			mergeSchema(s.schema, configSchema)
		} else {
			// YANG is loaded — only add top-level containers not already known.
			var added, skipped []string
			for name, xmlNode := range configSchema.children {
				if _, exists := s.schema.children[name]; !exists {
					s.schema.children[name] = xmlNode
					added = append(added, name)
				} else {
					skipped = append(skipped, name)
				}
			}
			slog.Info("schema: XML merge complete", "added_from_xml", added, "kept_from_yang", skipped)
		}
	} else {
		slog.Warn("schema: get-config failed, using YANG-only schema", "error", err)
	}

	slog.Info("schema ready", "top_elements", s.schema.childNames())
}

// extractSchemaText extracts YANG text from get-schema rpc-reply
func extractSchemaText(reply string) string {
	// <data xmlns="...">YANG TEXT</data>
	start := strings.Index(reply, "<data")
	if start < 0 {
		return ""
	}
	gt := strings.Index(reply[start:], ">")
	if gt < 0 {
		return ""
	}
	contentStart := start + gt + 1
	end := strings.LastIndex(reply, "</data>")
	if end <= contentStart {
		return ""
	}
	text := reply[contentStart:end]
	// Unescape XML entities
	text = strings.ReplaceAll(text, "&amp;", "&")
	text = strings.ReplaceAll(text, "&lt;", "<")
	text = strings.ReplaceAll(text, "&gt;", ">")
	text = strings.ReplaceAll(text, "&quot;", "\"")
	return text
}

// findNamespace finds namespace for a module from capabilities
func findNamespace(caps []string, module string) string {
	for _, cap := range caps {
		if strings.Contains(cap, "module="+module) {
			if idx := strings.Index(cap, "?"); idx > 0 {
				return cap[:idx]
			}
		}
	}
	return ""
}

// mergeSchema merges src into dst recursively
func mergeSchema(dst, src *schemaNode) {
	for name, srcChild := range src.children {
		dstChild, exists := dst.children[name]
		if !exists {
			dst.children[name] = srcChild
		} else {
			if srcChild.namespace != "" && dstChild.namespace == "" {
				dstChild.namespace = srcChild.namespace
			}
			mergeSchema(dstChild, srcChild)
		}
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
  set                                Set config (paste XML or text format)
  set <path...> <value>              Set a config value by path
  unset <path...>                    Delete a config node
  commit                             Commit candidate to running
  validate                           Validate candidate configuration
  discard                            Discard candidate changes
  lock [datastore]                   Lock datastore (default: candidate)
  unlock [datastore]                 Unlock datastore (default: candidate)
  dump text [filename]               Dump running config as text
  dump xml  [filename]               Dump running config as XML
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

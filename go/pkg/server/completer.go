package server

import (
	"encoding/xml"
	"fmt"
	"sort"
	"strings"
)

// ---------------------------------------------------------------------------
// Schema tree — built from NETCONF get-config XML response
// ---------------------------------------------------------------------------

type schemaNode struct {
	children  map[string]*schemaNode
	namespace string // XML namespace (captured on top-level containers)
}

func newSchemaNode() *schemaNode {
	return &schemaNode{children: make(map[string]*schemaNode), namespace: ""}
}

func (n *schemaNode) isContainer() bool {
	return len(n.children) > 0
}

func (n *schemaNode) childNames() []string {
	names := make([]string, 0, len(n.children))
	for name := range n.children {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// containerChildNames returns only children that have sub-children (containers/lists).
// Used for path navigation commands like "show" where leaf nodes are not useful mid-path.
func (n *schemaNode) containerChildNames() []string {
	names := make([]string, 0, len(n.children))
	for name, child := range n.children {
		if child.isContainer() {
			names = append(names, name)
		}
	}
	sort.Strings(names)
	return names
}

func (n *schemaNode) lookup(path []string) *schemaNode {
	node := n
	for _, p := range path {
		if p == "" {
			continue
		}
		child, ok := node.children[p]
		if !ok {
			return nil
		}
		node = child
	}
	return node
}

// parseSchemaFromYANG parses a YANG module text and builds a schema tree.
// Handles: container, list, leaf, leaf-list, grouping, uses.
// Two-pass: first collect all grouping definitions, then build the tree
// expanding `uses` statements inline.
func parseSchemaFromYANG(yangText string, ns string) *schemaNode {
	lines := strings.Split(yangText, "\n")
	groupings := collectGroupings(lines)
	return buildSchemaTree(lines, groupings, ns)
}

// yangNodeName extracts the node name from a YANG statement line.
// e.g. "container ntp {" with prefix "container " → "ntp"
func yangNodeName(line, prefix string) string {
	s := line[len(prefix):]
	end := strings.IndexAny(s, " {;")
	if end >= 0 {
		s = s[:end]
	}
	return strings.TrimSpace(s)
}

// isYANGMetadata returns true for YANG keywords that carry no schema-tree
// meaning (they do not create navigable nodes).
func isYANGMetadata(line string) bool {
	for _, kw := range []string{
		"namespace ", "prefix ", "key ", "type ", "revision ",
		"organization ", "description", "contact ", "default ",
		"range ", "length ", "ordered-by ", "nullable ", "enum ",
		"format ", "when ", "must ", "pattern ", "status ",
		"units ", "reference ", "if-feature ", "min-elements ",
		"max-elements ", "presence ", "config ", "fraction-digits ",
		"import ", "include ", "belongs-to ", "deviation ",
		"identity ", "extension ", "anyxml ", "anydata ",
		"rpc ", "notification ", "action ", "input ", "output ",
	} {
		if strings.HasPrefix(line, kw) {
			return true
		}
	}
	return false
}

// collectGroupings does a first pass extracting all `grouping` blocks.
// Returns a map of grouping-name → schema subtree.
//
// Two-pass approach:
//  1. Collect raw body text for every grouping (in document order).
//  2. Parse each body — passing already-parsed groupings so that `uses`
//     statements inside a grouping body are resolved immediately.
//     This handles patterns like "grouping outer { uses inner; … }".
func collectGroupings(lines []string) map[string]*schemaNode {
	// Pass 1: collect body text in document order.
	type entry struct {
		name string
		body string
	}
	var entries []entry

	i := 0
	for i < len(lines) {
		line := strings.TrimSpace(lines[i])
		i++
		if !strings.HasPrefix(line, "grouping ") {
			continue
		}
		name := yangNodeName(line, "grouping ")
		depth := strings.Count(line, "{") - strings.Count(line, "}")
		var innerLines []string
		for i < len(lines) && depth > 0 {
			inner := lines[i]
			i++
			trimmed := strings.TrimSpace(inner)
			depth += strings.Count(trimmed, "{") - strings.Count(trimmed, "}")
			if depth > 0 {
				innerLines = append(innerLines, inner)
			}
		}
		entries = append(entries, entry{name, strings.Join(innerLines, "\n")})
	}

	// Pass 2: parse each body, passing the already-built map so nested
	// `uses` can be resolved immediately (YANG convention: define before use).
	groupings := make(map[string]*schemaNode, len(entries))
	for _, e := range entries {
		groupings[e.name] = parseGroupingBody(e.body, groupings)
	}
	return groupings
}

// parseGroupingBody builds a schema node from the interior lines of a
// grouping block. The groupings map is used to resolve any `uses` statements
// found inside the body (e.g. when one grouping references another).
func parseGroupingBody(text string, groupings map[string]*schemaNode) *schemaNode {
	root := newSchemaNode()
	var stack []*schemaNode
	stack = append(stack, root)
	metaDepth := 0

	for _, raw := range strings.Split(text, "\n") {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "//") {
			continue
		}
		opens := strings.Count(line, "{")
		closes := strings.Count(line, "}")

		if metaDepth > 0 {
			metaDepth += opens - closes
			continue
		}
		if line == "}" {
			if len(stack) > 1 {
				stack = stack[:len(stack)-1]
			}
			continue
		}
		if isYANGMetadata(line) {
			if opens > closes {
				metaDepth += opens - closes
			}
			continue
		}
		// Resolve `uses` within the grouping body.
		// Strip module prefix if present: "uses smf:vsmf-grp" → look up "vsmf-grp".
		if strings.HasPrefix(line, "uses ") {
			gName := yangNodeName(line, "uses ")
			if colon := strings.LastIndex(gName, ":"); colon >= 0 {
				gName = gName[colon+1:]
			}
			if g, ok := groupings[gName]; ok {
				mergeSchema(stack[len(stack)-1], g)
			}
			continue
		}
		for _, kw := range []string{"container ", "list ", "leaf-list ", "leaf "} {
			if strings.HasPrefix(line, kw) {
				name := yangNodeName(line, kw)
				parent := stack[len(stack)-1]
				child, exists := parent.children[name]
				if !exists {
					child = newSchemaNode()
					parent.children[name] = child
				}
				if strings.HasSuffix(line, "{") {
					stack = append(stack, child)
				}
				break
			}
		}
	}
	return root
}

// buildSchemaTree does the second pass: builds the schema tree while
// resolving `uses` references and skipping `grouping` blocks.
func buildSchemaTree(lines []string, groupings map[string]*schemaNode, ns string) *schemaNode {
	root := newSchemaNode()
	var stack []*schemaNode
	stack = append(stack, root)

	metaDepth := 0    // depth inside metadata blocks (type enumeration {}, etc.)
	groupingDepth := 0 // depth inside grouping blocks (skip in pass 2)
	inGrouping := false

	for _, raw := range lines {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "//") {
			continue
		}
		opens := strings.Count(line, "{")
		closes := strings.Count(line, "}")

		// Skip grouping block bodies (already collected in pass 1)
		if inGrouping {
			groupingDepth += opens - closes
			if groupingDepth <= 0 {
				inGrouping = false
				groupingDepth = 0
			}
			continue
		}

		// Skip metadata block bodies (type enumeration { enum ...; })
		if metaDepth > 0 {
			metaDepth += opens - closes
			continue
		}

		// module X { — transparent wrapper
		if strings.HasPrefix(line, "module ") {
			if strings.HasSuffix(line, "{") {
				stack = append(stack, root)
			}
			continue
		}

		// grouping foo { — skip entire block
		if strings.HasPrefix(line, "grouping ") {
			inGrouping = true
			groupingDepth = opens - closes
			continue
		}

		// uses groupingName — expand inline.
		// Strip module prefix: "uses smf:vsmf-grp" → look up "vsmf-grp".
		if strings.HasPrefix(line, "uses ") {
			name := yangNodeName(line, "uses ")
			if colon := strings.LastIndex(name, ":"); colon >= 0 {
				name = name[colon+1:]
			}
			if g, ok := groupings[name]; ok {
				parent := stack[len(stack)-1]
				mergeSchema(parent, g)
			}
			continue
		}

		// augment "/some:path/other:child" { — navigate to target node and
		// build the augmented content into it.  Strip module prefixes from
		// each path segment so we can match against the bare node names we
		// already have in the schema tree.
		if strings.HasPrefix(line, "augment ") {
			rawPath := yangNodeName(line, "augment ")
			rawPath = strings.Trim(rawPath, `"'`)
			segments := strings.Split(rawPath, "/")
			target := root
			for _, seg := range segments {
				if seg == "" {
					continue
				}
				if colon := strings.LastIndex(seg, ":"); colon >= 0 {
					seg = seg[colon+1:]
				}
				child, ok := target.children[seg]
				if !ok {
					child = newSchemaNode()
					target.children[seg] = child
				}
				target = child
			}
			if strings.HasSuffix(line, "{") {
				stack = append(stack, target)
			}
			continue
		}

		if line == "}" {
			if len(stack) > 1 {
				stack = stack[:len(stack)-1]
			}
			continue
		}

		// Metadata keywords — skip, but track depth if they open a block
		if isYANGMetadata(line) {
			if opens > closes {
				metaDepth += opens - closes
			}
			continue
		}

		for _, kw := range []string{"container ", "list ", "leaf-list ", "leaf "} {
			if strings.HasPrefix(line, kw) {
				name := yangNodeName(line, kw)
				parent := stack[len(stack)-1]
				child, exists := parent.children[name]
				if !exists {
					child = newSchemaNode()
					parent.children[name] = child
				}
				// Track namespace on top-level containers
				if parent == root && ns != "" {
					child.namespace = ns
				}
				if strings.HasSuffix(line, "{") {
					stack = append(stack, child)
				}
				break
			}
		}
	}
	return root
}

// parseSchemaFromXML extracts the element hierarchy from a get-config rpc-reply.
func parseSchemaFromXML(xmlData string) *schemaNode {
	root := newSchemaNode()
	decoder := xml.NewDecoder(strings.NewReader(xmlData))

	var stack []*schemaNode
	inData := false

	for {
		token, err := decoder.Token()
		if err != nil {
			break
		}

		switch t := token.(type) {
		case xml.StartElement:
			name := t.Name.Local
			if name == "rpc-reply" {
				continue
			}
			if name == "data" {
				inData = true
				stack = []*schemaNode{root}
				continue
			}
			if !inData || len(stack) == 0 {
				continue
			}
			parent := stack[len(stack)-1]
			child, exists := parent.children[name]
			if !exists {
				child = newSchemaNode()
				parent.children[name] = child
			}
			// Capture namespace on top-level containers (direct children of root)
			if len(stack) == 1 && t.Name.Space != "" {
				child.namespace = t.Name.Space
			}
			stack = append(stack, child)

		case xml.EndElement:
			name := t.Name.Local
			if name == "data" {
				inData = false
				continue
			}
			if name == "rpc-reply" {
				continue
			}
			if inData && len(stack) > 1 {
				stack = stack[:len(stack)-1]
			}
		}
	}
	return root
}

// ---------------------------------------------------------------------------
// Tab completion handler
// ---------------------------------------------------------------------------

var commandsBase = []string{
	"connect", "exit", "help", "show",
}

var commandsConnected = []string{
	"commit", "connect", "discard", "disconnect",
	"dump", "exit", "help", "lock", "restore", "rpc",
	"set", "show", "unlock", "unset", "validate",
}

var showSubcommands = []string{"backups", "candidate-config", "ne", "running-config"}

func (s *session) handleComplete(line string, pos int, key rune) (string, int, bool) {
	if key != '\t' {
		return "", 0, false
	}
	if pos != len(line) {
		return "", 0, false
	}

	parts := strings.Fields(line)
	trailing := len(line) == 0 || line[len(line)-1] == ' '

	var completions []string
	var prefix string

	if len(parts) == 0 || (len(parts) == 1 && !trailing) {
		word := ""
		if len(parts) == 1 {
			word = parts[0]
		}
		prefix = ""
		completions = s.completeCommand(word)
	} else {
		cmd := strings.ToLower(parts[0])
		args := parts[1:]
		if trailing {
			args = append(args, "")
		}
		word := args[len(args)-1]
		prefix = line[:len(line)-len(word)]
		completions = s.completeArgs(cmd, args, word)
	}

	if len(completions) == 0 {
		return "", 0, false
	}

	// --- Single match ---
	if len(completions) == 1 {
		newLine := prefix + completions[0] + " "
		return newLine, len(newLine), true
	}

	// --- Multiple matches: extend to common prefix ---
	common := longestCommonPrefix(completions)
	newLine := prefix + common
	if newLine != line {
		return newLine, len(newLine), true
	}

	// --- Already at common prefix: show options ---
	s.displayCompletions(completions, line)
	return line, pos, true
}

// ---------------------------------------------------------------------------
// Completion generators
// ---------------------------------------------------------------------------

func (s *session) completeCommand(word string) []string {
	cmds := commandsBase
	if s.nc != nil {
		cmds = commandsConnected
	}
	return filterByPrefix(cmds, word)
}

func (s *session) completeArgs(cmd string, args []string, word string) []string {
	switch cmd {
	case "show":
		if len(args) == 1 {
			return filterByPrefix(showSubcommands, word)
		}
		sub := strings.ToLower(args[0])
		if sub == "running-config" || sub == "candidate-config" {
			return s.pathCompletions(args[1:], word, false)
		}

	case "connect":
		if len(s.neList) == 0 {
			return nil
		}
		var opts []string
		for i, ne := range s.neList {
			opts = append(opts, fmt.Sprintf("%d", i+1))
			opts = append(opts, ne.Ne)
		}
		return filterByPrefix(opts, word)

	case "set", "unset":
		return s.pathCompletions(args, word, false)

	case "restore":
		var ids []string
		for _, b := range s.backups {
			ids = append(ids, fmt.Sprintf("%d", b.id))
		}
		return filterByPrefix(ids, word)

	case "dump":
		if len(args) == 1 {
			return filterByPrefix([]string{"text", "xml"}, word)
		}

	case "lock", "unlock":
		return filterByPrefix([]string{"candidate", "running"}, word)
	}

	return nil
}

// pathCompletions completes space-separated config paths using the schema tree.
// pathArgs includes the word being typed as the last element.
// containersOnly restricts suggestions to nodes that have children (containers/lists),
// which is appropriate for navigation commands like "show".
func (s *session) pathCompletions(pathArgs []string, word string, containersOnly bool) []string {
	if s.schema == nil {
		return nil
	}

	// Navigate to parent node using all args except the last (being typed)
	parentPath := pathArgs[:len(pathArgs)-1]
	node := s.schema.lookup(parentPath)
	if node == nil {
		return nil
	}

	if containersOnly {
		return filterByPrefix(node.containerChildNames(), word)
	}
	return filterByPrefix(node.childNames(), word)
}

// ---------------------------------------------------------------------------
// Display completions (write directly to SSH channel)
// ---------------------------------------------------------------------------

func (s *session) displayCompletions(completions []string, currentLine string) {
	if s.sshConn == nil {
		return
	}

	maxLen := 0
	for _, c := range completions {
		if len(c) > maxLen {
			maxLen = len(c)
		}
	}

	colWidth := maxLen + 3
	cols := 72 / colWidth
	if cols < 1 {
		cols = 1
	}

	var buf strings.Builder
	buf.WriteString("\r\n")
	for i, c := range completions {
		buf.WriteString(fmt.Sprintf("  %-*s", colWidth-2, c))
		if (i+1)%cols == 0 {
			buf.WriteString("\r\n")
		}
	}
	if len(completions)%cols != 0 {
		buf.WriteString("\r\n")
	}

	// Re-print prompt + line so terminal cursor tracking stays correct
	buf.WriteString(s.promptStr)
	buf.WriteString(currentLine)

	s.sshConn.Write([]byte(buf.String()))
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func filterByPrefix(items []string, prefix string) []string {
	if prefix == "" {
		result := make([]string, len(items))
		copy(result, items)
		return result
	}
	lp := strings.ToLower(prefix)
	var result []string
	for _, item := range items {
		if strings.HasPrefix(strings.ToLower(item), lp) {
			result = append(result, item)
		}
	}
	return result
}

func longestCommonPrefix(strs []string) string {
	if len(strs) == 0 {
		return ""
	}
	p := strs[0]
	for _, s := range strs[1:] {
		for len(p) > 0 && !strings.HasPrefix(s, p) {
			p = p[:len(p)-1]
		}
	}
	return p
}

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

// parseSchemaFromYANG parses a simplified YANG module text and builds a schema tree.
// Handles: container, list, leaf, leaf-list with nesting.
func parseSchemaFromYANG(yangText string, ns string) *schemaNode {
	root := newSchemaNode()
	lines := strings.Split(yangText, "\n")

	var stack []*schemaNode
	stack = append(stack, root)

	for _, raw := range lines {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "//") {
			continue
		}

		// Skip YANG metadata keywords
		skip := false
		for _, kw := range []string{
			"namespace ", "prefix ", "key ", "type ", "revision ",
			"organization ", "description", "default ", "range ",
			"length ", "ordered-by ", "nullable ", "enum ", "format ",
		} {
			if strings.HasPrefix(line, kw) {
				skip = true
				break
			}
		}
		if skip {
			continue
		}

		// module X { — treat as transparent (push root, not a new node)
		if strings.HasPrefix(line, "module ") {
			if strings.HasSuffix(line, "{") {
				stack = append(stack, root)
			}
			continue
		}

		if line == "}" {
			if len(stack) > 1 {
				stack = stack[:len(stack)-1]
			}
			continue
		}

		for _, keyword := range []string{"container ", "list ", "leaf-list ", "leaf "} {
			if strings.HasPrefix(line, keyword) {
				nameEnd := strings.IndexAny(line[len(keyword):], " {;")
				name := line[len(keyword):]
				if nameEnd >= 0 {
					name = line[len(keyword) : len(keyword)+nameEnd]
				}
				name = strings.TrimSpace(name)

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
	"dump", "exit", "help", "lock", "rpc",
	"set", "show", "unlock", "unset", "validate",
}

var showSubcommands = []string{"candidate-config", "ne", "running-config"}

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
			return s.pathCompletions(args[1:], word)
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
		return s.pathCompletions(args, word)

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
func (s *session) pathCompletions(pathArgs []string, word string) []string {
	if s.schema == nil {
		return nil
	}

	// Navigate to parent node using all args except the last (being typed)
	parentPath := pathArgs[:len(pathArgs)-1]
	node := s.schema.lookup(parentPath)
	if node == nil {
		return nil
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

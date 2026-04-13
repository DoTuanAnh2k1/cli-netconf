package server

import (
	"encoding/xml"
	"fmt"
	"strings"
)

// ---------------------------------------------------------------------------
// Generic XML tree
// ---------------------------------------------------------------------------

type xmlNode struct {
	name     string
	value    string
	children []*xmlNode
}

// parseXMLTree parses NETCONF rpc-reply XML into a generic tree.
// It strips the <rpc-reply> and <data> wrappers.
func parseXMLTree(xmlData string) []*xmlNode {
	decoder := xml.NewDecoder(strings.NewReader(xmlData))
	var roots []*xmlNode
	var stack []*xmlNode
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
				continue
			}
			if !inData {
				continue
			}

			node := &xmlNode{name: name}
			if len(stack) > 0 {
				parent := stack[len(stack)-1]
				parent.children = append(parent.children, node)
			} else {
				roots = append(roots, node)
			}
			stack = append(stack, node)

		case xml.CharData:
			text := strings.TrimSpace(string(t))
			if text != "" && len(stack) > 0 {
				stack[len(stack)-1].value = text
			}

		case xml.EndElement:
			name := t.Name.Local
			if name == "rpc-reply" {
				continue
			}
			if name == "data" {
				inData = false
				continue
			}
			if inData && len(stack) > 0 {
				stack = stack[:len(stack)-1]
			}
		}
	}

	return roots
}

// findNode walks the tree following the given path segments.
func findNode(nodes []*xmlNode, path []string) *xmlNode {
	if len(path) == 0 || len(nodes) == 0 {
		return nil
	}
	for _, n := range nodes {
		if n.name == path[0] {
			if len(path) == 1 {
				return n
			}
			return findNode(n.children, path[1:])
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Text formatter — indented key-value output with list detection
// ---------------------------------------------------------------------------

func formatTree(nodes []*xmlNode, depth int) string {
	var buf strings.Builder
	writeNodes(&buf, nodes, depth)
	return buf.String()
}

func writeNodes(buf *strings.Builder, nodes []*xmlNode, depth int) {
	// Count sibling name occurrences to detect list entries
	counts := make(map[string]int)
	for _, n := range nodes {
		counts[n.name]++
	}

	for _, n := range nodes {
		indent := strings.Repeat("  ", depth)
		isList := counts[n.name] > 1

		if len(n.children) == 0 {
			// Leaf or leaf-list
			buf.WriteString(fmt.Sprintf("%s%-24s %s%s%s\n", indent, n.name, colorCyan, n.value, colorReset))
			continue
		}

		// Container or list entry
		if isList && len(n.children) > 0 {
			// List entry — use first leaf child as key for display
			first := n.children[0]
			if len(first.children) == 0 && first.value != "" {
				buf.WriteString(fmt.Sprintf("%s%s%s %s%s%s\n", indent, colorYellow, n.name, colorCyan, first.value, colorReset))
				if len(n.children) > 1 {
					writeNodes(buf, n.children[1:], depth+1)
				}
				continue
			}
		}

		// Regular container
		buf.WriteString(fmt.Sprintf("%s%s%s%s\n", indent, colorYellow, n.name, colorReset))
		writeNodes(buf, n.children, depth+1)
	}
}

// formatXMLResponse parses an rpc-reply and returns human-readable text.
// If path is non-empty, only the matching subtree is shown.
func formatXMLResponse(xmlData string, path []string) string {
	tree := parseXMLTree(xmlData)
	if len(tree) == 0 {
		return ""
	}

	if len(path) > 0 {
		node := findNode(tree, path)
		if node == nil {
			return fmt.Sprintf("%spath not found: %s%s\n", colorRed, strings.Join(path, " "), colorReset)
		}
		prefix := strings.Join(path[:len(path)-1], " ")
		if prefix != "" {
			prefix += " "
		}
		if len(node.children) > 0 {
			// Container/list — show path prefix on first line, children indented
			var buf strings.Builder
			buf.WriteString(fmt.Sprintf("%s%s%s%s\n", colorDim, prefix, colorReset, node.name))
			writeNodes(&buf, node.children, 1)
			return buf.String()
		}
		// Single leaf — "system hostname ne-amf-01"
		return fmt.Sprintf("%s%s%s%s %s%s%s\n", colorDim, prefix, colorReset, node.name, colorCyan, node.value, colorReset)
	}

	return formatTree(tree, 0)
}

// formatTreePlain renders without ANSI colors (for file dump).
func formatTreePlain(nodes []*xmlNode, depth int) string {
	var buf strings.Builder
	writeNodesPlain(&buf, nodes, depth)
	return buf.String()
}

func writeNodesPlain(buf *strings.Builder, nodes []*xmlNode, depth int) {
	counts := make(map[string]int)
	for _, n := range nodes {
		counts[n.name]++
	}

	for _, n := range nodes {
		indent := strings.Repeat("  ", depth)
		isList := counts[n.name] > 1

		if len(n.children) == 0 {
			buf.WriteString(fmt.Sprintf("%s%-24s %s\n", indent, n.name, n.value))
			continue
		}

		if isList && len(n.children) > 0 {
			first := n.children[0]
			if len(first.children) == 0 && first.value != "" {
				buf.WriteString(fmt.Sprintf("%s%s %s\n", indent, n.name, first.value))
				if len(n.children) > 1 {
					writeNodesPlain(buf, n.children[1:], depth+1)
				}
				continue
			}
		}

		buf.WriteString(fmt.Sprintf("%s%s\n", indent, n.name))
		writeNodesPlain(buf, n.children, depth+1)
	}
}

// formatXMLResponsePlain formats without colors for file export.
func formatXMLResponsePlain(xmlData string, path []string) string {
	tree := parseXMLTree(xmlData)
	if len(tree) == 0 {
		return ""
	}
	if len(path) > 0 {
		node := findNode(tree, path)
		if node == nil {
			return ""
		}
		if len(node.children) > 0 {
			return formatTreePlain([]*xmlNode{node}, 0)
		}
		return fmt.Sprintf("%-24s %s\n", node.name, node.value)
	}
	return formatTreePlain(tree, 0)
}

// extractDataXML returns clean XML content from rpc-reply (strips wrapper).
func extractDataXML(xmlData string) string {
	start := strings.Index(xmlData, "<data>")
	end := strings.LastIndex(xmlData, "</data>")
	if start >= 0 && end > start {
		content := strings.TrimSpace(xmlData[start+6 : end])
		return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<config>\n" + content + "\n</config>\n"
	}
	return xmlData
}

// ---------------------------------------------------------------------------
// Text config → XML converter (for paste-to-set feature)
// ---------------------------------------------------------------------------

// textConfigToXML converts indented text config format back to XML.
// Input format (output of show running-config):
//
//	system
//	  hostname                 ne-amf-01
//	  ntp
//	    enabled                true
//
// Output:
//
//	<system><hostname>ne-amf-01</hostname><ntp><enabled>true</enabled></ntp></system>
func textConfigToXML(text string, schema *schemaNode) string {
	lines := strings.Split(text, "\n")
	type stackEntry struct {
		name  string
		depth int
	}
	var stack []stackEntry
	var buf strings.Builder

	for _, raw := range lines {
		if strings.TrimSpace(raw) == "" {
			continue
		}

		// Calculate indent depth (2 spaces = 1 level)
		trimmed := strings.TrimLeft(raw, " ")
		indent := (len(raw) - len(trimmed)) / 2

		// Close tags for nodes at same or deeper depth
		for len(stack) > 0 && stack[len(stack)-1].depth >= indent {
			top := stack[len(stack)-1]
			buf.WriteString(fmt.Sprintf("</%s>", top.name))
			stack = stack[:len(stack)-1]
		}

		// Parse the line: could be "container" or "key  value"
		fields := strings.Fields(trimmed)
		if len(fields) == 0 {
			continue
		}

		name := fields[0]

		if len(fields) == 1 {
			// Container — open tag, push to stack
			if indent == 0 && schema != nil {
				if topNode, ok := schema.children[name]; ok && topNode.namespace != "" {
					buf.WriteString(fmt.Sprintf("<%s xmlns=\"%s\">", name, topNode.namespace))
				} else {
					buf.WriteString(fmt.Sprintf("<%s>", name))
				}
			} else {
				buf.WriteString(fmt.Sprintf("<%s>", name))
			}
			stack = append(stack, stackEntry{name, indent})
		} else {
			// Leaf — key + value(s)
			value := strings.Join(fields[1:], " ")
			buf.WriteString(fmt.Sprintf("<%s>%s</%s>", name, value, name))
		}
	}

	// Close remaining open tags
	for i := len(stack) - 1; i >= 0; i-- {
		buf.WriteString(fmt.Sprintf("</%s>", stack[i].name))
	}

	return buf.String()
}

// isTextConfig detects if input is text config format (indented key-value)
// rather than XML. Simple heuristic: no < or > characters in the first line.
func isTextConfig(text string) bool {
	first := strings.TrimSpace(text)
	if first == "" {
		return false
	}
	firstLine := strings.SplitN(first, "\n", 2)[0]
	return !strings.Contains(firstLine, "<")
}

// isRPCOK checks if the rpc-reply contains <ok/>
func isRPCOK(xmlData string) bool {
	return strings.Contains(xmlData, "<ok/>")
}

// isRPCError checks if the rpc-reply contains <rpc-error>
func isRPCError(xmlData string) bool {
	return strings.Contains(xmlData, "<rpc-error>")
}

// extractRPCErrorMessage extracts the error message from rpc-error
func extractRPCErrorMessage(xmlData string) string {
	tree := parseXMLTree(xmlData)
	for _, n := range tree {
		if n.name == "rpc-error" {
			for _, child := range n.children {
				if child.name == "error-message" {
					return child.value
				}
			}
		}
	}
	return "unknown error"
}

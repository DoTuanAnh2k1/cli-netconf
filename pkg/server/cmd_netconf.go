package server

import (
	"context"
	"fmt"
	"os"
	"strings"
	"time"
)

func (s *session) cmdSet(args []string) {
	if !s.requireConnection() {
		return
	}

	var xmlData string

	if len(args) >= 2 {
		// set path... value  →  auto-build XML
		path := args[:len(args)-1]
		value := args[len(args)-1]
		xmlData = s.buildSetXML(path, value)
		if xmlData == "" {
			s.writef("%sInvalid path: %s%s\n", colorRed, strings.Join(path, " "), colorReset)
			return
		}
	} else if len(args) == 0 {
		// set (no args)  →  multiline input (XML or text config)
		s.writef("Enter config (XML or text format, end with '%s.%s' on a new line):\n", colorYellow, colorReset)
		input := s.readMultiline()
		if input == "" {
			s.writef("Cancelled.\n")
			return
		}
		if isTextConfig(input) {
			xmlData = textConfigToXML(input, s.schema)
		} else {
			xmlData = input
		}
	} else if len(args) == 1 {
		s.writef("Usage: set <path...> <value>\n")
		s.writef("       set                     (paste config mode)\n")
		return
	}

	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	reply, err := s.nc.EditConfig(ctx, "candidate", xmlData)
	elapsed := time.Since(start)

	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sOK%s\n", colorGreen, colorReset)
	} else if isRPCError(reply) {
		s.writef("%sError: %s%s\n", colorRed, extractRPCErrorMessage(reply), colorReset)
	} else {
		s.writef("%s\n", reply)
	}
	s.writef("%s(%s)%s\n", colorDim, elapsed.Round(time.Millisecond), colorReset)
	s.saveHistory("set", elapsed)
}

// buildSetXML converts "set system hostname new-name" into NETCONF edit-config XML.
func (s *session) buildSetXML(path []string, value string) string {
	if s.schema == nil || len(path) == 0 {
		return ""
	}

	// Verify path exists in schema
	topName := path[0]
	topNode, ok := s.schema.children[topName]
	if !ok {
		return ""
	}

	// Build nested XML: open tags → leaf with value → close tags
	var buf strings.Builder

	// Opening tags
	ns := topNode.namespace
	if ns != "" {
		buf.WriteString(fmt.Sprintf("<%s xmlns=\"%s\">\n", path[0], ns))
	} else {
		buf.WriteString(fmt.Sprintf("<%s>\n", path[0]))
	}
	for i := 1; i < len(path)-1; i++ {
		buf.WriteString(strings.Repeat("  ", i))
		buf.WriteString(fmt.Sprintf("<%s>\n", path[i]))
	}

	// Leaf element with value
	leafDepth := len(path) - 1
	leafName := path[leafDepth]
	buf.WriteString(strings.Repeat("  ", leafDepth))
	buf.WriteString(fmt.Sprintf("<%s>%s</%s>\n", leafName, value, leafName))

	// Closing tags (reverse order, skip the leaf)
	for i := len(path) - 2; i >= 0; i-- {
		buf.WriteString(strings.Repeat("  ", i))
		buf.WriteString(fmt.Sprintf("</%s>\n", path[i]))
	}

	return buf.String()
}

func (s *session) cmdUnset(args []string) {
	if !s.requireConnection() {
		return
	}

	if len(args) == 0 {
		s.writef("Usage: unset <path...>\n")
		s.writef("  e.g. unset system contact\n")
		s.writef("       unset interfaces interface eth2\n")
		return
	}

	xmlData := s.buildUnsetXML(args)
	if xmlData == "" {
		s.writef("%sInvalid path: %s%s\n", colorRed, strings.Join(args, " "), colorReset)
		return
	}

	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	reply, err := s.nc.EditConfig(ctx, "candidate", xmlData)
	elapsed := time.Since(start)

	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sOK%s\n", colorGreen, colorReset)
	} else if isRPCError(reply) {
		s.writef("%sError: %s%s\n", colorRed, extractRPCErrorMessage(reply), colorReset)
	} else {
		s.writef("%s\n", reply)
	}
	s.writef("%s(%s)%s\n", colorDim, elapsed.Round(time.Millisecond), colorReset)
	s.saveHistory("unset", elapsed)
}

// buildUnsetXML converts "unset system contact" into NETCONF edit-config XML
// with operation="delete" on the target element.
func (s *session) buildUnsetXML(path []string) string {
	if s.schema == nil || len(path) == 0 {
		return ""
	}

	topName := path[0]
	topNode, ok := s.schema.children[topName]
	if !ok {
		return ""
	}

	var buf strings.Builder
	nc := "urn:ietf:params:xml:ns:netconf:base:1.0"

	// Opening tags
	ns := topNode.namespace
	if ns != "" {
		buf.WriteString(fmt.Sprintf("<%s xmlns=\"%s\" xmlns:nc=\"%s\">\n", path[0], ns, nc))
	} else {
		buf.WriteString(fmt.Sprintf("<%s xmlns:nc=\"%s\">\n", path[0], nc))
	}
	for i := 1; i < len(path)-1; i++ {
		buf.WriteString(strings.Repeat("  ", i))
		buf.WriteString(fmt.Sprintf("<%s>\n", path[i]))
	}

	// Target element with delete operation
	last := path[len(path)-1]
	depth := len(path) - 1
	buf.WriteString(strings.Repeat("  ", depth))
	buf.WriteString(fmt.Sprintf("<%s nc:operation=\"delete\"/>\n", last))

	// Closing tags
	for i := len(path) - 2; i >= 0; i-- {
		buf.WriteString(strings.Repeat("  ", i))
		buf.WriteString(fmt.Sprintf("</%s>\n", path[i]))
	}

	return buf.String()
}

func (s *session) cmdCommit() {
	if !s.requireConnection() {
		return
	}

	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()

	reply, err := s.nc.Commit(ctx)
	elapsed := time.Since(start)

	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sCommit successful.%s\n", colorGreen, colorReset)
	} else if isRPCError(reply) {
		s.writef("%sCommit failed: %s%s\n", colorRed, extractRPCErrorMessage(reply), colorReset)
	} else {
		s.writef("%s\n", reply)
	}
	s.writef("%s(%s)%s\n", colorDim, elapsed.Round(time.Millisecond), colorReset)
	s.saveHistory("commit", elapsed)
}

func (s *session) cmdValidate() {
	if !s.requireConnection() {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	reply, err := s.nc.Validate(ctx, "candidate")
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sValidation OK.%s\n", colorGreen, colorReset)
	} else if isRPCError(reply) {
		s.writef("%sValidation failed: %s%s\n", colorRed, extractRPCErrorMessage(reply), colorReset)
	} else {
		s.writef("%s\n", reply)
	}
}

func (s *session) cmdDiscard() {
	if !s.requireConnection() {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	reply, err := s.nc.DiscardChanges(ctx)
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sChanges discarded.%s\n", colorGreen, colorReset)
	} else {
		s.writef("%s\n", reply)
	}
}

func (s *session) cmdLock(args []string) {
	if !s.requireConnection() {
		return
	}
	target := "candidate"
	if len(args) > 0 {
		target = args[0]
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	reply, err := s.nc.Lock(ctx, target)
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sLocked %s.%s\n", colorGreen, target, colorReset)
	} else if isRPCError(reply) {
		s.writef("%sLock failed: %s%s\n", colorRed, extractRPCErrorMessage(reply), colorReset)
	} else {
		s.writef("%s\n", reply)
	}
}

func (s *session) cmdUnlock(args []string) {
	if !s.requireConnection() {
		return
	}
	target := "candidate"
	if len(args) > 0 {
		target = args[0]
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	reply, err := s.nc.Unlock(ctx, target)
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	if isRPCOK(reply) {
		s.writef("%sUnlocked %s.%s\n", colorGreen, target, colorReset)
	} else {
		s.writef("%s\n", reply)
	}
}

func (s *session) cmdDump(args []string) {
	if !s.requireConnection() {
		return
	}

	if len(args) == 0 {
		s.writef("Usage: dump text [filename]\n")
		s.writef("       dump xml  [filename]\n")
		return
	}

	format := strings.ToLower(args[0])
	if format != "text" && format != "xml" {
		s.writef("%sFormat must be 'text' or 'xml'%s\n", colorRed, colorReset)
		return
	}

	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	reply, err := s.nc.GetConfig(ctx, "running", "")
	elapsed := time.Since(start)
	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	var content string
	switch format {
	case "text":
		content = formatXMLResponsePlain(reply, nil)
	case "xml":
		content = extractDataXML(reply)
	}

	if len(args) > 1 {
		filename := args[1]
		if err := os.WriteFile(filename, []byte(content), 0644); err != nil {
			s.writef("%sError writing file: %s%s\n", colorRed, err, colorReset)
			return
		}
		s.writef("%sSaved to %s (%d bytes)%s\n", colorGreen, filename, len(content), colorReset)
	} else {
		s.writef("%s", content)
	}
	s.writef("%s(%s)%s\n", colorDim, elapsed.Round(time.Millisecond), colorReset)
	s.saveHistory("dump "+format, elapsed)
}

func (s *session) cmdRPC() {
	if !s.requireConnection() {
		return
	}

	s.writef("Enter RPC body XML (end with '%s.%s' on a new line):\n", colorYellow, colorReset)
	body := s.readMultiline()
	if body == "" {
		s.writef("Cancelled.\n")
		return
	}

	start := time.Now()
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	reply, err := s.nc.SendRPC(ctx, body)
	elapsed := time.Since(start)

	if err != nil {
		s.writef("%sError: %s%s\n", colorRed, err, colorReset)
		return
	}

	s.writef("%s\n", reply)
	s.writef("%s(%s)%s\n", colorDim, elapsed.Round(time.Millisecond), colorReset)
}

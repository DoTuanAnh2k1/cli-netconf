package server

import (
	"context"
	"fmt"
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
		// set (no args)  →  multiline XML input
		s.writef("Enter config XML (end with '%s.%s' on a new line):\n", colorYellow, colorReset)
		xmlData = s.readMultiline()
		if xmlData == "" {
			s.writef("Cancelled.\n")
			return
		}
	} else {
		s.writef("Usage: set <path...> <value>\n")
		s.writef("       set                     (XML input mode)\n")
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

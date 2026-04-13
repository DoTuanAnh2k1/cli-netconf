package server

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"testing"
	"time"

	"github.com/DoTuanAnh2k1/cli-netconf/pkg/netconf"
)

// TestSMFSchemaTabCompletion verifies that the XML-config merger does not
// corrupt the YANG-derived schema structure.
//
// The mock-netconf server advertises smf-config and returns a running-config
// XML where <qosConf> appears directly under <nsmfPduSession> WITHOUT the
// intermediate <vsmf> wrapper — this reproduces the ConfD behaviour where
// containers whose leaves all use default values are omitted from the XML.
//
// Expected (correct) schema from YANG:
//
//	smf → nsmfPduSession → common
//	                     → vsmf → httpProtocol
//	                            → gtpProtocol
//	                            → features
//	                            → qosConf → useLocalQos
//	                                      → predefined
//
// qosConf must NOT appear as a direct child of nsmfPduSession.
func TestSMFSchemaTabCompletion(t *testing.T) {
	// Each RPC uses a generous per-call timeout so it can never expire on a
	// healthy loopback connection.  Using context.Background()-derived timeouts
	// that are too short caused goroutine races when a previous readMessage
	// goroutine kept reading from stdout after its context fired, competing
	// with the next goroutine.  The outer -timeout flag guards against hangs.
	rpcCtx := context.Background()

	dialCtx, dialCancel := context.WithTimeout(rpcCtx, 10*time.Second)
	defer dialCancel()

	nc, err := netconf.Dial(dialCtx, "127.0.0.1", 8830, "admin", "admin")
	if err != nil {
		t.Skipf("mock-netconf not running: %v", err)
	}
	defer nc.Close()

	// --- replicate loadSchema logic ---
	schema := newSchemaNode()

	modules := nc.ExtractModules()
	t.Logf("modules from capabilities: %v", modules)

	for _, mod := range modules {
		reply, err := nc.GetSchema(rpcCtx, mod)
		if err != nil {
			t.Logf("GetSchema %s: %v", mod, err)
			continue
		}
		yangText := extractSchemaText(reply)
		if yangText == "" {
			continue
		}
		ns := findNamespace(nc.Capabilities, mod)
		parsed := parseSchemaFromYANG(yangText, ns)
		mergeSchema(schema, parsed)
		t.Logf("YANG loaded: module=%s top-elements=%v", mod, parsed.childNames())
	}

	reply, err := nc.GetConfig(rpcCtx, "running", "")
	if err == nil {
		configSchema := parseSchemaFromXML(reply)
		// Apply the fix: only add top-level containers YANG doesn't know about
		if len(schema.children) == 0 {
			mergeSchema(schema, configSchema)
		} else {
			for name, xmlNode := range configSchema.children {
				if _, exists := schema.children[name]; !exists {
					schema.children[name] = xmlNode
				}
			}
		}
		t.Logf("XML config top-elements: %v", configSchema.childNames())
	}

	t.Logf("final schema top-elements: %v", schema.childNames())

	// --- assertions ---

	check := func(path []string, wantPresent, wantAbsent []string) {
		t.Helper()
		node := schema.lookup(path)
		if node == nil {
			t.Errorf("path [%s] not found in schema", strings.Join(path, " "))
			return
		}
		children := node.childNames()
		sort.Strings(children)
		childSet := make(map[string]bool, len(children))
		for _, c := range children {
			childSet[c] = true
		}

		for _, w := range wantPresent {
			if !childSet[w] {
				t.Errorf("path [%s]: missing expected child %q  (got %v)",
					strings.Join(path, " "), w, children)
			}
		}
		for _, w := range wantAbsent {
			if childSet[w] {
				t.Errorf("path [%s]: unexpected child %q appeared (should only be under vsmf). "+
					"XML merge bug not fixed!  children=%v",
					strings.Join(path, " "), w, children)
			}
		}
		if !t.Failed() {
			fmt.Printf("  OK  [%-45s] children=%v\n",
				strings.Join(path, " "), children)
		}
	}

	// nsmfPduSession direct children: must have common+vsmf, must NOT have qosConf
	check(
		[]string{"smf", "nsmfPduSession"},
		[]string{"common", "vsmf"},
		[]string{"qosConf"},
	)

	// vsmf children: must contain qosConf
	check(
		[]string{"smf", "nsmfPduSession", "vsmf"},
		[]string{"qosConf", "httpProtocol", "gtpProtocol", "features"},
		nil,
	)

	// qosConf children
	check(
		[]string{"smf", "nsmfPduSession", "vsmf", "qosConf"},
		[]string{"useLocalQos", "predefined"},
		nil,
	)
}

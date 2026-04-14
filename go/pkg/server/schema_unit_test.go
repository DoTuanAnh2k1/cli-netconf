package server

import (
	"os"
	"strings"
	"testing"
)

// smfConfigYANGText is identical to the mock's smfConfigYang constant.
// Kept here so the unit tests don't depend on the mock package.
const smfConfigYANGText = `module smf-config {
  namespace "urn:5gc:smf-config";
  prefix smf;

  grouping gpNsmfPduSession {
    container common {
      leaf hostname {
        type string;
      }
    }
    container vsmf {
      container httpProtocol {
        leaf host {
          type string;
        }
        leaf port {
          type uint16;
        }
      }
      container gtpProtocol {
        leaf listenPort {
          type uint16;
        }
      }
      container features {
        leaf enableFeatureA {
          type boolean;
          default false;
        }
        leaf enableFeatureB {
          type boolean;
          default false;
        }
      }
      container qosConf {
        leaf useLocalQos {
          type boolean;
          default true;
        }
        container predefined {
          leaf enabled {
            type boolean;
            default false;
          }
        }
      }
    }
  }

  container smf {
    container nsmfPduSession {
      uses gpNsmfPduSession;
    }
  }
}`

// TestYANGParserMockSchema verifies the schema produced from the mock YANG
// (same structure as smf-config) — no network calls required.
func TestYANGParserMockSchema(t *testing.T) {
	schema := parseSchemaFromYANG(smfConfigYANGText, "urn:5gc:smf-config")
	assertSchema(t, schema)
}

// TestYANGParserRealConfig verifies the schema produced from the real config.yang
// shipped in the repository.  Skip when the file is absent (CI without artifacts).
func TestYANGParserRealConfig(t *testing.T) {
	data, err := os.ReadFile("../../config.yang")
	if err != nil {
		t.Skipf("config.yang not found: %v", err)
	}
	schema := parseSchemaFromYANG(string(data), "urn:5gc:smf-config")
	assertSchema(t, schema)
}

// TestXMLParserWithVsmf tests parseSchemaFromXML when the XML running-config
// contains a <vsmf> wrapper (i.e. smfName is configured, as on the real NE).
func TestXMLParserWithVsmf(t *testing.T) {
	xmlData := `<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="1" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <data>
    <smf xmlns="urn:5gc:smf-config">
      <nsmfPduSession>
        <common>
          <hostname>smf-01</hostname>
        </common>
        <vsmf>
          <smfName>SMF-PROD-01</smfName>
          <qosConf>
            <useLocalQos>true</useLocalQos>
          </qosConf>
        </vsmf>
      </nsmfPduSession>
    </smf>
  </data>
</rpc-reply>`

	schema := parseSchemaFromXML(xmlData)

	check := func(path []string, wantPresent, wantAbsent []string) {
		t.Helper()
		node := schema.lookup(path)
		if node == nil {
			t.Errorf("path [%s] not found in schema", strings.Join(path, " "))
			return
		}
		children := node.childNames()
		childSet := make(map[string]bool, len(children))
		for _, c := range children {
			childSet[c] = true
		}
		for _, w := range wantPresent {
			if !childSet[w] {
				t.Errorf("path [%s]: missing %q (got %v)", strings.Join(path, " "), w, children)
			}
		}
		for _, w := range wantAbsent {
			if childSet[w] {
				t.Errorf("path [%s]: unexpected %q appeared (got %v)", strings.Join(path, " "), w, children)
			}
		}
	}

	// qosConf must be under vsmf, not directly under nsmfPduSession
	check([]string{"smf", "nsmfPduSession"}, []string{"common", "vsmf"}, []string{"qosConf"})
	check([]string{"smf", "nsmfPduSession", "vsmf"}, []string{"smfName", "qosConf"}, nil)
	check([]string{"smf", "nsmfPduSession", "vsmf", "qosConf"}, []string{"useLocalQos"}, nil)
}

// TestMergeXMLOnlyWithVsmf simulates the real NE scenario:
//   - smf-config NOT in capabilities → no YANG loaded for it
//   - XML running-config HAS <vsmf> (smfName is configured)
//   - The schema should still be correct via XML-only fallback
func TestMergeXMLOnlyWithVsmf(t *testing.T) {
	// Simulate: YANG schema loaded only for ne-system (not smf-config)
	neSystemYANG := `module ne-system {
  namespace "urn:params:xml:ns:yang:ne-system";
  prefix sys;
  container system {
    leaf hostname { type string; }
  }
}`
	schema := parseSchemaFromYANG(neSystemYANG, "urn:params:xml:ns:yang:ne-system")

	// XML config has smf with vsmf wrapper (smfName is configured)
	xmlData := `<?xml version="1.0" encoding="UTF-8"?>
<rpc-reply message-id="1" xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <data>
    <smf xmlns="urn:5gc:smf-config">
      <nsmfPduSession>
        <common><hostname>smf-01</hostname></common>
        <vsmf>
          <smfName>SMF-PROD-01</smfName>
          <qosConf><useLocalQos>true</useLocalQos></qosConf>
        </vsmf>
      </nsmfPduSession>
    </smf>
    <system xmlns="urn:params:xml:ns:yang:ne-system">
      <hostname>ne-01</hostname>
    </system>
  </data>
</rpc-reply>`

	configSchema := parseSchemaFromXML(xmlData)

	// Merge: add top-level XML containers not already in YANG schema
	for name, xmlNode := range configSchema.children {
		if _, exists := schema.children[name]; !exists {
			schema.children[name] = xmlNode
		}
	}

	check := func(path []string, wantPresent, wantAbsent []string) {
		t.Helper()
		node := schema.lookup(path)
		if node == nil {
			t.Errorf("path [%s] not found in schema", strings.Join(path, " "))
			return
		}
		children := node.childNames()
		childSet := make(map[string]bool, len(children))
		for _, c := range children {
			childSet[c] = true
		}
		for _, w := range wantPresent {
			if !childSet[w] {
				t.Errorf("path [%s]: missing %q (got %v)", strings.Join(path, " "), w, children)
			}
		}
		for _, w := range wantAbsent {
			if childSet[w] {
				t.Errorf("path [%s]: unexpected %q appeared (got %v)", strings.Join(path, " "), w, children)
			}
		}
	}

	// Top-level: both system (YANG) and smf (XML) should be present
	check([]string{}, []string{"system", "smf"}, nil)

	// smf → nsmfPduSession → must have vsmf, must NOT have qosConf
	check([]string{"smf", "nsmfPduSession"}, []string{"common", "vsmf"}, []string{"qosConf"})

	// vsmf → qosConf (and smfName)
	check([]string{"smf", "nsmfPduSession", "vsmf"}, []string{"smfName", "qosConf"}, nil)

	// qosConf → useLocalQos
	check([]string{"smf", "nsmfPduSession", "vsmf", "qosConf"}, []string{"useLocalQos"}, nil)
}

// assertSchema checks that the given schema has the correct smf/nsmfPduSession/vsmf/qosConf structure.
func assertSchema(t *testing.T, schema *schemaNode) {
	t.Helper()

	check := func(path []string, wantPresent, wantAbsent []string) {
		t.Helper()
		node := schema.lookup(path)
		if node == nil {
			t.Errorf("path [%s] not found in schema", strings.Join(path, " "))
			return
		}
		children := node.childNames()
		childSet := make(map[string]bool, len(children))
		for _, c := range children {
			childSet[c] = true
		}
		for _, w := range wantPresent {
			if !childSet[w] {
				t.Errorf("path [%s]: missing expected child %q (got %v)",
					strings.Join(path, " "), w, children)
			}
		}
		for _, w := range wantAbsent {
			if childSet[w] {
				t.Errorf("path [%s]: unexpected child %q present (got %v)",
					strings.Join(path, " "), w, children)
			}
		}
		if !t.Failed() {
			t.Logf("OK [%-45s] children=%v", strings.Join(path, " "), children)
		}
	}

	// nsmfPduSession must have common + vsmf, but NOT qosConf directly
	check([]string{"smf", "nsmfPduSession"}, []string{"common", "vsmf"}, []string{"qosConf"})

	// vsmf must contain qosConf
	check([]string{"smf", "nsmfPduSession", "vsmf"}, []string{"qosConf"}, nil)

	// qosConf must contain useLocalQos + predefined
	check([]string{"smf", "nsmfPduSession", "vsmf", "qosConf"}, []string{"useLocalQos", "predefined"}, nil)
}

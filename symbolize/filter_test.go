// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package symbolize

import (
	"encoding/json"
	"fmt"
	"reflect"
	"testing"
)

type mockModule struct {
	name      string
	addr2line map[uint64][]SourceLocation
}

type mockSymbolizer struct {
	modules map[string]mockModule
}

func newMockSymbolizer(modules []mockModule) Symbolizer {
	var out mockSymbolizer
	out.modules = make(map[string]mockModule)
	for _, mod := range modules {
		out.modules[mod.name] = mod
	}
	return &out
}

func (s *mockSymbolizer) FindSrcLoc(file string, modRelAddr uint64) <-chan LLVMSymbolizeResult {
	out := make(chan LLVMSymbolizeResult, 1)
	if mod, ok := s.modules[file]; ok {
		if locs, ok := mod.addr2line[modRelAddr]; ok {
			out <- LLVMSymbolizeResult{locs, nil}
		} else {
			out <- LLVMSymbolizeResult{nil, fmt.Errorf("0x%x was not a valid address in %s", modRelAddr, file)}
		}
	} else {
		out <- LLVMSymbolizeResult{nil, fmt.Errorf("%s could not be found", file)}
	}
	return out
}

type symbolizerRepo struct {
	builds map[string]string
}

func ExampleBasic() {
	// mock the input and outputs of llvm-symbolizer
	symbo := newMockSymbolizer([]mockModule{
		{"out/libc.so", map[uint64][]SourceLocation{
			0x429c0: {{NewOptStr("atan2.c"), 49, NewOptStr("atan2")}, {NewOptStr("math.h"), 51, NewOptStr("__DOUBLE_FLOAT")}},
			0x43680: {{NewOptStr("pow.c"), 23, NewOptStr("pow")}},
			0x44987: {{NewOptStr("memcpy.c"), 76, NewOptStr("memcpy")}},
		}},
		{"out/libcrypto.so", map[uint64][]SourceLocation{
			0x81000: {{NewOptStr("rsa.c"), 101, NewOptStr("mod_exp")}},
			0x82000: {{NewOptStr("aes.c"), 17, NewOptStr("gf256_mul")}},
			0x83000: {{NewOptStr("aes.c"), 560, NewOptStr("gf256_div")}},
		}},
	})
	// mock ids.txt
	repo := NewRepo()
	repo.AddObjects(map[string]string{
		"be4c4336e20b734db97a58e0f083d0644461317c": "out/libc.so",
		"b4b6c520ccf0aa11ff71d8ded7d6a2bc03037aa1": "out/libcrypto.so",
	})

	// make an actual filter using those two mock objects
	filter := NewFilter(repo, symbo)

	// parse some example lines
	filter.AddModule(Module{"libc.so", "be4c4336e20b734db97a58e0f083d0644461317c", 1})
	filter.AddModule(Module{"libcrypto.so", "b4b6c520ccf0aa11ff71d8ded7d6a2bc03037aa1", 2})
	filter.AddSegment(Segment{1, 0x12345000, 849596, "rx", 0x0})
	filter.AddSegment(Segment{2, 0x23456000, 539776, "rx", 0x80000})
	line := ParseLine("\033[1m Error at {{{pc:0x123879c0}}}")
	// print out a more precise form
	line.Accept(&FilterVisitor{filter})
	json, err := GetLineJson(line)
	if err != nil {
		fmt.Printf("json did not parse correctly: %v", err)
		return
	}
	fmt.Print(string(json))

	// Output:
	//{
	//	"type": "group",
	//	"children": [
	//		{
	//			"type": "color",
	//			"color": 1,
	//			"children": [
	//				{
	//					"type": "text",
	//					"text": " Error at "
	//				},
	//				{
	//					"type": "pc",
	//					"vaddr": 305691072,
	//					"file": "atan2.c",
	//					"line": 49,
	//					"function": "atan2"
	//				}
	//			]
	//		}
	//	]
	//}
}

func TestMalformed(t *testing.T) {
	// Parse a bad line
	line := ParseLine("\033[1m Error at {{{pc:0x123879c0")

	if line != nil {
		t.Error("expected", nil, "got", line)
	}
}

func EqualJson(a, b []byte) bool {
	var j1, j2 interface{}
	err := json.Unmarshal(a, &j1)
	if err != nil {
		panic(err.Error())
	}
	err = json.Unmarshal(b, &j2)
	if err != nil {
		panic(err.Error())
	}
	return reflect.DeepEqual(j1, j2)
}

func TestBacktrace(t *testing.T) {
	line := ParseLine("Error at {{{bt:0:0x12389987}}}")

	if line == nil {
		t.Error("got", nil, "expected", "not nil")
		return
	}

	// mock the input and outputs of llvm-symbolizer
	symbo := newMockSymbolizer([]mockModule{
		{"out/libc.so", map[uint64][]SourceLocation{
			0x44987: {{NewOptStr("duff.h"), 64, NewOptStr("duffcopy")}, {NewOptStr("memcpy.c"), 76, NewOptStr("memcpy")}},
		}},
	})
	// mock ids.txt
	repo := NewRepo()
	repo.AddObjects(map[string]string{
		"be4c4336e20b734db97a58e0f083d0644461317c": "out/libc.so",
	})

	// make an actual filter using those two mock objects
	filter := NewFilter(repo, symbo)

	// add some context
	filter.AddModule(Module{"libc.so", "be4c4336e20b734db97a58e0f083d0644461317c", 1})
	filter.AddSegment(Segment{1, 0x12345000, 849596, "rx", 0x0})
	line.Accept(&FilterVisitor{filter})

	json, err := GetLineJson(line)
	if err != nil {
		t.Error("json did not parse correctly", err)
	}

	expectedJson := []byte(`{
    "type": "group",
    "children": [
      {"type": "text", "text": "Error at "},
      {"type": "bt", "vaddr": 305699207, "num": 0, "locs":[
        {"line": 64, "function": "duffcopy", "file": "duff.h"},
        {"line": 76, "function": "memcpy", "file": "memcpy.c"}
      ]}
    ]
  }`)

	if !EqualJson(json, expectedJson) {
		t.Error("unexpected json output", "got", string(json), "expected", string(expectedJson))
	}
}

func TestReset(t *testing.T) {
	line := ParseLine("{{{reset}}}")

	json, err := GetLineJson(line)
	if err != nil {
		t.Error("json did not parse correctly", err)
	}

	expectedJson := []byte(`{"type":"group", "children":[{"type":"reset"}]}`)
	if !EqualJson(json, expectedJson) {
		t.Error("unexpected json output", "got", string(json), "expected", string(expectedJson))
	}

	// mock the input and outputs of llvm-symbolizer
	symbo := newMockSymbolizer([]mockModule{
		{"out/libc.so", map[uint64][]SourceLocation{
			0x44987: {{NewOptStr("memcpy.c"), 76, NewOptStr("memcpy")}},
		}},
	})
	// mock ids.txt
	repo := NewRepo()
	repo.AddObjects(map[string]string{
		"be4c4336e20b734db97a58e0f083d0644461317c": "out/libc.so",
	})

	// make an actual filter using those two mock objects
	filter := NewFilter(repo, symbo)

	// add some context
	mod := Module{"libc.so", "be4c4336e20b734db97a58e0f083d0644461317c", 1}
	filter.AddModule(mod)
	seg := Segment{1, 0x12345000, 849596, "rx", 0x0}
	filter.AddSegment(seg)

	addr := uint64(0x12389987)

	if info, err := filter.FindInfoForAddress(addr); err != nil {
		t.Error("expected", nil, "got", err)
		if len(info.locs) != 1 {
			t.Error("expected", 1, "source location but got", len(info.locs))
		} else {
			loc := SourceLocation{NewOptStr("memcpy.c"), 76, NewOptStr("memcpy")}
			if info.locs[0] != loc {
				t.Error("expected", loc, "got", info.locs[0])
			}
		}
		if info.mod != mod {
			t.Error("expected", mod, "got", info.mod)
		}
		if info.seg != seg {
			t.Error("expected", seg, "got", info.seg)
		}
		if info.addr != addr {
			t.Error("expected", addr, "got", info.addr)
		}
	}

	// now forget the context
	line.Accept(&FilterVisitor{filter})

	if _, err := filter.FindInfoForAddress(addr); err == nil {
		t.Error("expected non-nil error but got", err)
	}
}

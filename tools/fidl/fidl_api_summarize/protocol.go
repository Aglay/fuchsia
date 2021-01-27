// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package main

import (
	"fmt"
	"strings"

	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

// addProtocols adds the protocols to the elements list.
func (s *summarizer) addProtocols(protocols []fidlgen.Protocol) {
	for _, p := range protocols {
		for _, m := range p.Methods {
			s.addElement(newMethod(p.Name, m))
		}
		s.addElement(protocol{named: named{name: string(p.Name)}})
	}
}

// protocol represents an element of the protocol type.
type protocol struct {
	named
	notMember
}

// String implements Element.
func (p protocol) String() string {
	return fmt.Sprintf("protocol %v", p.Name())
}

// method represents an Element for a protocol method.
type method struct {
	membership isMember
	method     fidlgen.Method
}

// newMethod creates a new protocol method element.
func newMethod(parent fidlgen.EncodedCompoundIdentifier, m fidlgen.Method) method {
	return method{
		membership: newIsMember(parent, m.Name, fidlgen.ProtocolDeclType),
		method:     m,
	}
}

// Name implements Element.
func (m method) Name() string {
	return m.membership.Name()
}

// String implements Element.  It formats a protocol method using a notation
// familiar from FIDL.
func (m method) String() string {
	var parlist []string
	request := getParamList(m.method.HasRequest, m.method.Request)
	if request != "" {
		parlist = append(parlist, request)
	}
	response := getParamList(m.method.HasResponse, m.method.Response)
	if response != "" {
		if request == "" {
			// -> Method(T a)
			parlist = append(parlist, "")
		}
		parlist = append(parlist, "->", response)
	}
	return fmt.Sprintf(
		"protocol/member %v%v",
		m.membership.Name(), strings.Join(parlist, " "))
}

// Member implements Element.
func (m method) Member() bool {
	return m.membership.Member()
}

// getParamList formats a parameter list, as in Foo(ty1 a, ty2b)
func getParamList(hasParams bool, params []fidlgen.Parameter) string {
	if !hasParams {
		return ""
	}
	var ps []string
	for _, p := range params {
		ps = append(ps, fmt.Sprintf("%v %v", fidlTypeString(p.Type), p.Name))
	}
	return fmt.Sprintf("(%v)", strings.Join(ps, ","))
}

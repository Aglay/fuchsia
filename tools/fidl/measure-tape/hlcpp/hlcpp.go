// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package hlcpp

import (
	"bytes"
	"fmt"
	"log"
	"regexp"
	"sort"
	"strings"
	"text/template"

	fidlcommon "fidl/compiler/backend/common"

	"measure-tape/measurer"
)

// TODO(fxb/50011): Create a Rust backend similar to the below.

type Printer struct {
	m            *measurer.Measurer
	hIncludePath string
}

func NewPrinter(m *measurer.Measurer, hIncludePath string) *Printer {
	return &Printer{
		m:            m,
		hIncludePath: hIncludePath,
	}
}

type tmplParams struct {
	HeaderTag              string
	HIncludePath           string
	Namespaces             []string
	LibraryNameWithSlashes string
	TargetType             string
	CcIncludes             []string
}

func (params tmplParams) RevNamespaces() []string {
	rev := make([]string, len(params.Namespaces), len(params.Namespaces))
	for i, j := 0, len(params.Namespaces)-1; i < len(params.Namespaces); i, j = i+1, j-1 {
		rev[i] = params.Namespaces[j]
	}
	return rev
}

var header = template.Must(template.New("tmpls").Parse(
	`// File is automatically generated; do not modify.
// See tools/fidl/measure-tape/README.md

#ifndef {{ .HeaderTag }}
#define {{ .HeaderTag }}

#include <{{ .LibraryNameWithSlashes }}/cpp/fidl.h>

{{ range .Namespaces }}
namespace {{ . }} {
{{- end}}

struct Size {
  explicit Size(int64_t num_bytes, int64_t num_handles)
    : num_bytes(num_bytes), num_handles(num_handles) {}

  const int64_t num_bytes;
  const int64_t num_handles;
};

// Helper function to measure {{ .TargetType }}.
//
// In most cases, the size returned is a precise size. Otherwise, the size
// returned is a safe upper-bound.
Size Measure(const {{ .TargetType }}& value);

{{ range .RevNamespaces }}
}  // {{ . }}
{{- end}}

#endif  // {{ .HeaderTag }}
`))

var ccTop = template.Must(template.New("tmpls").Parse(
	`// File is automatically generated; do not modify.
// See tools/fidl/measure-tape/README.md

#include <{{ .HIncludePath }}>
{{ range .CcIncludes }}
{{ . }}
{{- end }}
#include <zircon/types.h>

{{ range .Namespaces }}
namespace {{ . }} {
{{- end}}

namespace {

class MeasuringTape {
 public:
  MeasuringTape() = default;
`))

var ccBottom = template.Must(template.New("tmpls").Parse(`
  Size Done() {
    if (maxed_out_) {
      return Size(ZX_CHANNEL_MAX_MSG_BYTES, ZX_CHANNEL_MAX_MSG_HANDLES);
    }
    return Size(num_bytes_, num_handles_);
  }

private:
  void MaxOut() { maxed_out_ = true; }

  bool maxed_out_ = false;
  int64_t num_bytes_ = 0;
  int64_t num_handles_ = 0;
};

}  // namespace

Size Measure(const {{ .TargetType }}& value) {
  MeasuringTape tape;
  tape.Measure(value);
  return tape.Done();
}

{{ range .RevNamespaces }}
}  // {{ . }}
{{- end}}
`))

var pathSeparators = regexp.MustCompile("[/_.-]")

func (p *Printer) newTmplParams(targetMt *measurer.MeasuringTape) tmplParams {
	namespaces := []string{"measure_tape"}
	namespaces = append(namespaces, targetMt.Name().LibraryName().Parts()...)

	headerTagParts := pathSeparators.Split(p.hIncludePath, -1)
	for i, part := range headerTagParts {
		headerTagParts[i] = strings.ToUpper(part)
	}
	headerTagParts = append(headerTagParts, "")
	headerTag := strings.Join(headerTagParts, "_")

	return tmplParams{
		HeaderTag:              headerTag,
		HIncludePath:           p.hIncludePath,
		LibraryNameWithSlashes: strings.Join(targetMt.Name().LibraryName().Parts(), "/"),
		TargetType:             fmtType(targetMt.Name()),
		Namespaces:             namespaces,
	}
}

func (p *Printer) WriteH(buf *bytes.Buffer, targetMt *measurer.MeasuringTape) {
	if err := header.Execute(buf, p.newTmplParams(targetMt)); err != nil {
		panic(err.Error())
	}
}

func (p *Printer) WriteCc(buf *bytes.Buffer,
	targetMt *measurer.MeasuringTape,
	allMethods map[measurer.MethodID]*measurer.Method) {

	params := p.newTmplParams(targetMt)
	for _, libraryName := range p.m.RootLibraries() {
		params.CcIncludes = append(params.CcIncludes,
			fmt.Sprintf("#include <%s/cpp/fidl.h>", strings.Join(libraryName.Parts(), "/")))
	}
	sort.Strings(params.CcIncludes)

	if err := ccTop.Execute(buf, params); err != nil {
		panic(err.Error())
	}

	methodsToPrint := make([]measurer.MethodID, 0, len(allMethods))
	for id := range allMethods {
		methodsToPrint = append(methodsToPrint, id)
	}
	sort.Sort(measurer.ByTargetTypeThenKind(methodsToPrint))
	cb := codeBuffer{buf: buf, level: 1}
	for _, id := range methodsToPrint {
		buf.WriteString("\n")
		m := allMethods[id]
		cb.writeMethod(m)
	}

	if err := ccBottom.Execute(buf, params); err != nil {
		panic(err.Error())
	}
}

const indent = "  "

type codeBuffer struct {
	level int
	buf   *bytes.Buffer
}

func (buf *codeBuffer) writef(format string, a ...interface{}) {
	for i := 0; i < buf.level; i++ {
		buf.buf.WriteString(indent)
	}
	buf.buf.WriteString(fmt.Sprintf(format, a...))
}

func (buf *codeBuffer) indent(fn func()) {
	buf.level++
	fn()
	buf.level--
}

var _ measurer.StatementFormatter = (*codeBuffer)(nil)

func (buf *codeBuffer) CaseMaxOut() {
	buf.writef("MaxOut();\n")
}

func (buf *codeBuffer) CaseAddNumBytes(val measurer.Expression) {
	buf.writef("num_bytes_ += %s;\n", formatExpr{val}.String())
}

func (buf *codeBuffer) CaseAddNumHandles(val measurer.Expression) {
	buf.writef("num_handles_ += %s;\n", formatExpr{val}.String())
}

func (buf *codeBuffer) CaseInvoke(id measurer.MethodID, val measurer.Expression) {
	buf.writef("%s(%s);\n", fmtMethodKind(id.Kind), formatExpr{val}.String())
}

func (buf *codeBuffer) CaseGuard(cond measurer.Expression, body *measurer.Block) {
	buf.writef("if (%s) {\n", formatExpr{cond}.String())
	buf.indent(func() {
		buf.writeBlock(body)
	})
	buf.writef("}\n")
}

func (buf *codeBuffer) CaseIterate(local, val measurer.Expression, body *measurer.Block) {
	var deref string
	if val.Nullable() {
		deref = "*"
	}
	buf.writef("for (const auto& %s : %s%s) {\n",
		formatExpr{local}.String(),
		deref, formatExpr{val}.String())
	buf.indent(func() {
		buf.writeBlock(body)
	})
	buf.writef("}\n")
}

func (buf *codeBuffer) CaseSelectVariant(
	val measurer.Expression,
	targetType fidlcommon.Name,
	variants map[string]*measurer.Block) {

	buf.writef("switch (%s.Which()) {\n", formatExpr{val}.String())
	buf.indent(func() {
		var members []string
		for member := range variants {
			members = append(members, member)
		}
		sort.Strings(members)
		for _, member := range members {
			if member != "" {
				buf.writef("case %s:\n", fmtKnownVariant(targetType, member))
			} else {
				buf.writef("case %s:\n", fmtUnknownVariant(targetType))
			}
			buf.indent(func() {
				buf.writeBlock(variants[member])
				buf.writef("break;\n")
			})
		}
	})
	buf.writef("}\n")
}

func (buf *codeBuffer) CaseDeclareMaxOrdinal(local measurer.Expression) {
	buf.writef("int32_t %s = 0;\n", formatExpr{local}.String())
}

func (buf *codeBuffer) CaseSetMaxOrdinal(local, ordinal measurer.Expression) {
	buf.writef("%s = %s;\n", formatExpr{local}.String(), formatExpr{ordinal}.String())
}

func (buf *codeBuffer) writeBlock(b *measurer.Block) {
	b.ForAllStatements(func(stmt *measurer.Statement) {
		stmt.Visit(buf)
	})
}

func (buf *codeBuffer) writeMethod(m *measurer.Method) {
	buf.writef("void %s(const %s& %s) {\n", fmtMethodKind(m.ID.Kind), fmtType(m.ID.TargetType), formatExpr{m.Arg})
	buf.indent(func() {
		buf.writeBlock(m.Body)
	})
	buf.writef("}\n")
}

func fmtMethodKind(kind measurer.MethodKind) string {
	switch kind {
	case measurer.Measure:
		return "Measure"
	case measurer.MeasureOutOfLine:
		return "MeasureOutOfLine"
	case measurer.MeasureHandles:
		return "MeasureHandles"
	default:
		log.Panicf("should not be reachable for kind %v", kind)
		return ""
	}
}

func fmtType(name fidlcommon.Name) string {
	return fmt.Sprintf("::%s::%s", strings.Join(name.LibraryName().Parts(), "::"), name.DeclarationName())
}

func fmtKnownVariant(name fidlcommon.Name, variant string) string {
	return fmt.Sprintf("%s::Tag::k%s", fmtType(name), fidlcommon.ToUpperCamelCase(variant))
}

func fmtUnknownVariant(name fidlcommon.Name) string {
	return fmt.Sprintf("%s::Tag::Invalid", fmtType(name))
}

type formatExpr struct {
	measurer.Expression
}

func (val formatExpr) String() string {
	return val.Fmt(val)
}

var _ measurer.ExpressionFormatter = formatExpr{}

func (formatExpr) CaseNum(num int) string {
	return fmt.Sprintf("%d", num)
}

func (formatExpr) CaseLocal(name string, _ measurer.TapeKind) string {
	return name
}

func (formatExpr) CaseMemberOf(val measurer.Expression, member string) string {
	var accessor string
	if kind := val.AssertKind(measurer.Struct, measurer.Union, measurer.Table); kind != measurer.Struct {
		accessor = "()"
	}
	return fmt.Sprintf("%s%s%s%s", formatExpr{val}, getDerefOp(val), fidlcommon.ToSnakeCase(member), accessor)
}

func (formatExpr) CaseFidlAlign(val measurer.Expression) string {
	return fmt.Sprintf("FIDL_ALIGN(%s)", formatExpr{val})
}

func (formatExpr) CaseLength(val measurer.Expression) string {
	var op string
	switch val.AssertKind(measurer.String, measurer.Vector) {
	case measurer.String:
		op = "length"
	case measurer.Vector:
		op = "size"
	}
	return fmt.Sprintf("%s%s%s()", formatExpr{val}, getDerefOp(val), op)
}

func (formatExpr) CaseHasMember(val measurer.Expression, member string) string {
	return fmt.Sprintf("%s%shas_%s()", formatExpr{val}, getDerefOp(val), fidlcommon.ToSnakeCase(member))
}

func (formatExpr) CaseMult(lhs, rhs measurer.Expression) string {
	return fmt.Sprintf("%s * %s", formatExpr{lhs}, formatExpr{rhs})
}

func getDerefOp(val measurer.Expression) string {
	if val.Nullable() {
		return "->"
	}
	return "."
}

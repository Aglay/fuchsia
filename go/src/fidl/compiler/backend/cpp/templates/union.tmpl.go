// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Union = `
{{- define "UnionForwardDeclaration" }}
class {{ .Name }};
{{- end }}

{{- define "UnionDeclaration" }}
class {{ .Name }} {
 public:
  {{ .Name }}();
  ~{{ .Name }}();

  {{ .Name }}({{ .Name }}&&);
  {{ .Name }}& operator=({{ .Name }}&&);

  void Encode(::fidl::Encoder* encoder, size_t offset);
  static void Decode(::fidl::Decoder* decoder, {{ .Name }}* value, size_t offset);
  zx_status_t Clone({{ .Name }}* result) const;

 private:
  ::fidl_union_tag_t tag;
  union {
    {{- range .Members }}
    {{ .Type.Decl }} {{ .Name }};
    {{- end }}
  };
};
{{- end }}

{{- define "UnionDefinition" }}
{{ .Name }}::{{ .Name }}() {}

{{ .Name }}::~{{ .Name }}() {}

{{ .Name }}::{{ .Name }}({{ .Name }}&&) {}

{{ .Name }}& {{ .Name }}::operator=({{ .Name }}&&) {
  return *this;
}

void {{ .Name }}::Encode(::fidl::Encoder* encoder, size_t offset) {
  ::fidl::Encode(encoder, &tag, offset);
  switch (tag) {
  {{- range $index, $member := .Members }}
   case {{ $index }}:
    ::fidl::Encode(encoder, &{{ .Name }}, offset + {{ .Offset }});
    break;
  {{- end }}
  }
}

void {{ .Name }}::Decode(::fidl::Decoder* decoder, {{ .Name }}* value, size_t offset) {
  ::fidl::Decode(decoder, &value->tag, offset);
  switch (value->tag) {
  {{- range $index, $member := .Members }}
   case {{ $index }}:
    ::fidl::Decode(decoder, &value->{{ .Name }}, offset + {{ .Offset }});
    break;
  {{- end }}
  }
}

zx_status_t {{ .Name }}::Clone({{ .Name }}* result) const {
  result->tag = tag;
  switch (tag) {
    {{- range $index, $member := .Members }}
     case {{ $index }}:
      return ::fidl::Clone({{ .Name }}, &result->{{ .Name }});
    {{- end }}
     default:
      return ZX_ERR_INVALID_ARGS;
  }
}
{{- end }}

{{- define "UnionTraits" }}
template <>
struct CodingTraits<{{ .Namespace }}::{{ .Name }}>
    : public EncodableCodingTraits<{{ .Namespace }}::{{ .Name }}, {{ .Size }}> {};

inline zx_status_t Clone(const {{ .Namespace }}::{{ .Name }}& value,
                         {{ .Namespace }}::{{ .Name }}* result) {
  return value.Clone(result);
}
{{- end }}
`

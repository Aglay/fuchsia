// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Struct = `
{{- define "StructSizeAndAlloc" }}
template<>
struct MinSize<{{ .Name }}> {
  operator size_t() {
    return {{ range $index, $member := .Members }}
      {{- if $index }} + {{ end }}MinSize<{{ $member.Type.Decl }}>()
    {{- end }};
  }
};
template<>
struct Allocate<{{ .Name }}> {
  {{ .Name }} operator()(FuzzInput* src, size_t* size) {
    ZX_ASSERT(*size >= MinSize<{{ .Name }}>());
    const size_t slack_per_member = (*size - MinSize<{{ .Name }}>()) / {{ len .Members }};
    {{ .Name }} out;
    size_t out_size;
    {{- range .Members }}
    out_size = MinSize<{{ .Type.Decl }}>() + slack_per_member;
    out.{{ .Name }} = Allocate<{{ .Type.Decl }}>{}(src, &out_size);
    {{- end }}
    return out;
  }
};
{{- end }}
`

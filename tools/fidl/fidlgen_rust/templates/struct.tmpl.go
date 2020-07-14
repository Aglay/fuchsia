// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Struct = `
{{- define "StructDeclaration" }}

{{- if .Members }}

{{- range .DocComments}}
///{{ . }}
{{- end}}
{{ .Derives }}
{{ if .UseFidlStructCopy -}}
#[repr(C)]
{{- end }}
pub struct {{ .Name }} {
  {{- range .Members }}
  {{- range .DocComments}}
  ///{{ . }}
  {{- end}}
  pub {{ .Name }}: {{ .Type }},
  {{- end }}
}

{{ if .UseFidlStructCopy -}}
fidl_struct_copy! {
{{- else -}}
fidl_struct! {
{{- end }}
  name: {{ .Name }},
  members: [
  {{- range .Members }}
    {{ .Name }} {
      ty: {{ .Type }},
      offset_v1: {{ .Offset }},
    },
  {{- end }}
  ],
  {{ if not .UseFidlStructCopy -}}
  padding: [
  {{- range .PaddingMarkers }}
  {
      ty: {{ .Type }},
      offset: {{ .Offset }},
      mask: {{ .Mask }},
  },
  {{- end }}
  ],
  {{- end }}
  size_v1: {{ .Size }},
  align_v1: {{ .Alignment }},
}
{{- else }}

fidl_empty_struct!(
	{{- range .DocComments}}
	///{{ . }}
	{{- end}}
	{{ .Name }}
);

{{- end }}
{{- end }}
`

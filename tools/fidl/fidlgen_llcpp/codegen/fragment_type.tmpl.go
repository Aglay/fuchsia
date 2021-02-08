// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fragmentTypeTmpl = `
{{- define "ArgumentName" -}}
  {{- if .Access -}}
    {{ .ArgumentName }}()
  {{- else if .MutableAccess -}}
    mutable_{{ .ArgumentName }}()
  {{- else -}}
    {{ .ArgumentName }}
  {{- end -}}
{{- end -}}

{{- define "ArgumentValue" -}}
  {{- if .Access -}}
    {{ .ArgumentName }}()
  {{- else if .MutableAccess -}}
    mutable_{{ .ArgumentName }}()
  {{- else -}}
    {{ .ArgumentValue }}
  {{- end -}}
{{- end -}}

{{- define "TypeCloseHandles" }}
  {{- if or (eq .ArgumentType.Kind TypeKinds.Handle) (eq .ArgumentType.Kind TypeKinds.Request) (eq .ArgumentType.Kind TypeKinds.Protocol)}}
    {{- if .Pointer }}
      {{- if .Nullable }}
      if ({{- template "ArgumentName" . }} != nullptr) {
        {{- template "ArgumentName" . }}->reset();
      }
      {{- else }}
      {{- template "ArgumentName" . }}->reset();
      {{- end }}
    {{- else }}
      {{- template "ArgumentName" . }}.reset();
    {{- end }}
  {{- else if eq .ArgumentType.Kind TypeKinds.Array }}
    {
      {{ .ArgumentType.ElementType.Wire }}* {{ .ArgumentName }}_element = {{ template "ArgumentValue" . }}.data();
      for (size_t i = 0; i < {{ template "ArgumentValue" . }}.size(); ++i, ++{{ .ArgumentName }}_element) {
        {{- template "TypeCloseHandles" NewTypedArgumentElement .ArgumentName .ArgumentType.ElementType }}
      }
    }
  {{- else if eq .ArgumentType.Kind TypeKinds.Vector }}
    {
      {{ .ArgumentType.ElementType.Wire }}* {{ .ArgumentName }}_element = {{ template "ArgumentValue" . }}.mutable_data();
      for (uint64_t i = 0; i < {{ template "ArgumentValue" . }}.count(); ++i, ++{{ .ArgumentName }}_element) {
        {{- template "TypeCloseHandles" NewTypedArgumentElement .ArgumentName .ArgumentType.ElementType }}
      }
    }
  {{- else if .Pointer }}
    {{- if .Nullable }}
    if ({{- template "ArgumentName" . }} != nullptr) {
      {{- template "ArgumentName" . }}->_CloseHandles();
    }
    {{- else }}
    {{- template "ArgumentName" . }}->_CloseHandles();
    {{- end }}
  {{- else }}
    {{- template "ArgumentName" . }}._CloseHandles();
  {{- end }}
{{- end }}
`

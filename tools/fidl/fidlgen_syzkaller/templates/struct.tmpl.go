// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package templates

const Struct = `
{{- define "StructDefinition" -}}

{{ .Name }} {
       {{- range .Members }}
       {{ .Name }} {{ .Type }}
       {{- end }}
} [packed]

{{- end -}}
`

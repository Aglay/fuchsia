// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fragments

const Helpers = `
{{- define "SetTransactionHeaderForResponse" }}
  {{ .LLProps.InterfaceName }}::SetTransactionHeaderFor::{{ .Name }}Response(
      ::fidl::DecodedMessage<{{ .Name }}Response>(
          ::fidl::BytePart(reinterpret_cast<uint8_t*>(&_response),
              {{ .Name }}Response::PrimarySize,
              {{ .Name }}Response::PrimarySize)));
{{- end }}

{{- define "SetTransactionHeaderForRequestMethodSignature" }}
{{- $interface_name := .LLProps.InterfaceName -}}
{{- .Name }}Request(const ::fidl::DecodedMessage<{{ $interface_name}}::{{ .Name }}Request>& _msg)
{{- end }}

{{- define "SetTransactionHeaderForRequestMethodDefinition" -}}
{{- $interface_name := .LLProps.InterfaceName -}}
void {{ $interface_name }}::SetTransactionHeaderFor::{{ template "SetTransactionHeaderForRequestMethodSignature" . }} {
  ::fidl::InitializeTransactionHeader(&_msg.message()->_hdr);
  _msg.message()->_hdr.ordinal = {{ .Ordinals.Write.Name }};
}
{{- end }}

{{- define "SetTransactionHeaderForResponseMethodSignature" }}
{{- $interface_name := .LLProps.InterfaceName -}}
{{- .Name }}Response(const ::fidl::DecodedMessage<{{ $interface_name}}::{{ .Name }}Response>& _msg)
{{- end }}

{{- define "SetTransactionHeaderForResponseMethodDefinition" -}}
{{- $interface_name := .LLProps.InterfaceName -}}
void {{ $interface_name }}::SetTransactionHeaderFor::{{ template "SetTransactionHeaderForResponseMethodSignature" . }} {
  ::fidl::InitializeTransactionHeader(&_msg.message()->_hdr);
  _msg.message()->_hdr.ordinal = {{ .Ordinals.Write.Name }};
}
{{- end }}
`

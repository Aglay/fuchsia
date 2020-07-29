// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fragments

const SyncRequestCallerAllocate = `
{{- define "CallerBufferParams" -}}
{{- if . -}}
::fidl::BytePart _request_buffer, {{ range $index, $param := . -}}
    {{- if $index }}, {{ end -}}{{ $param.Type.LLDecl }} {{ $param.Name }}
  {{- end -}}
{{- end -}}
{{- end }}

{{- define "SyncRequestCallerAllocateMethodArguments" -}}
{{ template "CallerBufferParams" .Request }}{{ if .HasResponse }}{{ if .Request }}, {{ end }}::fidl::BytePart _response_buffer{{ end }}
{{- end }}

{{- define "StaticCallSyncRequestCallerAllocateMethodArguments" -}}
::zx::unowned_channel _client_end{{ if .Request }}, {{ end }}{{ template "CallerBufferParams" .Request }}{{ if .HasResponse }}, ::fidl::BytePart _response_buffer{{ end }}
{{- end }}

{{- define "SyncRequestCallerAllocateMethodDefinition" }}
{{ .LLProps.ProtocolName }}::UnownedResultOf::{{ .Name }}::{{ .Name }}(
  zx_handle_t _client
  {{- if .Request -}}
  , uint8_t* _request_bytes, uint32_t _request_byte_capacity
  {{- end -}}
  {{- template "CommaMessagePrototype" .Request }}
  {{- if .HasResponse }}
  , uint8_t* _response_bytes, uint32_t _response_byte_capacity)
    : bytes_(_response_bytes) {
  {{- else }}
  ) {
  {{- end }}
  {{- if .Request -}}
  {{ .Name }}UnownedRequest _request(_request_bytes, _request_byte_capacity, 0
  {{- else -}}
  {{ .Name }}OwnedRequest _request(0
  {{- end -}}
    {{- template "CommaPassthroughMessageParams" .Request -}});
  {{- if .HasResponse }}
  _request.GetFidlMessage().Call({{ .Name }}Response::Type, _client, _response_bytes, _response_byte_capacity);
  {{- else }}
  _request.GetFidlMessage().Write(_client);
  {{- end }}
  status_ = _request.status();
  error_ = _request.error();
}

{{ .LLProps.ProtocolName }}::UnownedResultOf::{{ .Name }} {{ .LLProps.ProtocolName }}::SyncClient::{{ .Name }}(
  {{- template "SyncRequestCallerAllocateMethodArguments" . }}) {
  return UnownedResultOf::{{ .Name }}(this->channel().get()
    {{- if .Request -}}
      , _request_buffer.data(), _request_buffer.capacity()
    {{- end -}}
      {{- template "CommaPassthroughMessageParams" .Request -}}
    {{- if .HasResponse -}}
      , _response_buffer.data(), _response_buffer.capacity()
    {{- end -}});
}
{{- end }}

{{- define "StaticCallSyncRequestCallerAllocateMethodDefinition" }}
{{ .LLProps.ProtocolName }}::UnownedResultOf::{{ .Name }} {{ .LLProps.ProtocolName }}::Call::{{ .Name }}(
  {{- template "StaticCallSyncRequestCallerAllocateMethodArguments" . }}) {
  return UnownedResultOf::{{ .Name }}(_client_end->get()
    {{- if .Request -}}
      , _request_buffer.data(), _request_buffer.capacity()
    {{- end -}}
      {{- template "CommaPassthroughMessageParams" .Request -}}
    {{- if .HasResponse -}}
      , _response_buffer.data(), _response_buffer.capacity()
    {{- end -}});
}
{{- end }}
`

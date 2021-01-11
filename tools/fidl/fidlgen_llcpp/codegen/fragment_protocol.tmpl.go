// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package codegen

const fragmentProtocolTmpl = `
{{- define "ArgumentDeclaration" -}}
  {{- if eq .Type.LLFamily FamilyKinds.TrivialCopy }}
  {{ .Type.LLDecl }} {{ .Name }}
  {{- else if eq .Type.LLFamily FamilyKinds.Reference }}
  {{ .Type.LLDecl }}& {{ .Name }}
  {{- else if eq .Type.LLFamily FamilyKinds.String }}
  const {{ .Type.LLDecl }}& {{ .Name }}
  {{- else if eq .Type.LLFamily FamilyKinds.Vector }}
  {{ .Type.LLDecl }}& {{ .Name }}
  {{- end }}
{{- end }}

{{- /* Defines the arguments for a response method/constructor. */}}
{{- define "MessagePrototype" -}}
  {{- range $index, $param := . -}}
    {{- if $index }}, {{ end}}{{ template "ArgumentDeclaration" $param }}
  {{- end -}}
{{- end }}

{{- /* Defines the arguments for a request method/constructor. */}}
{{- define "CommaMessagePrototype" -}}
  {{- range $param := . -}}
    , {{ template "ArgumentDeclaration" $param }}
  {{- end -}}
{{- end }}

{{- /* Defines the initialization of all the fields for a message constructor. */}}
{{- define "InitMessage" -}}
  {{- range $index, $param := . }}
  {{- if $index }}, {{- else }}: {{- end}}
    {{- if eq $param.Type.LLFamily FamilyKinds.TrivialCopy }}
      {{ $param.Name }}({{ $param.Name }})
    {{- else if eq $param.Type.LLFamily FamilyKinds.Reference }}
      {{ $param.Name }}(std::move({{ $param.Name }}))
    {{- else if eq $param.Type.LLFamily FamilyKinds.String }}
      {{ $param.Name }}(::fidl::unowned_ptr_t<const char>({{ $param.Name }}.data()), {{ $param.Name }}.size())
    {{- else if eq $param.Type.LLFamily FamilyKinds.Vector }}
      {{ $param.Name }}(::fidl::unowned_ptr_t<{{ $param.Type.ElementType.LLDecl }}>({{ $param.Name }}.mutable_data()), {{ $param.Name }}.count())
    {{- end }}
  {{- end }}
{{- end }}

{{- define "ProtocolForwardDeclaration" }}
class {{ .Name }};
{{- end }}

{{- define "RequestCodingTable" -}}
&{{ .RequestTypeName }}
{{- end }}

{{- define "ResponseCodingTable" -}}
&{{ .ResponseTypeName }}
{{- end }}

{{- /* All the parameters for a method which value content. */}}
{{- define "ForwardParams" -}}
  {{- range $index, $param := . -}}
    {{- if $index }}, {{ end -}} std::move({{ $param.Name }})
  {{- end -}}
{{- end }}

{{- define "CommaForwardParams" -}}
  {{- range $index, $param := . -}}
    , std::move({{ $param.Name }})
  {{- end -}}
{{- end }}

{{- define "ForwardMessageParamsUnwrapTypedChannels" -}}
  {{- range $index, $param := . -}}
    {{- if $index }}, {{ end -}} std::move(
      {{- if or (eq .Type.Kind TypeKinds.Protocol) (eq .Type.Kind TypeKinds.Request) -}}
        {{ $param.Name }}.channel()
      {{- else -}}
        {{ $param.Name }}
      {{- end -}}
    )
  {{- end -}}
{{- end }}

{{- /* All the parameters for a response method/constructor which uses values with references */}}
{{- /* or trivial copy. */}}
{{- define "PassthroughMessageParams" -}}
  {{- range $index, $param := . }}
    {{- if $index }}, {{- end }} {{ $param.Name }}
  {{- end }}
{{- end }}

{{- /* All the parameters for a request method/constructor which uses values with references */}}
{{- /* or trivial copy. */}}
{{- define "CommaPassthroughMessageParams" -}}
  {{- range $index, $param := . }}
    , {{ $param.Name }}
  {{- end }}
{{- end }}

{{- define "ClientAllocationComment" -}}
{{- $context := .LLProps.ClientContext }}
{{- if StackUse $context }} Allocates {{ StackUse $context }} bytes of {{ "" }}
{{- if not $context.StackAllocRequest -}} response {{- else -}}
  {{- if not $context.StackAllocResponse -}} request {{- else -}} message {{- end -}}
{{- end }} buffer on the stack. {{- end }}
{{- if and $context.StackAllocRequest $context.StackAllocResponse }} No heap allocation necessary.
{{- else }}
  {{- if not $context.StackAllocRequest }} Request is heap-allocated. {{- end }}
  {{- if not $context.StackAllocResponse }} Response is heap-allocated. {{- end }}
{{- end }}
{{- end }}

{{- define "RequestSentSize"}}
  {{- if gt .RequestSentMaxSize 65536 -}}
  ZX_CHANNEL_MAX_MSG_BYTES
  {{- else -}}
  PrimarySize + MaxOutOfLine
  {{- end -}}
{{- end }}

{{- define "ResponseSentSize"}}
  {{- if gt .ResponseSentMaxSize 65536 -}}
  ZX_CHANNEL_MAX_MSG_BYTES
  {{- else -}}
  PrimarySize + MaxOutOfLine
  {{- end -}}
{{- end }}

{{- define "ResponseReceivedSize"}}
  {{- if gt .ResponseReceivedMaxSize 65536 -}}
  ZX_CHANNEL_MAX_MSG_BYTES
  {{- else -}}
  {{ .Name }}Response::PrimarySize + {{ .Name }}Response::MaxOutOfLine
  {{- end -}}
{{- end }}

{{- define "ResponseReceivedByteAccess" }}
  {{- if gt .ResponseReceivedMaxSize 512 -}}
  bytes_->data()
  {{- else -}}
  bytes_
  {{- end -}}
{{- end }}

{{- define "ProtocolDeclaration" }}
{{- $protocol := . }}
{{ "" }}
  {{- range .Methods }}
extern "C" const fidl_type_t {{ .RequestTypeName }};
extern "C" const fidl_type_t {{ .ResponseTypeName }};
  {{- end }}
{{ "" }}

{{- range .DocComments }}
//{{ . }}
{{- end }}
class {{ .Name }} final {
  {{ .Name }}() = delete;
 public:
{{- if .ServiceName }}
  static constexpr char Name[] = {{ .ServiceName }};
{{- end }}
{{ "" }}
  {{- range .Methods }}

    {{- if .HasResponse }}
  struct {{ .Name }}Response final {
    FIDL_ALIGNDECL
        {{- /* Add underscore to prevent name collision */}}
    fidl_message_header_t _hdr;
        {{- range $index, $param := .Response }}
    {{ $param.Type.LLDecl }} {{ $param.Name }};
        {{- end }}

    {{- if .Response }}
    explicit {{ .Name }}Response({{ template "MessagePrototype" .Response }})
    {{ template "InitMessage" .Response }} {
      _InitHeader();
    }
    {{- end }}
    {{ .Name }}Response() {
      _InitHeader();
    }

    static constexpr const fidl_type_t* Type =
    {{- if .Response }}
      {{ template "ResponseCodingTable" . }};
    {{- else }}
      &::fidl::_llcpp_coding_AnyZeroArgMessageTable;
    {{- end }}
    static constexpr uint32_t MaxNumHandles = {{ .ResponseMaxHandles }};
    static constexpr uint32_t PrimarySize = {{ .ResponseSize }};
    static constexpr uint32_t MaxOutOfLine = {{ .ResponseMaxOutOfLine }};
    static constexpr bool HasFlexibleEnvelope = {{ .ResponseFlexible }};
    static constexpr bool HasPointer = {{ .ResponseHasPointer }};
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kResponse;

    {{- if .ResponseIsResource }}
    void _CloseHandles();
    {{- end }}

    class UnownedEncodedMessage final {
     public:
      UnownedEncodedMessage(uint8_t* _bytes, uint32_t _byte_size
        {{- template "CommaMessagePrototype" .Response }})
          : message_(_bytes, _byte_size, sizeof({{ .Name }}Response),
      {{- if gt .ResponseMaxHandles 0 }}
        handles_, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles), 0
      {{- else }}
        nullptr, 0, 0
      {{- end }}
        ) {
        FIDL_ALIGNDECL {{ .Name }}Response _response{
            {{- template "PassthroughMessageParams" .Response -}}
        };
        message_.LinearizeAndEncode<{{ .Name }}Response>(&_response);
      }
      UnownedEncodedMessage(uint8_t* bytes, uint32_t byte_size, {{ .Name }}Response* response)
          : message_(bytes, byte_size, sizeof({{ .Name }}Response),
      {{- if gt .ResponseMaxHandles 0 }}
        handles_, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles), 0
      {{- else }}
        nullptr, 0, 0
      {{- end }}
        ) {
        message_.LinearizeAndEncode<{{ .Name }}Response>(response);
      }
      UnownedEncodedMessage(const UnownedEncodedMessage&) = delete;
      UnownedEncodedMessage(UnownedEncodedMessage&&) = delete;
      UnownedEncodedMessage* operator=(const UnownedEncodedMessage&) = delete;
      UnownedEncodedMessage* operator=(UnownedEncodedMessage&&) = delete;

      zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
      const char* status_string() const { return message_.status_string(); }
#endif
      bool ok() const { return message_.status() == ZX_OK; }
      const char* error() const { return message_.error(); }

      ::fidl::OutgoingMessage& GetOutgoingMessage() { return message_; }

      template <typename ChannelLike>
      void Write(ChannelLike&& client) { message_.Write(std::forward<ChannelLike>(client)); }

     private:
      {{ .Name }}Response& Message() { return *reinterpret_cast<{{ .Name }}Response*>(message_.bytes()); }

      {{- if gt .ResponseMaxHandles 0 }}
        zx_handle_disposition_t handles_[std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles)];
      {{- end }}
      ::fidl::OutgoingMessage message_;
    };

    class OwnedEncodedMessage final {
     public:
      explicit OwnedEncodedMessage(
        {{- template "MessagePrototype" .Response }})
          {{- if gt .ResponseSentMaxSize 512 -}}
        : bytes_(std::make_unique<::fidl::internal::AlignedBuffer<{{- template "ResponseSentSize" .}}>>()),
          message_(bytes_->data(), {{- template "ResponseSentSize" .}}
          {{- else }}
          : message_(bytes_, sizeof(bytes_)
          {{- end }}
          {{- template "CommaPassthroughMessageParams" .Response }}) {}
      explicit OwnedEncodedMessage({{ .Name }}Response* response)
          {{- if gt .ResponseSentMaxSize 512 -}}
        : bytes_(std::make_unique<::fidl::internal::AlignedBuffer<{{- template "ResponseSentSize" .}}>>()),
          message_(bytes_->data(), {{- template "ResponseSentSize" .}}
          {{- else }}
          : message_(bytes_, sizeof(bytes_)
          {{- end }}
          , response) {}
      OwnedEncodedMessage(const OwnedEncodedMessage&) = delete;
      OwnedEncodedMessage(OwnedEncodedMessage&&) = delete;
      OwnedEncodedMessage* operator=(const OwnedEncodedMessage&) = delete;
      OwnedEncodedMessage* operator=(OwnedEncodedMessage&&) = delete;

      zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
      const char* status_string() const { return message_.status_string(); }
#endif
      bool ok() const { return message_.ok(); }
      const char* error() const { return message_.error(); }

      ::fidl::OutgoingMessage& GetOutgoingMessage() { return message_.GetOutgoingMessage(); }

      template <typename ChannelLike>
      void Write(ChannelLike&& client) { message_.Write(std::forward<ChannelLike>(client)); }

     private:
      {{- if gt .ResponseSentMaxSize 512 }}
      std::unique_ptr<::fidl::internal::AlignedBuffer<{{- template "ResponseSentSize" .}}>> bytes_;
      {{- else }}
      FIDL_ALIGNDECL
      uint8_t bytes_[PrimarySize + MaxOutOfLine];
      {{- end }}
      UnownedEncodedMessage message_;
    };

    class DecodedMessage final : public ::fidl::internal::IncomingMessage {
     public:
      DecodedMessage(uint8_t* bytes, uint32_t byte_actual, zx_handle_info_t* handles = nullptr,
                      uint32_t handle_actual = 0)
          : ::fidl::internal::IncomingMessage(bytes, byte_actual, handles, handle_actual) {
        Decode<{{ .Name }}Response>();
      }
      DecodedMessage(fidl_incoming_msg_t* msg) : ::fidl::internal::IncomingMessage(msg) {
        Decode<{{ .Name }}Response>();
      }
      DecodedMessage(const DecodedMessage&) = delete;
      DecodedMessage(DecodedMessage&&) = delete;
      DecodedMessage* operator=(const DecodedMessage&) = delete;
      DecodedMessage* operator=(DecodedMessage&&) = delete;
      {{- if .ResponseIsResource }}
      ~DecodedMessage() {
        if (ok() && (PrimaryObject() != nullptr)) {
          PrimaryObject()->_CloseHandles();
        }
      }
      {{- end }}

      {{ .Name }}Response* PrimaryObject() {
        ZX_DEBUG_ASSERT(ok());
        return reinterpret_cast<{{ .Name }}Response*>(bytes());
      }

      // Release the ownership of the decoded message. That means that the handles won't be closed
      // When the object is destroyed.
      // After calling this method, the DecodedMessage object should not be used anymore.
      void ReleasePrimaryObject() { ResetBytes(); }

      // These methods should only be used for testing purpose.
      // They create an DecodedMessage using the bytes of an outgoing message and copying the
      // handles.
      static DecodedMessage FromOutgoingWithRawHandleCopy(UnownedEncodedMessage* outgoing_message) {
        return DecodedMessage(outgoing_message->GetOutgoingMessage());
      }
      static DecodedMessage FromOutgoingWithRawHandleCopy(OwnedEncodedMessage* outgoing_message) {
        return DecodedMessage(outgoing_message->GetOutgoingMessage());
      }

     private:
      DecodedMessage(::fidl::OutgoingMessage& outgoing_message) {
      {{- if gt .ResponseMaxHandles 0 }}
        zx_handle_info_t handles[std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles)];
        Init(outgoing_message, handles, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles));
      {{- else }}
        Init(outgoing_message, nullptr, 0);
      {{- end }}
        if (ok()) {
          Decode<{{ .Name }}Response>();
        }
      }
    };

   private:
    void _InitHeader();
  };
    {{- end }}

    {{- if .HasRequest }}
  struct {{ .Name }}Request final {
    FIDL_ALIGNDECL
        {{- /* Add underscore to prevent name collision */}}
    fidl_message_header_t _hdr;
        {{- range $index, $param := .Request }}
    {{ $param.Type.LLDecl }} {{ $param.Name }};
        {{- end }}

    {{- if .Request }}
    explicit {{ .Name }}Request(zx_txid_t _txid {{- template "CommaMessagePrototype" .Request }})
    {{ template "InitMessage" .Request }} {
      _InitHeader(_txid);
    }
    {{- end }}
    explicit {{ .Name }}Request(zx_txid_t _txid) {
      _InitHeader(_txid);
    }

    static constexpr const fidl_type_t* Type =
    {{- if .Request }}
      {{ template "RequestCodingTable" . }};
    {{- else }}
      &::fidl::_llcpp_coding_AnyZeroArgMessageTable;
    {{- end }}
    static constexpr uint32_t MaxNumHandles = {{ .RequestMaxHandles }};
    static constexpr uint32_t PrimarySize = {{ .RequestSize }};
    static constexpr uint32_t MaxOutOfLine = {{ .RequestMaxOutOfLine }};
    static constexpr uint32_t AltPrimarySize = {{ .RequestSize }};
    static constexpr uint32_t AltMaxOutOfLine = {{ .RequestMaxOutOfLine }};
    static constexpr bool HasFlexibleEnvelope = {{ .RequestFlexible }};
    static constexpr bool HasPointer = {{ .RequestHasPointer }};
    static constexpr ::fidl::internal::TransactionalMessageKind MessageKind =
        ::fidl::internal::TransactionalMessageKind::kRequest;

        {{- if and .HasResponse .Response }}
    using ResponseType = {{ .Name }}Response;
        {{- end }}

    {{- if .RequestIsResource }}
    void _CloseHandles();
    {{- end }}

    class UnownedEncodedMessage final {
     public:
      UnownedEncodedMessage(uint8_t* _bytes, uint32_t _byte_size, zx_txid_t _txid
        {{- template "CommaMessagePrototype" .Request }})
          : message_(_bytes, _byte_size, sizeof({{ .Name }}Request),
      {{- if gt .RequestMaxHandles 0 }}
        handles_, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles), 0
      {{- else }}
        nullptr, 0, 0
      {{- end }}
        ) {
        FIDL_ALIGNDECL {{ .Name }}Request _request(_txid
            {{- template "CommaPassthroughMessageParams" .Request -}}
        );
        message_.LinearizeAndEncode<{{ .Name }}Request>(&_request);
      }
      UnownedEncodedMessage(uint8_t* bytes, uint32_t byte_size, {{ .Name }}Request* request)
          : message_(bytes, byte_size, sizeof({{ .Name }}Request),
      {{- if gt .RequestMaxHandles 0 }}
        handles_, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles), 0
      {{- else }}
        nullptr, 0, 0
      {{- end }}
        ) {
        message_.LinearizeAndEncode<{{ .Name }}Request>(request);
      }
      UnownedEncodedMessage(const UnownedEncodedMessage&) = delete;
      UnownedEncodedMessage(UnownedEncodedMessage&&) = delete;
      UnownedEncodedMessage* operator=(const UnownedEncodedMessage&) = delete;
      UnownedEncodedMessage* operator=(UnownedEncodedMessage&&) = delete;

      zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
      const char* status_string() const { return message_.status_string(); }
#endif
      bool ok() const { return message_.status() == ZX_OK; }
      const char* error() const { return message_.error(); }

      ::fidl::OutgoingMessage& GetOutgoingMessage() { return message_; }

      template <typename ChannelLike>
      void Write(ChannelLike&& client) { message_.Write(std::forward<ChannelLike>(client)); }

     private:
      {{ .Name }}Request& Message() { return *reinterpret_cast<{{ .Name }}Request*>(message_.bytes()); }

      {{- if gt .RequestMaxHandles 0 }}
        zx_handle_disposition_t handles_[std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles)];
      {{- end }}
      ::fidl::OutgoingMessage message_;
    };

    class OwnedEncodedMessage final {
     public:
      explicit OwnedEncodedMessage(zx_txid_t _txid
        {{- template "CommaMessagePrototype" .Request }})
          {{- if gt .RequestSentMaxSize 512 -}}
        : bytes_(std::make_unique<::fidl::internal::AlignedBuffer<{{- template "RequestSentSize" .}}>>()),
          message_(bytes_->data(), {{- template "RequestSentSize" .}}, _txid
          {{- else }}
          : message_(bytes_, sizeof(bytes_), _txid
          {{- end }}
          {{- template "CommaPassthroughMessageParams" .Request }}) {}
      explicit OwnedEncodedMessage({{ .Name }}Request* request)
          {{- if gt .RequestSentMaxSize 512 -}}
        : bytes_(std::make_unique<::fidl::internal::AlignedBuffer<{{- template "RequestSentSize" .}}>>()),
          message_(bytes_->data(), {{- template "RequestSentSize" .}}
          {{- else }}
          : message_(bytes_, sizeof(bytes_)
          {{- end }}
          , request) {}
      OwnedEncodedMessage(const OwnedEncodedMessage&) = delete;
      OwnedEncodedMessage(OwnedEncodedMessage&&) = delete;
      OwnedEncodedMessage* operator=(const OwnedEncodedMessage&) = delete;
      OwnedEncodedMessage* operator=(OwnedEncodedMessage&&) = delete;

      zx_status_t status() const { return message_.status(); }
#ifdef __Fuchsia__
      const char* status_string() const { return message_.status_string(); }
#endif
      bool ok() const { return message_.ok(); }
      const char* error() const { return message_.error(); }

      ::fidl::OutgoingMessage& GetOutgoingMessage() { return message_.GetOutgoingMessage(); }

      template <typename ChannelLike>
      void Write(ChannelLike&& client) { message_.Write(std::forward<ChannelLike>(client)); }

     private:
      {{- if gt .RequestSentMaxSize 512 }}
      std::unique_ptr<::fidl::internal::AlignedBuffer<{{- template "RequestSentSize" .}}>> bytes_;
      {{- else }}
      FIDL_ALIGNDECL
      uint8_t bytes_[PrimarySize + MaxOutOfLine];
      {{- end }}
      UnownedEncodedMessage message_;
    };

    class DecodedMessage final : public ::fidl::internal::IncomingMessage {
     public:
      DecodedMessage(uint8_t* bytes, uint32_t byte_actual, zx_handle_info_t* handles = nullptr,
                      uint32_t handle_actual = 0)
          : ::fidl::internal::IncomingMessage(bytes, byte_actual, handles, handle_actual) {
        Decode<{{ .Name }}Request>();
      }
      DecodedMessage(fidl_incoming_msg_t* msg) : ::fidl::internal::IncomingMessage(msg) {
        Decode<{{ .Name }}Request>();
      }
      DecodedMessage(const DecodedMessage&) = delete;
      DecodedMessage(DecodedMessage&&) = delete;
      DecodedMessage* operator=(const DecodedMessage&) = delete;
      DecodedMessage* operator=(DecodedMessage&&) = delete;
      {{- if .RequestIsResource }}
      ~DecodedMessage() {
        if (ok() && (PrimaryObject() != nullptr)) {
          PrimaryObject()->_CloseHandles();
        }
      }
      {{- end }}

      {{ .Name }}Request* PrimaryObject() {
        ZX_DEBUG_ASSERT(ok());
        return reinterpret_cast<{{ .Name }}Request*>(bytes());
      }

      // Release the ownership of the decoded message. That means that the handles won't be closed
      // When the object is destroyed.
      // After calling this method, the DecodedMessage object should not be used anymore.
      void ReleasePrimaryObject() { ResetBytes(); }

      // These methods should only be used for testing purpose.
      // They create an DecodedMessage using the bytes of an outgoing message and copying the
      // handles.
      static DecodedMessage FromOutgoingWithRawHandleCopy(UnownedEncodedMessage* encoded_message) {
        return DecodedMessage(encoded_message->GetOutgoingMessage());
      }
      static DecodedMessage FromOutgoingWithRawHandleCopy(OwnedEncodedMessage* encoded_message) {
        return DecodedMessage(encoded_message->GetOutgoingMessage());
      }

     private:
      DecodedMessage(::fidl::OutgoingMessage& outgoing_message) {
      {{- if gt .RequestMaxHandles 0 }}
        zx_handle_info_t handles[std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles)];
        Init(outgoing_message, handles, std::min(ZX_CHANNEL_MAX_MSG_HANDLES, MaxNumHandles));
      {{- else }}
        Init(outgoing_message, nullptr, 0);
      {{- end }}
        if (ok()) {
          Decode<{{ .Name }}Request>();
        }
      }
    };

   private:
    void _InitHeader(zx_txid_t _txid);
  };
{{ "" }}
    {{- end }}

  {{- end }}

  class EventHandlerInterface {
   public:
    EventHandlerInterface() = default;
    virtual ~EventHandlerInterface() = default;
    {{- range .Events -}}

      {{- range .DocComments }}
    //{{ . }}
      {{- end }}
    virtual void {{ .Name }}({{ .Name }}Response* event) {}
    {{- end }}
  };
  {{- if .Events }}
{{ "" }}
  class SyncEventHandler : public EventHandlerInterface {
   public:
    SyncEventHandler() = default;

    // Method called when an unknown event is found. This methods gives the status which, in this
    // case, is returned by HandleOneEvent.
    virtual zx_status_t Unknown() = 0;

    // Handle all possible events defined in this protocol.
    // Blocks to consume exactly one message from the channel, then call the corresponding virtual
    // method.
    ::fidl::Result HandleOneEvent(
        ::fidl::UnownedClientEnd<{{ .Namespace }}::{{ .Name }}> client_end);
  };
  {{- end }}

  // Collection of return types of FIDL calls in this protocol.
  class ResultOf final {
    ResultOf() = delete;
   public:
    {{- range .ClientMethods -}}
    {{- if .HasResponse -}}
{{ "" }}
    {{- end }}
    class {{ .Name }} final : public ::fidl::Result {
     public:
      explicit {{ .Name }}(
          ::fidl::UnownedClientEnd<{{ $protocol.Namespace }}::{{ $protocol.Name }}> _client
          {{- template "CommaMessagePrototype" .Request }});
    {{- if .HasResponse }}
      {{ .Name }}(
          ::fidl::UnownedClientEnd<{{ $protocol.Namespace }}::{{ $protocol.Name }}> _client
          {{- template "CommaMessagePrototype" .Request }},
          zx_time_t _deadline);
    {{- end }}
      explicit {{ .Name }}(const ::fidl::Result& result) : ::fidl::Result(result) {}
      {{ .Name }}({{ .Name }}&&) = delete;
      {{ .Name }}(const {{ .Name }}&) = delete;
      {{ .Name }}* operator=({{ .Name }}&&) = delete;
      {{ .Name }}* operator=(const {{ .Name }}&) = delete;
      {{- if and .HasResponse .ResponseIsResource }}
      ~{{ .Name }}() {
        if (ok()) {
          Unwrap()->_CloseHandles();
        }
      }
      {{- else }}
      ~{{ .Name }}() = default;
      {{- end }}
      {{- if .HasResponse }}

      {{ .Name }}Response* Unwrap() {
        ZX_DEBUG_ASSERT(ok());
        return reinterpret_cast<{{ .Name }}Response*>({{- template "ResponseReceivedByteAccess" . }});
      }
      const {{ .Name }}Response* Unwrap() const {
        ZX_DEBUG_ASSERT(ok());
        return reinterpret_cast<const {{ .Name }}Response*>({{- template "ResponseReceivedByteAccess" . }});
      }

      {{ .Name }}Response& value() { return *Unwrap(); }
      const {{ .Name }}Response& value() const { return *Unwrap(); }

      {{ .Name }}Response* operator->() { return &value(); }
      const {{ .Name }}Response* operator->() const { return &value(); }

      {{ .Name }}Response& operator*() { return value(); }
      const {{ .Name }}Response& operator*() const { return value(); }
      {{- end }}

     private:
      {{- if .HasResponse }}
        {{- if gt .ResponseReceivedMaxSize 512 }}
        std::unique_ptr<::fidl::internal::AlignedBuffer<{{ template "ResponseReceivedSize" . }}>> bytes_;
        {{- else }}
        FIDL_ALIGNDECL
        uint8_t bytes_[{{ .Name }}Response::PrimarySize + {{ .Name }}Response::MaxOutOfLine];
        {{- end }}
      {{- end }}
    };
    {{- end }}
  };

  // Collection of return types of FIDL calls in this protocol,
  // when the caller-allocate flavor or in-place call is used.
  class UnownedResultOf final {
    UnownedResultOf() = delete;

   public:
    {{- range .ClientMethods -}}
    class {{ .Name }} final : public ::fidl::Result {
     public:
      explicit {{ .Name }}(
          ::fidl::UnownedClientEnd<{{ $protocol.Namespace }}::{{ $protocol.Name }}> _client
        {{- if .Request -}}
          , uint8_t* _request_bytes, uint32_t _request_byte_capacity
        {{- end -}}
        {{- template "CommaMessagePrototype" .Request }}
        {{- if .HasResponse -}}
          , uint8_t* _response_bytes, uint32_t _response_byte_capacity
        {{- end -}});
      explicit {{ .Name }}(const ::fidl::Result& result) : ::fidl::Result(result) {}
      {{ .Name }}({{ .Name }}&&) = delete;
      {{ .Name }}(const {{ .Name }}&) = delete;
      {{ .Name }}* operator=({{ .Name }}&&) = delete;
      {{ .Name }}* operator=(const {{ .Name }}&) = delete;
      {{- if and .HasResponse .ResponseIsResource }}
      ~{{ .Name }}() {
        if (ok()) {
          Unwrap()->_CloseHandles();
        }
      }
      {{- else }}
      ~{{ .Name }}() = default;
      {{- end }}
      {{- if .HasResponse }}

      {{ .Name }}Response* Unwrap() {
        ZX_DEBUG_ASSERT(ok());
        return reinterpret_cast<{{ .Name }}Response*>(bytes_);
      }
      const {{ .Name }}Response* Unwrap() const {
        ZX_DEBUG_ASSERT(ok());
        return reinterpret_cast<const {{ .Name }}Response*>(bytes_);
      }

      {{ .Name }}Response& value() { return *Unwrap(); }
      const {{ .Name }}Response& value() const { return *Unwrap(); }

      {{ .Name }}Response* operator->() { return &value(); }
      const {{ .Name }}Response* operator->() const { return &value(); }

      {{ .Name }}Response& operator*() { return value(); }
      const {{ .Name }}Response& operator*() const { return value(); }

     private:
      uint8_t* bytes_;
      {{- end }}
    };
    {{- end }}
  };

  // Methods to make a sync FIDL call directly on an unowned channel or a
  // const reference to a |fidl::ClientEnd<{{ .Namespace }}::{{ .Name }}>|,
  // avoiding setting up a client.
  class Call final {
    Call() = delete;
   public:
{{ "" }}
    {{- /* Client-calling functions do not apply to events. */}}
    {{- range .ClientMethods -}}
      {{- range .DocComments }}
    //{{ . }}
      {{- end }}
    //{{ template "ClientAllocationComment" . }}
    static ResultOf::{{ .Name }} {{ .Name }}({{ template "StaticCallSyncRequestManagedMethodArguments" . }}) {
      return ResultOf::{{ .Name }}(_client_end
        {{- template "CommaPassthroughMessageParams" .Request -}}
        );
    }
{{ "" }}
      {{- if or .Request .Response }}
        {{- range .DocComments }}
    //{{ . }}
        {{- end }}
    // Caller provides the backing storage for FIDL message via request and response buffers.
    static UnownedResultOf::{{ .Name }} {{ .Name }}({{ template "StaticCallSyncRequestCallerAllocateMethodArguments" . }}) {
      return UnownedResultOf::{{ .Name }}(_client_end
        {{- if .Request -}}
          , _request_buffer.data, _request_buffer.capacity
        {{- end -}}
          {{- template "CommaPassthroughMessageParams" .Request -}}
        {{- if .HasResponse -}}
          , _response_buffer.data, _response_buffer.capacity
        {{- end -}});
    }
      {{- end }}
{{ "" }}
    {{- end }}
  };

  class SyncClient final {
   public:
    SyncClient() = default;

    explicit SyncClient(::fidl::ClientEnd<{{ .Name }}> client_end)
        : client_end_(std::move(client_end)) {}

    ~SyncClient() = default;
    SyncClient(SyncClient&&) = default;
    SyncClient& operator=(SyncClient&&) = default;

    const ::fidl::ClientEnd<{{ .Name }}>& client_end() const { return client_end_; }
    ::fidl::ClientEnd<{{ .Name }}>& client_end() { return client_end_; }

    const ::zx::channel& channel() const { return client_end_.channel(); }
    ::zx::channel* mutable_channel() { return &client_end_.channel(); }
{{ "" }}
    {{- /* Client-calling functions do not apply to events. */}}
    {{- range .ClientMethods -}}
      {{- range .DocComments }}
    //{{ . }}
      {{- end }}
    //{{ template "ClientAllocationComment" . }}
    ResultOf::{{ .Name }} {{ .Name }}({{ template "SyncRequestManagedMethodArguments" . }}) {
      return ResultOf::{{ .Name }}(this->client_end()
        {{- template "CommaPassthroughMessageParams" .Request -}});
    }
{{ "" }}
      {{- if or .Request .Response }}
        {{- range .DocComments }}
    //{{ . }}
        {{- end }}
    // Caller provides the backing storage for FIDL message via request and response buffers.
    UnownedResultOf::{{ .Name }} {{ .Name }}({{ template "SyncRequestCallerAllocateMethodArguments" . }}) {
      return UnownedResultOf::{{ .Name }}(this->client_end()
        {{- if .Request -}}
          , _request_buffer.data, _request_buffer.capacity
        {{- end -}}
          {{- template "CommaPassthroughMessageParams" .Request -}}
        {{- if .HasResponse -}}
          , _response_buffer.data, _response_buffer.capacity
        {{- end -}});
    }
      {{- end }}
{{ "" }}
    {{- end }}
    {{- if .Events }}
    // Handle all possible events defined in this protocol.
    // Blocks to consume exactly one message from the channel, then call the corresponding virtual
    // method defined in |SyncEventHandler|. The return status of the handler function is folded with
    // any transport-level errors and returned.
    ::fidl::Result HandleOneEvent(SyncEventHandler& event_handler) {
      return event_handler.HandleOneEvent(client_end_);
    }
    {{- end }}
   private:
     ::fidl::ClientEnd<{{ .Name }}> client_end_;
  };

{{ template "ClientForwardDeclaration" . }}

{{ "" }}
  // Pure-virtual interface to be implemented by a server.
  // This interface uses typed channels (i.e. |fidl::ClientEnd<SomeProtocol>|
  // and |fidl::ServerEnd<SomeProtocol>|).
  // TODO(fxbug.dev/65212): Rename this to |Interface| after all users have
  // migrated to the typed channels API.
  class TypedChannelInterface : public ::fidl::internal::IncomingMessageDispatcher {
   public:
    TypedChannelInterface() = default;
    virtual ~TypedChannelInterface() = default;

    // The marker protocol type within which this |TypedChannelInterface| class is defined.
    using _EnclosingProtocol = {{ $protocol.Name }};

{{ "" }}
    {{- range .Methods }}
      {{- if .HasRequest }}
        {{- if .HasResponse }}
    class {{ .Name }}CompleterBase : public ::fidl::CompleterBase {
     public:
      // In the following methods, the return value indicates internal errors during
      // the reply, such as encoding or writing to the transport.
      // Note that any error will automatically lead to the destruction of the binding,
      // after which the |on_unbound| callback will be triggered with a detailed reason.
      //
      // See //zircon/system/ulib/fidl/include/lib/fidl/llcpp/server.h.
      //
      // Because the reply status is identical to the unbinding status, it can be safely ignored.
      ::fidl::Result {{ template "ReplyManagedMethodSignature" . }};
          {{- if .Result }}
      ::fidl::Result {{ template "ReplyManagedResultSuccessMethodSignature" . }};
      ::fidl::Result {{ template "ReplyManagedResultErrorMethodSignature" . }};
          {{- end }}
          {{- if .Response }}
      ::fidl::Result {{ template "ReplyCallerAllocateMethodSignature" . }};
            {{- if .Result }}
      ::fidl::Result {{ template "ReplyCallerAllocateResultSuccessMethodSignature" . }};
            {{- end }}
          {{- end }}

     protected:
      using ::fidl::CompleterBase::CompleterBase;
    };

    using {{ .Name }}Completer = ::fidl::Completer<{{ .Name }}CompleterBase>;
        {{- else }}
    using {{ .Name }}Completer = ::fidl::Completer<>;
        {{- end }}

{{ "" }}
        {{- range .DocComments }}
    //{{ . }}
        {{- end }}
    virtual void {{ .Name }}(
        {{- template "Params" .Request }}{{ if .Request }}, {{ end -}}
        {{- if .Transitional -}}
          {{ .Name }}Completer::Sync& _completer) { _completer.Close(ZX_ERR_NOT_SUPPORTED); }
        {{- else -}}
          {{ .Name }}Completer::Sync& _completer) = 0;
        {{- end }}
{{ "" }}
      {{- end }}
    {{- end }}

   private:
    {{- /* Note that this implementation is snake_case to avoid name conflicts. */}}
    ::fidl::DispatchResult dispatch_message(fidl_incoming_msg_t* msg,
                                            ::fidl::Transaction* txn) final;
  };

  // Pure-virtual interface to be implemented by a server.
  class Interface : public TypedChannelInterface {
   public:
    Interface() = default;
    virtual ~Interface() = default;

    // The marker protocol type within which this |Interface| class is defined.
    using TypedChannelInterface::_EnclosingProtocol;

    {{- range .ClientMethods }}
    using TypedChannelInterface::{{ .Name }}Completer;

{{ "" }}
      {{- if .ShouldEmitTypedChannelCascadingInheritance }}
    virtual void {{ .Name }}(
        {{- template "Params" .Request }}{{ if .Request }}, {{ end -}}
        {{ .Name }}Completer::Sync& _completer) final {
      {{ .Name }}({{ template "ForwardMessageParamsUnwrapTypedChannels" .Request }}
        {{- if .Request }}, {{ end -}} _completer);
    }

    // TODO(fxbug.dev/65212): Overriding this method is discouraged since it
    // uses raw channels instead of |fidl::ClientEnd| and |fidl::ServerEnd|.
    // Please move to overriding the typed channel overload above instead.
    virtual void {{ .Name }}(
      {{- template "ParamsNoTypedChannels" .Request }}{{ if .Request }}, {{ end -}}
        {{- if .Transitional -}}
          {{ .Name }}Completer::Sync& _completer) { _completer.Close(ZX_ERR_NOT_SUPPORTED); }
        {{- else -}}
          {{ .Name }}Completer::Sync& _completer) = 0;
        {{- end }}
{{ "" }}
      {{- end }}
    {{- end }}
  };

  // Attempts to dispatch the incoming message to a handler function in the server implementation.
  // If there is no matching handler, it returns false, leaving the message and transaction intact.
  // In all other cases, it consumes the message and returns true.
  // It is possible to chain multiple TryDispatch functions in this manner.
  static ::fidl::DispatchResult TryDispatch{{ template "SyncServerDispatchMethodSignature" }};

  // Dispatches the incoming message to one of the handlers functions in the protocol.
  // If there is no matching handler, it closes all the handles in |msg| and closes the channel with
  // a |ZX_ERR_NOT_SUPPORTED| epitaph, before returning false. The message should then be discarded.
  static ::fidl::DispatchResult Dispatch{{ template "SyncServerDispatchMethodSignature" }};

  // Same as |Dispatch|, but takes a |void*| instead of |TypedChannelInterface*|.
  // Only used with |fidl::BindServer| to reduce template expansion.
  // Do not call this method manually. Use |Dispatch| instead.
  static ::fidl::DispatchResult TypeErasedDispatch(
      void* impl, fidl_incoming_msg_t* msg, ::fidl::Transaction* txn) {
    return Dispatch(static_cast<TypedChannelInterface*>(impl), msg, txn);
  }

  class EventSender;
  class WeakEventSender;
};
{{- end }}

{{- define "ProtocolTraits" -}}
{{ $protocol := . -}}
{{ range .Methods -}}
{{ $method := . -}}
{{- if .HasRequest }}

template <>
struct IsFidlType<{{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Request> : public std::true_type {};
template <>
struct IsFidlMessage<{{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Request> : public std::true_type {};
static_assert(sizeof({{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Request)
    == {{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Request::PrimarySize);
{{- range $index, $param := .Request }}
static_assert(offsetof({{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ $method.Name }}Request, {{ $param.Name }}) == {{ $param.Offset }});
{{- end }}
{{- end }}
{{- if .HasResponse }}

template <>
struct IsFidlType<{{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Response> : public std::true_type {};
template <>
struct IsFidlMessage<{{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Response> : public std::true_type {};
static_assert(sizeof({{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Response)
    == {{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ .Name }}Response::PrimarySize);
{{- range $index, $param := .Response }}
static_assert(offsetof({{ $protocol.Namespace }}::{{ $protocol.Name }}::{{ $method.Name }}Response, {{ $param.Name }}) == {{ $param.Offset }});
{{- end }}
{{- end }}
{{- end }}
{{- end }}

{{- define "ReturnResponseStructMembers" }}
{{- range $param := . }}
  *out_{{ $param.Name }} = std::move(_response.{{ $param.Name }});
{{- end }}
{{- end }}

{{- define "ProtocolDefinition" }}

namespace {
{{ $protocol := . -}}

{{- range .Methods }}
[[maybe_unused]]
constexpr uint64_t {{ .OrdinalName }} = {{ .Ordinal }}lu;
extern "C" const fidl_type_t {{ .RequestTypeName }};
extern "C" const fidl_type_t {{ .ResponseTypeName }};
{{- end }}

}  // namespace

{{- /* Client-calling functions do not apply to events. */}}
{{- range .ClientMethods -}}
{{ "" }}
    {{- template "SyncRequestManagedMethodDefinition" . }}
  {{- if or .Request .Response }}
{{ "" }}
    {{- template "SyncRequestCallerAllocateMethodDefinition" . }}
  {{- end }}
{{ "" }}
{{- end }}

{{- range .ClientMethods }}
{{ "" }}
  {{- template "ClientSyncRequestManagedMethodDefinition" . }}
  {{- if or .Request .Response }}
{{ "" }}
    {{- template "ClientSyncRequestCallerAllocateMethodDefinition" . }}
  {{- end }}
  {{- if .HasResponse }}
{{ "" }}
    {{- template "ClientAsyncRequestManagedMethodDefinition" . }}
  {{- end }}
{{- end }}
{{ template "ClientDispatchDefinition" . }}
{{ "" }}

{{- if .Events }}
  {{- template "EventHandlerHandleOneEventMethodDefinition" . }}
{{- end }}

{{- /* Server implementation */}}
{{ template "SyncServerTryDispatchMethodDefinition" . }}
{{ template "SyncServerDispatchMethodDefinition" . }}

{{- if .Methods }}
{{ "" }}
  {{- range .TwoWayMethods -}}
{{ "" }}
    {{- template "ReplyManagedMethodDefinition" . }}
    {{- if .Result }}
      {{- template "ReplyManagedResultSuccessMethodDefinition" . }}
      {{- template "ReplyManagedResultErrorMethodDefinition" . }}
    {{- end }}
    {{- if .Response }}
{{ "" }}
      {{- template "ReplyCallerAllocateMethodDefinition" . }}
      {{- if .Result }}
        {{- template "ReplyCallerAllocateResultSuccessMethodDefinition" . }}
      {{- end }}
    {{- end }}
{{ "" }}
  {{- end }}
{{ "" }}

  {{- range .Methods }}
{{ "" }}
    {{- if .HasRequest }}
{{ "" }}
    void {{ .LLProps.ProtocolName }}::{{ .Name }}Request::_InitHeader(zx_txid_t _txid) {
      fidl_init_txn_header(&_hdr, _txid, {{ .OrdinalName }});
    }
      {{- if .RequestIsResource }}

    void {{ .LLProps.ProtocolName }}::{{ .Name }}Request::_CloseHandles() {
      {{- range .Request }}
        {{- template "StructMemberCloseHandles" . }}
      {{- end }}
    }
      {{- end }}
    {{- end }}
    {{- if .HasResponse }}
{{ "" }}
    void {{ .LLProps.ProtocolName }}::{{ .Name }}Response::_InitHeader() {
      fidl_init_txn_header(&_hdr, 0, {{ .OrdinalName }});
    }
      {{- if .ResponseIsResource }}

    void {{ .LLProps.ProtocolName }}::{{ .Name }}Response::_CloseHandles() {
      {{- range .Response }}
        {{- template "StructMemberCloseHandles" . }}
      {{- end }}
    }
      {{- end }}
    {{- end }}
  {{- end }}
{{- end }}

{{- end }}
`

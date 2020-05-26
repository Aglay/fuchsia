# LLCPP bindings

## Libraries {#libraries}

Given the library declaration:

```fidl
library games.tictactoe;
```

Bindings code for this library is generated in the `llcpp::games::tictactoe`
namespace.

## Constants {#constants}

[Constants][lang-constants] are generated as a `constexpr`. For example, the
following constants:

```fidl
const uint8 BOARD_SIZE = 9;
const string NAME = "Tic-Tac-Toe";
```

Are generated in the header file as:

```c++
constexpr uint8_t BOARD_SIZE = 9u;
extern const char[] NAME;
```

The correspondence between FIDL primitive types and C++ types is outlined in
[built-in types](#builtins). Instead of `constexpr`, strings are declared as an
`extern const char[]` in the header file, and defined in a `.cc` file.

## Fields

This section describes how the FIDL toolchain converts FIDL types to native
types in LLCPP. These types can appear as members in an aggregate type or as
parameters to a protocol method.

### Built-in types {#builtins}

The FIDL types are converted to C++ types based on the following table:

|FIDL Type|LLCPP Type|
|--- |--- |
|`bool`|`bool`, *(requires sizeof(bool) == 1)*|
|`int8`|`int8_t`|
|`int16`|`int16_t`|
|`int32`|`int32_t`|
|`int64`|`int64_t`|
|`uint8`|`uint8_t`|
|`uint16`|`uint16_t`|
|`uint32`|`uint32_t`|
|`uint64`|`uint64_t`|
|`float32`|`float`|
|`float64`|`double`|
|`array<T>:N`|`fidl::Array<T, N>`|
|`vector<T>:N`|`fidl::VectorView<T>`|
|`string`|`fidl::StringView`|
|`request<P>`, `P` |`zx::channel`|
|`handle`|`zx::handle`|
|`handle<S>`|The corresponding zx type is used whenever possible. For example, `zx::vmo` or `zx::channel`.|

Nullable built-in types do not have different generated types than their
non-nullable counterparts in LLCPP, and are omitted from the table above.

### User defined types {#user-defined-types}

In LLCPP, a user defined type (bits, enum, constant, struct, union, or table) is
referred to using the generated class or variable (see [Type Definitions](#type-definitions)
). The nullable version of a user defined type
`T` is referred to using a `fidl::tracking_ptr` of the generated type *except*
for unions, which simply use the generated type itself. Refer to the [LLCPP Tutorial][llcpp-allocation]
for information about `tracking_ptr`.

## Type definitions {#type-definitions}

### Bits {#bits}

Given the [bits][lang-bits] definition:

```fidl
bits FileMode : uint16 {
    READ = 0b001;
    WRITE = 0b010;
    EXECUTE = 0b100;
};
```

The FIDL toolchain generates a `FileMode` class with a static member for each
flag, as well as a `mask` member that contains a mask of all bits members (in
this example `0b111`):

* `const static FileMode READ`
* `const static FileMode WRITE`
* `const static FileMode EXECUTE`
* `const static FileMode mask`

`FileMode` provides the following methods:

* `explicit constexpr FileMode(uint16_t)`: Constructor for `FileMode`, which
  uses the specified underlying type (in this example `uint16_t`).
* Bitwise operators: Implementations for the `|`, `|=`, `&`, `&=`, `^`, `^=`,
  and `~` operators are provided, allowing bitwise operations on the bits like
  `mode |= FileMode::EXECUTE`.
* Comparison operators `==` and `!=`.
* Explicit conversion functions for `uint16_t` and `bool`.


### Enums {#enums}

Given the [enum][lang-enums] definition:

```fidl
enum Color {
    RED = 1;
    GREEN = 2;
    BLUE = 3;
};
```

The FIDL toolchain generates an equivalent C++ `enum class` using the specified
underlying type, or `uint32_t` if none is specified:

```c++
enum class Color : uint32_t {
    RED = 1u;
    GREEN = 2u;
    BLUE = 3u;
};
```

### Structs {#structs}

Given the [struct][lang-structs] declaration:

```fidl
struct Person {
    uint32 id;
    string name = "john";
};
```

The FIDL toolchain generates an equivalent `struct`:

```c++
struct Person {
    uint32_t id = {};
    fidl::StringView name = {};
}
```

LLCPP does not currently support default values, and instead zero-initializes
all fields of the struct.

### Unions {#unions}

Given the union definition:

```fidl
union JsonValue {
    1: reserved;
    2: int32 int_value;
    3: string string_value;
};
```

FIDL will generate a `JsonValue` class. `JsonValue` contains a public tag enum
class representing the possible [variants][union-lexicon]:

```c++
enum class Tag : fidl_xunion_tag_t {
  kIntValue = 2,
  kStringValue = 3,
};
```

Each member of `Tag` has a value matching its ordinal specified in the `union`
definition. Reserved fields do not have any generated code

`JsonValue` provides the following methods:

* `JsonValue()`: Default constructor. The constructed union is initially in an
  "invalid" state until a variant is set. The `WithFoo` constructors should be
  preferred whenever possible.
* `~JsonValue()`: Destructor that clears the underlying union data.
* `JsonValue(JsonValue&&)`: Default move constructor.
* `JsonValue& operator=(JsonValue&&)`: Default move assignment
* `static JsonValue WithIntValue(fidl::tracking_ptr<int32>&&)` and `static
  JsonValue WithStringValue(fidl::tracking_ptr<fidl::StringView>&&)`: Static
  constructors that directly construct a specific variant of the union.
* `bool has_invalid_tag()`: Returns `true` if the instance of `JsonValue` does
   not yet have a variant set. Calling this method without first setting the
   variant leads to an assertion error.
* `bool is_int_value() const` and `bool is_string_value() const`: Each variant
  has an associated method to check whether an instance of `JsonValue` is of
  that variant
* `const int32_t& int_value() const` and `const fidl::StringView& string_value()
  const`: Read-only accessor methods for each variant. Calling these methods
  without first setting the variant leads to an assertion error.
* `int32_t& int_value()` and `fidl::StringView& string_value()`: Mutable
  accessor methods for each variant. These methods will fail if `JsonValue` does
  not have the specified variant set
* `void set_int_value(fidl::tracking_ptr<int32_t>&& value)` and `void
  set_string_value(fidl::tracking_ptr<fidl::StringView>&& value)`: Setter
  methods for each variant. These setters will overwrite the previously selected
  member, if any.
* `Tag Which() const`: returns the current [tag][union-lexicon] of the
  `JsonValue`. Calling this method without first setting the variant leads to an
  assertion error.

#### Flexible unions and unknown variants

[Flexible unions][lang-unions] (that is, unions that are prefixed with the
`flexible` keyword in their FIDL definition) have an extra variant in the
generated `Tag`:

```c++
  enum class Tag : fidl_xunion_tag_t {
    ... // other fields omitted
    kUnknown = ::std::numeric_limits<::fidl_union_tag_t>::max(),
  };
```

When a FIDL message containing a union with an unknown variant is decoded into
`JsonValue`, `JsonValue::Which()` will return `JsonValue::Tag::kUnknown`.

A flexible `JsonValue` also has the following extra methods:

* `void* unknownData() const`: Returns the raw bytes of the union variant. This
  method fails with an assertion error if the variant is *not* unknown.

Encoding a union with an unknown variant will write the unknown data and the
original [ordinal][union-lexicon] back onto the wire.

The decoding operation will fail when encountering an unknown variant at a
non-flexible union type.

### Tables {#tables}

Given the [table][lang-tables] definition:

```fidl
table User {
    1: reserved;
    2: uint8 age;
    3: string name;
};
```

The FIDL toolchain `User` class with the following methods:

* `User()`: Default constructor, initializes with all fields unset.
* `User(User&&)`: Default move constructor.
* `~User()`: Default destructor.
* `User& operator=(User&&)`: Default move assignment.
* `bool IsEmpty() const`: Returns true if no fields are set.
* `bool has_age() const` and `bool has_name() const`: Returns whether a field is
  set.
* `const uint8_t& age() const` and `const fidl::StringView& name() const`:
  Read-only field accessor methods. Calling these methods without first setting
  the field leads to an assertion error.
* `uint8_t& age()` and `fidl::StringView& mutable_age()`: Mutable field accessor
  methods. Calling these methods without first setting the variant leads to an
  assertion error.
* `User& set_age(uint8_t _value)` and `User& set_name(std::string _value)`:
  Field setters.

In order to build a table, three additional classes are generated:
`User::Frame`, `User::Builder`, and `User::UnownedBuilder`.

`User::Frame` is a container for the table's internal storage, and is allocated
separately from the builder because LLCPP maintains the object layout of the
underlying wire format. It only needs to be used in conjunction with
`User::Builder`. `User::Frame` has the following methods:

* `Frame()`: Default constructor.

`User::Builder` and `User::UnownedBuilder` both provide the following methods
for constructing a new `User`:

* `Builder&& set_age(fidl::tracking_ptr<uint8_t> elem)` and `Builder&&
  set_name(fidl::tracking_ptr<fidl::StringView> elem)`: Sets the specified field
  and returns the updated Builder.
* `User build()`: Returns a User based on the Builder's data.

However, they differ in that `User::UnownedBuilder` directly owns the underlying
`Frame`, which simplifies working with unowned data. The unowned builder is
constructed using the default constructor, whereas `User::Builder` explicitly
takes in a Frame:

```c++
Builder(fidl::tracking_ptr<User::Frame>&& frame_ptr)
```

An example of using all of these pieces together would be:

```c++
User user = User::Builder(std::make_unique<User::Frame>())
                 .set_age(std::make_unique<uint8_t>(30))
                 .set_name(std::make_unique<fidl::StringView>("john doe"))
                 .build();
```

In addition to assigning fields with `std::unique_ptr`, any of the allocation
strategies described in the [tutorial][llcpp-allocation] can also be used.

## Protocols {#protocols}

Given the [protocol][lang-protocols]:

```fidl
protocol TicTacToe {
    StartGame(bool start_first);
    MakeMove(uint8 row, uint8 col) -> (bool success, GameState? new_state);
    -> OnOpponentMove(GameState new_state);
};
```

Note: The `MakeMove` method above returns a bool representing success, and a
nullable response value. This is considered un-idiomatic, you should use an [error type](#protocols-results)
instead.

FIDL will generate a `TicTacToe` class, which acts as an entry point for types
and classes that both clients and servers will use to interact with this
service. The members of this class are described in individual subsections in
the rest of this section.

### Request and response structs {#request-response-structs}

FIDL generates a type for each request, response, and event in the protocol by
treating the parameters as struct fields. For example, the `MakeMoveRequest` is
generated as if it were a struct with two fields: `uint8 row`, and `uint8 col`,
providing the same generated code API as [regular structs](#structs):

```c++
struct MakeMoveRequest final {
    uint8_t row;
    uint8_t col;
}
```

For this example, the following types are generated:

* `TicTacToe::StartGameRequest`
* `TicTacToe::MakeMoveRequest`
* `TicTacToe::MakeMoveResponse`
* `TicTacToe::OnOpponentMoveResponse`

<!-- TODO: zeroargmessage -->

The naming scheme for requests is `[Method]Request`, and the naming scheme for
both responses and events is `[Method]Response`.

Any empty request, response, or event is aliased to `fidl::AnyZeroArgMessage`,
which is a type representing an empty message, instead of having a new type
generated.

### Client {#client}

The LLCPP bindings provides two main ways to interact with a FIDL protocol as a
client. The first is using a `SyncClient`, which is a class that owns the client
end of a channel and provides methods to make requests over the channel. The
second is using `Call`, which provides static methods to make requests directly
on an unowned channel without instantiating a `SyncClient`.

The methods provided by these two classes are analogous, but `Call` may be
easier to use when migrating code from the C bindings to LLCPP, or when
implementing C APIs that take a raw `zx_handle_t`.

#### SyncClient

`TicTacToe::SyncClient` provides the following methods:

* `explicit SyncClient(zx::channel)`: Constructor.
* `~SyncClient()`: Default destructor.
* `SyncClient(&&)`: Default move constructor.
* `SyncClient& operator=(SyncClient&&)`: Default move assignment.
* `const zx::channel& channel() const`: Returns the underlying channel as a
  const.
* `zx::channel* mutable_channel()`: Returns the underlying channel as mutable.
* `TicTacToe::ResultOf::StartGame StartGame(bool start_first)`: Owned variant of
  a fire and forget method call, which takes the parameters as arguments and
  returns the `ResultOf` class.
* `TicTacToe::UnownedResultOf::StartGame StartGame(fidl::BytePart, bool
  start_first)`: Caller-allocated variant of a fire and forget call, which takes
  in backing storage for the request buffer, as well as request parameters, and
  returns an `UnownedResultOf`.
* `ResultOf::MakeMove MakeMove(uint8_t row, uint8_t col)`: Owned variant of a
  two way method call, which takes the parameters as arguments and returns the
  `ResultOf` class.
* `UnownedResultOf::MakeMove(fidl::BytePart _request_bufffer, uint8_t row,
  uint8_t col, fidl::BytePart _response_buffer)`: Caller-allocated variant of a
  two way method, which takes in backing storage for the request buffer,
  followed by the request parameters, and finally backing storage for the
  response buffer, and returns an `UnownedResultOf`.
* `zx_status_t HandleEvents(EventHandlers handlers)`: Blocks to consume exactly
  one event from the channel. See [Events](#events)

Note that each method has both an owned and caller-allocated variant. In brief,
the owned variant of each method handles memory allocation for requests and
responses, whereas the caller-allocated variant allows the user to pass in the
buffers themselves. The owned variant is easier to use, but may result in extra
allocation. Details as well as examples for each variant are provided in the
[LLCPP Tutorial][llcpp-tutorial].

#### Call {#client-call}

`TicTacToe::Call` provides similar methods to those found in `SyncClient`, with
the only difference being that they are all `static` and take an
`unowned_channel` as the first parameter:

* `static ResultOf::StartGame StartGame(zx::unowned_channel _client_end, bool
  start_first)`:
* `static UnownedResultOf::StartGame StartGame(zx::unowned_channel _client_end,
  fidl::BytePart _request_buffer, bool start_first)`:
* `static ResultOf::MakeMove MakeMove(zx::unowned_channel _client_end, uint8_t
  row, uint8_t col)`:
* `static UnownedResultOf::MakeMove MakeMove(zx::unowned_channel _client_end,
  fidl::BytePart _request_buffer, uint8_t row, uint8_t col, fidl::BytePart
  _response_buffer);`:
* `static zx_status_t HandleEvents(zx::unowned_channel client_end, EventHandlers
  handlers)`:

#### ResultOf and UnownedResultOf

The managed variants of each method of `SyncClient` and `Call` all return a
`ResultOf::` type, whereas the caller-allocating variants all return an
`UnownedResultOf::`. Both types define the same set of methods:

*   `zx_status status() const` returns the transport status. it returns the
    first error encountered during (if applicable) linearizing, encoding, making
    a call on the underlying channel, and decoding the result. If the status is
    `ZX_OK`, the call has succeeded, and vice versa.
*   `const char* error() const` contains a brief error message when status is
    not `ZX_OK`. Otherwise, returns `nullptr`.
*   **(only for two-way calls)** `T* Unwrap()` returns a pointer to the
    [response struct](#request-response-structs). For `ResultOf::`, the pointer
    points to memory owned by the result object. For `UnownedResultOf::`, the
    pointer points to the caller-provided buffer. `Unwrap()` should only be
    called when the status is `ZX_OK`.

### Server

Implementing a server for a FIDL protocol involves providing a concrete
implementation of `TicTacToe`.

The generated `TicTacToe::Interface` class has pure virtual methods
corresponding to the method calls defined in the FIDL protocol. Users implement
a `TicTacToe` server by providing a concerete implementation of
`TicTacToe::Interface`, which has the following pure virtual methods:

* `virtual void StartGame(bool start_first, StartGameCompleter::Sync
  _completer)`
* `virtual void MakeMove(uint8_t row, uint8_t col, MakeMoveCompleter::Sync
  _completer)`

Refer to the [example LLCPP server][llcpp-server-example] for how to bind and
set up a server implementation.

The LLCPP bindings also provide functions for manually dispatching a message
given an implementation, `TicTacToe::TryDispatch` and `TicTacToe::Dispatch`:

* `static bool TryDispatch(Interface* impl, fidl_msg_t* msg,
  ::fidl::Transaction* txn)`: Attempts to dispatch the incoming message. If
  there is no matching handler, it returns false, leaving the message and
  transaction intact. In all other cases, it consumes the message and returns
  true.
* `static bool Dispatch(Interface* impl, fidl_msg_t* msg, ::fidl::Transaction*
  txn)`: Dispatches the incoming message. If there is no matching handler, it
  closes all handles in the message and closes the channel with a
  `ZX_ERR_NOT_SUPPORTED` epitaph, and returns false.

#### Completers {#server-completers}

A completer is provied as the last argument of each generated FIDL method
handler, after all the FIDL request parameters for that method. The completer
classes capture the various ways one can complete a FIDL transaction, e.g. by
sending a reply, closing the channel with an epitaph, etc, and come in both
synchronous and asynchronous versions (though the `::Sync` class is provided as
an argument by default). In this example, this completers are:

* `Interface::TicTacToe::StartGameCompleter::Sync`
* `Interface::TicTacToe::StartGameCompleter::Async`
* `Interface::TicTacToe::MakeMoveCompleter::Sync`
* `Interface::TicTacToe::MakeMoveCompleter::Async`

All completer classes provide the following methods:

* `void Close(zx_status_t status)`: Close the channel and send `status` as the
  epitaph.

In addition, two way methods will provide two versions of a `Reply` method for
replying to a response: a managed variant and a caller-allocating variant. These
correspond to the variants present in the [client API](#client). For example,
both `MakeMoveCompleter::Sync` and `MakeMoveCompleter::Async` provide the
following `Reply` methods:

* `void Reply(bool success, fidl::tracking_ptr<GameState> new_state)`
* `void Reply(fidl::BytePart _buffer, bool success,
  fidl::tracking_ptr<GameState> new_state)`

Finally, sync completers for two way methods will provide a method for
converting to an async completer. Async completers can out-live the scope of the
handler by e.g. moving it into a lambda capture (see [LLCPP tutorial][llcpp-async-example]
for example usage), allowing the server to
respond to requests asynchronously. There is only one such method in the above
example, on `MakeMoveCompleter::Sync`:

```c++
MakeMoveCompleter::Async ToAsync()
```

### Events {#events}

#### Client

Clients can handle events by calling `TicTacToe::HandleEvents` and passing it a
`TicTacToe::EventHandlers`. This is a struct that contains handlers for each
type of event. In this example, it consists of the following members:

* `fit::callback<zx_status_t(GameState new_state)> on_opponent_move`:
* `fit::callback<zx_status_t()> unknown`:

#### Server

The `TicTacToe` class provides static methods for sending events. Similarly to
the client [Call](#client-call) APIs, these methods take an `unowned_channel` as
the first argument, sending the event over this channel. Each event has managed
and caller-allocating sender events, analogous to the [client API](#client) as
well as the [server completers](#server-completers).

The event sender methods are:

* `static zx_status_t SendOnOpponentMoveEvent(zx::unowned_channel _chan,
  GameState new_state)`
* `static zx_status_t SendOnOpponentMoveEventzx::unowned_channel _chan,
  fidl::BytePart _buffer, GameState new_state)`

### Results {#protocols-results}

Given a method:

```fidl
protocol TicTacToe {
    MakeMove(uint8 row, uint8 col) -> (GameState new_state) error MoveError;
};
```

FIDL will generate convenience methods on the [completers](#server-completers)
corresponding to methods with an error type. Depending on the Reply "variant",
the completer will have `ReplySuccess`, `ReplyError`, or both methods to respond
directly with the success or error data, without having to construct a union.

For the managed flavor, both methods are available:

* `void ReplySuccess(GameState new_state)`
* `void ReplyError(MoveError error)`

Since `ReplyError` doesn't require heap allocation, only `ReplySuccess` exists
for the caller-allocated flavor:

* `void ReplySuccess(fidl::BytePart _buffer, GameState new_state)`

Note that the passed in buffer is used to hold the *entire* response, not just
the data corresponding to the success variant.

The regularly generated `Reply` methods are available as well:

* `void Reply(TicTacToe_MakeMove_Result result)`: Owned variant.
* `void Reply(fidl::BytePart _buffer, TicTacToe_MakeMove_Result result)`:
  Caller-allocated variant.

The owned and caller-allocated variant use a parameter of
`TicTacToe_MakeMove_Result`, which is generated as a [union](#unions) with two
variants: `Response`, which is a `TicTacToe_MakeMove_Response`, and `Err`, which
is a `MoveError`. `TicTacToe_MakeMove_Response` is generated as a
[struct](#structs) with the response parameters as its fields. In this case, it
has a single field `new_state`, which is a `GameState`.

### Protocol composition {#protocol-composition}

FIDL does not have a concept of inheritance, and generates full code as
described above for all [composed protocols][lang-protocol-composition]. In
other words, the code generated for

```fidl
protocol A {
    Foo();
};

protocol B {
    compose A;
    Bar();
};
```

Provides the same API as the code generated for:

```fidl
protocol A {
    Foo();
};

protocol B {
    Foo();
    Bar();
};
```

The generated code is identical except for the method ordinals.

### Protocol and method attributes {#protocol-and-method-attributes}

#### Transitional

For protocol methods annotated with the
[`[Transitional]`](/docs/reference/fidl/language/attributes.md#transitional)
attribute, the `virtual` methods on the protocol class come with a default
`Close(ZX_NOT_SUPPORTED)` implementation. This allows implementations of the
protocol class with missing method overrides to compile successfully.

#### Discoverable

A protocol annotated with the
[`[Discoverable]`](/docs/reference/fidl/language/attributes.md#discoverable)
attribute causes the FIDL toolchain to generate an additional `static const char
Name[]` field on the protocol class, containing the full protocol name.

<!-- xrefs -->
[llcpp-allocation]:
/docs/development/languages/fidl/tutorials/tutorial-llcpp.md#memory-ownership
[llcpp-async-example]:
/docs/development/languages/fidl/tutorials/tutorial-llcpp.md#async-server
[llcpp-tutorial]: /docs/development/languages/fidl/tutorials/tutorial-llcpp.md
[llcpp-server-example]: /garnet/examples/fidl/echo_server_llcpp
[lang-constants]: /docs/reference/fidl/language/language.md#constants
[lang-bits]: /docs/reference/fidl/language/language.md#bits
[lang-enums]: /docs/reference/fidl/language/language.md#enums
[lang-structs]: /docs/reference/fidl/language/language.md#structs
[lang-tables]: /docs/reference/fidl/language/language.md#tables
[lang-unions]: /docs/reference/fidl/language/language.md#unions
[lang-protocols]: /docs/reference/fidl/language/language.md#protocols
[lang-protocol-composition]: /docs/reference/fidl/language/language.md#protocol-composition
[union-lexicon]: /docs/reference/fidl/language/lexicon.md#union-terms

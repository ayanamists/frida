# Experimental ArkTS bridge: registered-method invocation

This is an independent bridge layer. It does not add ArkTS runtime assumptions
to Gum, require Native Interceptor, or generate executable callbacks. Its first
target is the OHOS ARM64 Gadget with `code_signing: "required"` and QuickJS.

The bridge covers an App-registered set of synchronous methods. It does not
discover arbitrary methods, private methods, or methods in unregistered roots,
and does not observe App-internal calls. Those are separate milestones.

## Integration

Build this directory with the App's OHOS Native CMake toolchain, e.g. using
`add_subdirectory`, and package `libarktsbridge.so`. The `types/` directory is an
OHPM local native-module type package named `libarktsbridge.so`.

On the desired ArkTS thread, before loading Gadget, import the native module
and register roots. The same generic registration and invocation code handles
every method; descriptors provide the call signatures, not business wrappers.

```ts
import bridge from 'libarktsbridge.so';

class Calculator {
  static add(a: number, b: number): number { return a + b; }
}

bridge.registerRoot('app.calculator', Calculator, 'static', [
  { name: 'add', parameters: ['number', 'number'], returnType: 'number' }
]);
```

For instance methods, pass an object and the kind `instance`. For static
methods, pass its class and the kind `static`. A root identity is unique within
the current registering environment. Invalid or duplicate registrations throw
before publishing any method from that root. Registration retains the receiver
until environment teardown; script disposal releases transport sessions, not
registered App roots.

Load `index.js` into a Frida script, or bundle its CommonJS export. Then:

```js
const bridge = ArkTS.connect();
const methods = await bridge.enumerateMethods();
const target = methods.find(m => m.root === 'app.calculator' && m.name === 'add');
const value = await bridge.invoke(target.id, [20, 22]); // 42
bridge.close();
```

Use an async function or RPC export for this snippet. Method identities include
the registered root, property name and parameter types, such as
`app.calculator::add(number,number)`. Metadata includes `kind`, `returnType` and
the explicit discovery source `app-registration`.

## Call path and lifecycle

1. `NativeFunction` invokes a precompiled C export to submit a bounded JSON
   request, and immediately returns a ticket.
2. The native bridge queues a precompiled callback through
   `napi_call_threadsafe_function(..., napi_tsfn_nonblocking)`.
3. On the original ArkTS registration thread, the callback validates the
   signature, resolves the registered receiver and method property, and calls
   it with `napi_call_function`.
4. The response, including thread identities, is copied into ticket-owned
   storage. A Frida timer polls it and settles the JS Promise.

The Gadget thread never calls NAPI object APIs. No `NativeCallback`, libffi
closure, inline hook, LLDB bootstrap, or executable-memory allocation is used
by this layer. `lastExecution` exposes the submission, registration and
execution thread IDs for diagnostics.

`close()` immediately closes the native session, releases its tickets and
rejects pending JS Promises. `Script.bindWeak` also closes the native session
when the script is unloaded or its connection is collected. Queued work whose
ticket has been released is skipped. A method that has already started cannot
be interrupted or rolled back, including after a timeout. NAPI environment
teardown invalidates sessions, releases receiver references and aborts the
queue. One registering environment is supported per process; workers and
replacement environments are not yet supported.

## Types, errors and limits

Parameters and results support finite JS numbers except negative zero,
strings, booleans and null. Non-finite numbers and negative zero are rejected
instead of allowing JSON to silently change their values. Strings include
Unicode and embedded NUL. A `void` result is represented by null and identified
by its method metadata. Parameters must have the exact declared count and
types; there is no implicit coercion.

Objects, arrays, function arguments, BigInt, constructors and Promise results
are not supported yet. A method returning a Promise or another incompatible
value fails with `RETURN_TYPE_MISMATCH`; the asynchronous bridge transport does
not imply support for asynchronous target methods.

| Error code | Meaning |
| --- | --- |
| `METHOD_NOT_FOUND` | No registered method matches the requested identity |
| `ARGUMENT_MISMATCH` | Wrong argument count, type or unsupported value |
| `ARKTS_EXCEPTION` | The actual target method threw; its exception was cleared and reported |
| `RETURN_TYPE_MISMATCH` | Return value does not match the declared supported type |
| `SESSION_CLOSED` | The connection or ticket has been disposed |
| `SUBMIT_REJECTED` | Request/queue limit reached or native bridge unavailable |
| `TIMEOUT` | No response by the configured deadline; executing work cannot be interrupted |
| `SIZE_LIMIT` | The response cannot fit within the transport limit |

The initial transport allows 32 sessions, 128 outstanding requests, 256
registered method descriptors, 16 parameters per method and 64 KiB JSON
requests/responses. The JS facade polls every 5 ms and defaults to a 5-second
timeout. Capabilities explicitly report the registered-method coverage and
unsupported observation, callbacks, private discovery and asynchronous results.

The native C ABI is experimental: `open() -> uint32`,
`submit(session, utf8_json) -> uint32`,
`read(session, ticket, buffer, capacity) -> int32`,
`release(session, ticket) -> uint32`, and `close(session) -> uint32`.
Zero from `open` or `submit` means rejection. A read returns zero while pending,
negative for an invalid ticket, or the required byte count including NUL; it
copies only when the buffer is large enough. Reading does not consume a ticket.

## Validation

`fixtures/BridgeFixtures.ets` registers 13 actual ArkTS methods across a static
class and two separate instances. It is independent of ordinary Todo data.
`tests/session.js` checks metadata, dynamic target selection, scalar/string
conversion, receiver identity and state across script reloads, argument errors,
target exceptions, recovery, concurrent requests, immediate close and ticket
cleanup. It is loaded by the HarmonyOS workspace's `scripts/test-gadget.py
--arkts`; the existing M1 clean-process and required-policy checks run as part
of that suite.

The validated environment is Mate 70 / ARM64 / HongMeng Kernel 1.12.0 with
Command Line Tools 26.0.0.851, Native Clang 15.0.4 and an ordinary debug-signed
App. This is an M2A registered-method subset, not M3A discovery or M4A Hook
support, and does not establish Cangjie support.

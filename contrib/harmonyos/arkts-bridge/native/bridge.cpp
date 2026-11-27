#include <napi/native_api.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <sys/syscall.h>
#include <unistd.h>

namespace {
constexpr size_t kMaxBytes = 65536;
constexpr size_t kMaxRequests = 128;
constexpr size_t kMaxSessions = 32;
constexpr size_t kMaxMethods = 256;

struct Error { std::string code; std::string message; };
struct Method {
  std::string id, root, name, kind, result;
  std::vector<std::string> parameters;
};
struct Root { napi_ref receiver; };
struct Request {
  uint32_t session, id, origin;
  std::string input, output;
  bool ready = false;
};
struct State {
  // Only transport state is shared. NAPI references and method metadata are
  // accessed exclusively by the registering ArkTS environment's thread.
  std::mutex mutex;
  napi_env env = nullptr;
  napi_threadsafe_function queue = nullptr;
  uint32_t owner = 0, next_session = 1, next_request = 1;
  bool ready = false;
  std::set<uint32_t> sessions;
  std::map<uint32_t, std::shared_ptr<Request>> requests;
  std::map<std::string, Root> roots;
  std::map<std::string, Method> methods;
};
State state;

uint32_t Tid() { return static_cast<uint32_t>(syscall(SYS_gettid)); }
void Check(napi_status status) {
  if (status != napi_ok) throw Error{"NAPI_ERROR", "NAPI operation failed: " + std::to_string(status)};
}
napi_value String(napi_env env, const std::string &value) {
  napi_value result;
  Check(napi_create_string_utf8(env, value.data(), value.size(), &result));
  return result;
}
std::string Text(napi_env env, napi_value value) {
  size_t size;
  Check(napi_get_value_string_utf8(env, value, nullptr, 0, &size));
  if (size > kMaxBytes) throw Error{"SIZE_LIMIT", "String exceeds the bridge limit"};
  std::string result(size + 1, '\0');
  Check(napi_get_value_string_utf8(env, value, result.data(), result.size(), &size));
  result.resize(size);
  return result;
}
napi_value Object(napi_env env) {
  napi_value result;
  Check(napi_create_object(env, &result));
  return result;
}
napi_value Get(napi_env env, napi_value value, const char *name) {
  napi_value result;
  Check(napi_get_named_property(env, value, name, &result));
  return result;
}
void Set(napi_env env, napi_value object, const char *name, napi_value value) {
  Check(napi_set_named_property(env, object, name, value));
}
void SetText(napi_env env, napi_value object, const char *name, const std::string &value) {
  Set(env, object, name, String(env, value));
}
void SetNumber(napi_env env, napi_value object, const char *name, uint32_t value) {
  napi_value number;
  Check(napi_create_uint32(env, value, &number));
  Set(env, object, name, number);
}
void SetBool(napi_env env, napi_value object, const char *name, bool value) {
  napi_value boolean;
  Check(napi_get_boolean(env, value, &boolean));
  Set(env, object, name, boolean);
}
napi_value Json(napi_env env, const char *operation, napi_value input) {
  napi_value global, result;
  Check(napi_get_global(env, &global));
  napi_value json = Get(env, global, "JSON");
  Check(napi_call_function(env, json, Get(env, json, operation), 1, &input, &result));
  return result;
}
napi_value Element(napi_env env, napi_value array, uint32_t index) {
  napi_value value;
  Check(napi_get_element(env, array, index, &value));
  return value;
}
uint32_t ArraySize(napi_env env, napi_value value) {
  bool array;
  Check(napi_is_array(env, value, &array));
  if (!array) throw Error{"ARGUMENT_MISMATCH", "Expected an array"};
  uint32_t size;
  Check(napi_get_array_length(env, value, &size));
  return size;
}
bool Matches(napi_env env, napi_value value, const std::string &type) {
  napi_valuetype actual;
  Check(napi_typeof(env, value, &actual));
  if (type == "number" && actual == napi_number) {
    double number;
    Check(napi_get_value_double(env, value, &number));
    return std::isfinite(number) && !(number == 0 && std::signbit(number));
  }
  return (type == "string" && actual == napi_string) ||
      (type == "boolean" && actual == napi_boolean) ||
      (type == "null" && actual == napi_null) ||
      (type == "void" && actual == napi_undefined);
}
bool SupportedType(const std::string &type, bool result) {
  return type == "number" || type == "string" || type == "boolean" ||
      type == "null" || (result && type == "void");
}
void ClearException(napi_env env) {
  bool pending = false;
  napi_is_exception_pending(env, &pending);
  if (pending) {
    napi_value exception;
    napi_get_and_clear_last_exception(env, &exception);
  }
}
std::string ExceptionMessage(napi_env env) {
  napi_value exception;
  Check(napi_get_and_clear_last_exception(env, &exception));
  napi_value text;
  Check(napi_coerce_to_string(env, exception, &text));
  return Text(env, text);
}
napi_value MethodInfo(napi_env env, const Method &method) {
  napi_value info = Object(env), parameters;
  SetText(env, info, "id", method.id);
  SetText(env, info, "root", method.root);
  SetText(env, info, "name", method.name);
  SetText(env, info, "kind", method.kind);
  SetText(env, info, "returnType", method.result);
  SetText(env, info, "source", "app-registration");
  Check(napi_create_array_with_length(env, method.parameters.size(), &parameters));
  for (uint32_t i = 0; i < method.parameters.size(); i++)
    Check(napi_set_element(env, parameters, i, String(env, method.parameters[i])));
  Set(env, info, "parameters", parameters);
  return info;
}
napi_value Execute(napi_env env, napi_value request) {
  const std::string operation = Text(env, Get(env, request, "operation"));
  if (operation == "methods") {
    napi_value list;
    Check(napi_create_array_with_length(env, state.methods.size(), &list));
    uint32_t index = 0;
    for (const auto &item : state.methods)
      Check(napi_set_element(env, list, index++, MethodInfo(env, item.second)));
    return list;
  }
  if (operation == "capabilities") {
    napi_value info = Object(env);
    SetText(env, info, "version", "0.1.0");
    SetText(env, info, "coverage", "app-registered-methods");
    SetText(env, info, "transport", "precompiled-napi-threadsafe-function");
    SetBool(env, info, "observation", false);
    SetBool(env, info, "asynchronousResults", false);
    SetBool(env, info, "callbacks", false);
    SetBool(env, info, "nonPublicDiscovery", false);
    SetNumber(env, info, "maxRequestBytes", kMaxBytes);
    SetNumber(env, info, "maxPendingRequests", kMaxRequests);
    // Transport counts are useful to verify session disposal after script unload.
    std::lock_guard<std::mutex> lock(state.mutex);
    SetNumber(env, info, "activeSessions", state.sessions.size());
    SetNumber(env, info, "pendingRequests", state.requests.size());
    return info;
  }
  if (operation != "invoke") throw Error{"UNSUPPORTED_OPERATION", "Unknown bridge operation"};
  const std::string id = Text(env, Get(env, request, "method"));
  auto found = state.methods.find(id);
  if (found == state.methods.end()) throw Error{"METHOD_NOT_FOUND", "No registered method has identity " + id};
  const Method &method = found->second;
  napi_value arguments = Get(env, request, "arguments");
  const uint32_t count = ArraySize(env, arguments);
  if (count != method.parameters.size()) throw Error{"ARGUMENT_MISMATCH", "Incorrect argument count for " + id};
  std::vector<napi_value> values;
  for (uint32_t i = 0; i < count; i++) {
    napi_value value = Element(env, arguments, i);
    if (!Matches(env, value, method.parameters[i]))
      throw Error{"ARGUMENT_MISMATCH", "Argument " + std::to_string(i) + " must be " + method.parameters[i]};
    values.push_back(value);
  }
  napi_value receiver, result;
  Check(napi_get_reference_value(env, state.roots.at(method.root).receiver, &receiver));
  napi_value function = Get(env, receiver, method.name.c_str());
  const napi_status status = napi_call_function(env, receiver, function, values.size(), values.data(), &result);
  bool pending;
  Check(napi_is_exception_pending(env, &pending));
  if (pending) throw Error{"ARKTS_EXCEPTION", ExceptionMessage(env)};
  Check(status);
  if (!Matches(env, result, method.result))
    throw Error{"RETURN_TYPE_MISMATCH", "Return value does not match " + method.result};
  if (method.result == "void") Check(napi_get_null(env, &result));
  return result;
}
void Dispatch(napi_env env, napi_value, void *, void *data) {
  std::unique_ptr<std::shared_ptr<Request>> holder(static_cast<std::shared_ptr<Request> *>(data));
  const auto request = *holder;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (env == nullptr || !state.ready || state.sessions.count(request->session) == 0 ||
        state.requests.count(request->id) == 0) return;
  }
  std::string output;
  try {
    napi_value response = Object(env);
    try {
      if (Tid() != state.owner) throw Error{"WRONG_THREAD", "NAPI callback left the registering thread"};
      napi_value input;
      try { input = Json(env, "parse", String(env, request->input)); }
      catch (const Error &) { ClearException(env); throw Error{"INVALID_REQUEST", "Request must be valid JSON"}; }
      Set(env, response, "result", Execute(env, input));
      SetBool(env, response, "ok", true);
    } catch (const Error &error) {
      ClearException(env);
      SetBool(env, response, "ok", false);
      napi_value detail = Object(env);
      SetText(env, detail, "code", error.code);
      SetText(env, detail, "message", error.message);
      Set(env, response, "error", detail);
    }
    SetNumber(env, response, "registrationThread", state.owner);
    SetNumber(env, response, "executionThread", Tid());
    SetNumber(env, response, "originThread", request->origin);
    output = Text(env, Json(env, "stringify", response));
  } catch (const Error &error) {
    ClearException(env);
    output = error.code == "SIZE_LIMIT"
        ? "{\"ok\":false,\"error\":{\"code\":\"SIZE_LIMIT\",\"message\":\"Response exceeds the bridge limit\"}}"
        : "{\"ok\":false,\"error\":{\"code\":\"NAPI_ERROR\",\"message\":\"Unable to encode bridge response\"}}";
  } catch (...) {
    ClearException(env);
    output = "{\"ok\":false,\"error\":{\"code\":\"NAPI_ERROR\",\"message\":\"Unexpected native bridge failure\"}}";
  }
  if (output.size() > kMaxBytes)
    output = "{\"ok\":false,\"error\":{\"code\":\"SIZE_LIMIT\",\"message\":\"Response exceeds the bridge limit\"}}";
  std::lock_guard<std::mutex> lock(state.mutex);
  request->output = std::move(output);
  request->ready = true;
}
void Cleanup(void *) {
  napi_threadsafe_function queue;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.ready = false;
    state.sessions.clear();
    state.requests.clear();
    queue = state.queue;
    state.queue = nullptr;
  }
  for (const auto &root : state.roots) napi_delete_reference(state.env, root.second.receiver);
  state.roots.clear();
  state.methods.clear();
  if (queue != nullptr) napi_release_threadsafe_function(queue, napi_tsfn_abort);
}
void Initialize(napi_env env) {
  if (state.env != nullptr) {
    if (state.env != env || state.owner != Tid())
      throw Error{"WRONG_ENVIRONMENT", "Only one registering ArkTS environment is currently supported"};
    return;
  }
  napi_threadsafe_function queue;
  Check(napi_create_threadsafe_function(env, nullptr, nullptr, String(env, "frida-arkts-bridge"),
      kMaxRequests, 1, nullptr, nullptr, nullptr, Dispatch, &queue));
  Check(napi_unref_threadsafe_function(env, queue));
  Check(napi_add_env_cleanup_hook(env, Cleanup, nullptr));
  std::lock_guard<std::mutex> lock(state.mutex);
  state.env = env;
  state.owner = Tid();
  state.queue = queue;
  state.ready = true;
}
napi_value RegisterRoot(napi_env env, napi_callback_info info) {
  try {
    size_t argc = 4;
    napi_value argv[4];
    Check(napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr));
    if (argc != 4) throw Error{"INVALID_REGISTRATION", "Expected root name, receiver, kind and method metadata"};
    Initialize(env);
    const std::string root = Text(env, argv[0]), kind = Text(env, argv[2]);
    if (root.empty() || root.size() > 128 || root.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos)
      throw Error{"INVALID_REGISTRATION", "Invalid root identity"};
    if (state.roots.count(root) != 0) throw Error{"INVALID_REGISTRATION", "Root identity is already registered"};
    napi_valuetype type;
    Check(napi_typeof(env, argv[1], &type));
    if (!((kind == "static" && type == napi_function) || (kind == "instance" && type == napi_object)))
      throw Error{"INVALID_REGISTRATION", "Expected a class for static methods or an object for instance methods"};
    uint32_t count = ArraySize(env, argv[3]);
    if (count == 0 || state.methods.size() + count > kMaxMethods)
      throw Error{"INVALID_REGISTRATION", "Invalid method count"};
    std::map<std::string, Method> methods;
    for (uint32_t i = 0; i < count; i++) {
      napi_value descriptor = Element(env, argv[3], i);
      Method method;
      method.root = root;
      method.kind = kind;
      method.name = Text(env, Get(env, descriptor, "name"));
      method.result = Text(env, Get(env, descriptor, "returnType"));
      if (method.name.empty() || method.name.size() > 128 || method.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$") != std::string::npos)
        throw Error{"INVALID_REGISTRATION", "Invalid method name"};
      napi_value function = Get(env, argv[1], method.name.c_str());
      Check(napi_typeof(env, function, &type));
      if (type != napi_function) throw Error{"INVALID_REGISTRATION", "Registered property is not a function"};
      if (!SupportedType(method.result, true)) throw Error{"UNSUPPORTED_TYPE", "Unsupported return type"};
      napi_value parameters = Get(env, descriptor, "parameters");
      uint32_t n = ArraySize(env, parameters);
      if (n > 16) throw Error{"INVALID_REGISTRATION", "Too many parameters"};
      method.id = root + "::" + method.name + "(";
      for (uint32_t j = 0; j < n; j++) {
        const std::string parameter = Text(env, Element(env, parameters, j));
        if (!SupportedType(parameter, false)) throw Error{"UNSUPPORTED_TYPE", "Unsupported parameter type"};
        method.parameters.push_back(parameter);
        method.id += (j == 0 ? "" : ",") + parameter;
      }
      method.id += ")";
      if (!methods.emplace(method.id, method).second)
        throw Error{"INVALID_REGISTRATION", "Duplicate method identity"};
    }
    napi_ref receiver;
    Check(napi_create_reference(env, argv[1], 1, &receiver));
    state.roots.emplace(root, Root{receiver});
    state.methods.insert(methods.begin(), methods.end());
    napi_value result;
    Check(napi_get_undefined(env, &result));
    return result;
  } catch (const Error &error) {
    ClearException(env);
    napi_throw_error(env, error.code.c_str(), error.message.c_str());
    return nullptr;
  }
}
napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor descriptor = {"registerRoot", nullptr, RegisterRoot, nullptr, nullptr, nullptr, napi_default, nullptr};
  napi_define_properties(env, exports, 1, &descriptor);
  return exports;
}
napi_module module = {1, 0, nullptr, Init, "arktsbridge", nullptr, {0}};
__attribute__((constructor)) void Register() { napi_module_register(&module); }
}

#define BRIDGE_EXPORT extern "C" __attribute__((visibility("default")))

BRIDGE_EXPORT uint32_t frida_arkts_bridge_open() {
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.ready || state.sessions.size() >= kMaxSessions || state.next_session == 0) return 0;
  const uint32_t id = state.next_session++;
  state.sessions.insert(id);
  return id;
}
BRIDGE_EXPORT uint32_t frida_arkts_bridge_submit(uint32_t session, const char *input) {
  if (input == nullptr) return 0;
  const size_t length = strnlen(input, kMaxBytes + 1);
  if (length > kMaxBytes) return 0;
  std::lock_guard<std::mutex> lock(state.mutex);
  if (!state.ready || state.sessions.count(session) == 0 || state.requests.size() >= kMaxRequests || state.next_request == 0) return 0;
  auto request = std::make_shared<Request>();
  request->id = state.next_request++;
  request->session = session;
  request->origin = Tid();
  request->input.assign(input, length);
  auto data = std::make_unique<std::shared_ptr<Request>>(request);
  const napi_status status = napi_call_threadsafe_function(state.queue, data.get(), napi_tsfn_nonblocking);
  if (status != napi_ok) return 0;
  data.release();
  state.requests.emplace(request->id, request);
  return request->id;
}
BRIDGE_EXPORT int32_t frida_arkts_bridge_read(uint32_t session, uint32_t ticket, char *output, uint32_t capacity) {
  std::lock_guard<std::mutex> lock(state.mutex);
  const auto found = state.requests.find(ticket);
  if (state.sessions.count(session) == 0 || found == state.requests.end() || found->second->session != session) return -1;
  const auto &request = found->second;
  if (!request->ready) return 0;
  const uint32_t size = request->output.size() + 1;
  if (output != nullptr && capacity >= size) std::memcpy(output, request->output.c_str(), size);
  return static_cast<int32_t>(size);
}
BRIDGE_EXPORT uint32_t frida_arkts_bridge_release(uint32_t session, uint32_t ticket) {
  std::lock_guard<std::mutex> lock(state.mutex);
  auto found = state.requests.find(ticket);
  if (found == state.requests.end() || found->second->session != session) return 0;
  state.requests.erase(found);
  return 1;
}
BRIDGE_EXPORT uint32_t frida_arkts_bridge_close(uint32_t session) {
  std::lock_guard<std::mutex> lock(state.mutex);
  const uint32_t removed = state.sessions.erase(session);
  for (auto i = state.requests.begin(); i != state.requests.end(); ) {
    if (i->second->session == session) i = state.requests.erase(i);
    else ++i;
  }
  return removed;
}

/* A separate bridge layer: QuickJS -> precompiled C ABI -> ArkTS's NAPI thread. */
(function () {
  'use strict';
  class BridgeError extends Error {
    constructor(code, message) {
      super(message);
      this.name = 'ArkTSBridgeError';
      this.code = code;
    }
  }
  class Connection {
    constructor(options = {}) {
      if (Process.codeSigningPolicy !== 'required')
        throw new BridgeError('POLICY_MISMATCH', 'This bridge requires code_signing=required');
      const module = Process.getModuleByName(options.module || 'libarktsbridge.so');
      const bind = (name, result, args) => new NativeFunction(module.getExportByName('frida_arkts_bridge_' + name), result, args);
      this._abi = {
        open: bind('open', 'uint32', []),
        submit: bind('submit', 'uint32', ['uint32', 'pointer']),
        read: bind('read', 'int32', ['uint32', 'uint32', 'pointer', 'uint32']),
        release: bind('release', 'uint32', ['uint32', 'uint32']),
        close: bind('close', 'uint32', ['uint32'])
      };
      this._session = this._abi.open();
      if (this._session === 0)
        throw new BridgeError('BRIDGE_UNAVAILABLE', 'Register ArkTS roots before loading Gadget; session limit is 32');
      this._closed = false;
      this._pending = new Map();
      this._timeout = options.timeout || 5000;
      this._token = {};
      const close = this._abi.close, session = this._session;
      this._weak = Script.bindWeak(this._token, () => close(session));
      this.lastExecution = null;
    }
    capabilities() { return this._request({ operation: 'capabilities' }); }
    enumerateMethods() { return this._request({ operation: 'methods' }); }
    invoke(method, args = []) {
      if (typeof method !== 'string' || !Array.isArray(args))
        return Promise.reject(new BridgeError('ARGUMENT_MISMATCH', 'Expected a method identity and an argument array'));
      for (let i = 0; i < args.length; i++) {
        const value = args[i];
        if (value !== null && !(typeof value === 'string' || typeof value === 'boolean' ||
            (typeof value === 'number' && Number.isFinite(value) && !Object.is(value, -0))))
          return Promise.reject(new BridgeError('ARGUMENT_MISMATCH', 'Supported values are finite numbers excluding negative zero, strings, booleans and null'));
      }
      return this._request({ operation: 'invoke', method, arguments: args });
    }
    close() {
      if (this._closed) return;
      this._closed = true;
      // Weak callbacks may be deferred; explicit close must cancel immediately.
      this._abi.close(this._session);
      for (const [ticket, pending] of this._pending) {
        clearInterval(pending.timer);
        pending.reject(new BridgeError('SESSION_CLOSED', 'Bridge session closed'));
      }
      this._pending.clear();
      Script.unbindWeak(this._weak);
    }
    _request(request) {
      if (this._closed)
        return Promise.reject(new BridgeError('SESSION_CLOSED', 'Bridge session closed'));
      const ticket = this._abi.submit(this._session, Memory.allocUtf8String(JSON.stringify(request)));
      if (ticket === 0)
        return Promise.reject(new BridgeError('SUBMIT_REJECTED', 'Bridge unavailable or request/queue limit exceeded'));
      return new Promise((resolve, reject) => {
        const deadline = Date.now() + this._timeout;
        const finish = () => {
          const pending = this._pending.get(ticket);
          if (pending) clearInterval(pending.timer);
          this._pending.delete(ticket);
          this._abi.release(this._session, ticket);
        };
        const poll = () => {
          try {
            const size = this._abi.read(this._session, ticket, NULL, 0);
            if (size < 0) throw new BridgeError('SESSION_CLOSED', 'Bridge ticket is no longer valid');
            if (size === 0) {
              if (Date.now() >= deadline) throw new BridgeError('TIMEOUT', 'ArkTS response timed out; an executing method cannot be interrupted');
              return;
            }
            if (size > 65537) throw new BridgeError('PROTOCOL_ERROR', 'Invalid bridge response size');
            const buffer = Memory.alloc(size);
            if (this._abi.read(this._session, ticket, buffer, size) !== size)
              throw new BridgeError('PROTOCOL_ERROR', 'Bridge response changed while reading');
            const response = JSON.parse(buffer.readUtf8String());
            finish();
            this.lastExecution = {
              registrationThread: response.registrationThread,
              executionThread: response.executionThread,
              originThread: response.originThread
            };
            if (response.ok) resolve(response.result);
            else reject(new BridgeError(response.error.code, response.error.message));
          } catch (error) { finish(); reject(error); }
        };
        const timer = setInterval(poll, 5);
        this._pending.set(ticket, { timer, reject });
      });
    }
  }
  const api = { version: '0.1.0', connect: options => new Connection(options), BridgeError };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else globalThis.ArkTS = api;
})();

rpc.exports.arkts = async function (generation) {
  const bridge = ArkTS.connect();
  const results = [];
  function assert(value, message) { if (!value) throw new Error(message); }
  function equal(actual, expected, label) {
    assert(JSON.stringify(actual) === JSON.stringify(expected), label + ': ' + JSON.stringify(actual));
    results.push({ label, result: actual });
  }
  const capabilities = await bridge.capabilities();
  equal(capabilities.activeSessions, 1, 'previous script session disposed');
  equal(capabilities.observation, false, 'observation is not advertised');
  const methods = await bridge.enumerateMethods();
  equal(methods.length, 13, 'registered method count');
  assert(new Set(methods.map(m => m.id)).size === methods.length, 'method identities are unique');
  assert(methods.every(m => m.source === 'app-registration'), 'discovery source must be explicit');
  function method(root, name) {
    const candidates = methods.filter(m => m.root === root && m.name === name);
    assert(candidates.length === 1, 'ambiguous method ' + root + '/' + name);
    return candidates[0].id;
  }
  async function call(root, name, args, expected) {
    const id = method(root, name);
    equal(await bridge.invoke(id, args), expected, id);
    const thread = bridge.lastExecution;
    assert(thread.registrationThread === thread.executionThread, 'method ran off its ArkTS registration thread');
    assert(thread.originThread !== thread.executionThread, 'Gadget and ArkTS execution threads must be distinct');
    results[results.length - 1].threads = thread;
  }
  await call('fixture.math', 'add', [1.25, 2.5], 3.75);
  await call('fixture.math', 'multiply', [-3, 7], -21);
  await call('fixture.math', 'echo', ['鸿蒙🌟\u0000bridge'], 'static:鸿蒙🌟\u0000bridge');
  await call('fixture.math', 'invert', [true], false);
  await call('fixture.counterA', 'current', [], generation === 0 ? 10 : 7);
  await call('fixture.counterA', 'reset', [], null);
  await call('fixture.counterA', 'add', [7], 7);
  await call('fixture.counterA', 'current', [], 7);
  await call('fixture.counterB', 'current', [], 100);
  await call('fixture.counterA', 'echo', ['你好'], 'A:你好');
  await call('fixture.counterB', 'echo', ['你好'], 'B:你好');
  await call('fixture.counterA', 'acceptNull', [null], null);
  async function error(methodId, args, code, expectedMessage) {
    let caught;
    try { await bridge.invoke(methodId, args); } catch (error) { caught = error; }
    assert(caught && caught.code === code, 'Expected ' + code + ', got ' + caught);
    if (expectedMessage) assert(caught.message.includes(expectedMessage), 'ArkTS exception message was lost');
    results.push({ label: methodId, error: { code: caught.code, message: caught.message } });
  }
  await error('missing::method()', [], 'METHOD_NOT_FOUND');
  await error(method('fixture.math', 'add'), [1], 'ARGUMENT_MISMATCH');
  await error(method('fixture.math', 'add'), ['1', 2], 'ARGUMENT_MISMATCH');
  await error(method('fixture.math', 'add'), [NaN, 2], 'ARGUMENT_MISMATCH');
  await error(method('fixture.math', 'add'), [-0, 2], 'ARGUMENT_MISMATCH');
  await error(method('fixture.math', 'multiply'), [0, -1], 'RETURN_TYPE_MISMATCH');
  await error(method('fixture.counterA', 'acceptNull'), [undefined], 'ARGUMENT_MISMATCH');
  await error(method('fixture.math', 'fail'), ['expected'], 'ARKTS_EXCEPTION', 'fixture:expected');
  // A method exception must not remain pending in ArkTS or poison later calls.
  await call('fixture.math', 'add', [20, 22], 42);
  const batch = await Promise.all(Array.from({ length: 24 }, (_, i) => bridge.invoke(method('fixture.math', 'add'), [i, 1])));
  equal(batch, Array.from({ length: 24 }, (_, i) => i + 1), '24 concurrent requests');
  const second = ArkTS.connect();
  equal((await bridge.capabilities()).activeSessions, 2, 'independent sessions');
  second.close();
  let closed;
  try { await second.enumerateMethods(); } catch (error) { closed = error; }
  assert(closed && closed.code === 'SESSION_CLOSED', 'closed session remained usable');
  equal((await bridge.capabilities()).activeSessions, 1, 'explicit close released native session');
  const cancelledSession = ArkTS.connect();
  const cancelled = Array.from({ length: 16 }, () => cancelledSession.invoke(method('fixture.math', 'add'), [1, 2])
    .then(() => 'unexpected-success', error => error.code));
  cancelledSession.close();
  equal(await Promise.all(cancelled), Array(16).fill('SESSION_CLOSED'), 'closing rejects pending requests');
  const afterCancellation = await bridge.capabilities();
  equal(afterCancellation.activeSessions, 1, 'cancelled session released');
  equal(afterCancellation.pendingRequests, 1, 'only the current capability query retains a ticket');
  // Keep this connection reachable until script unload; the following script
  // generation verifies that Script.bindWeak closed its native session.
  globalThis.fixtureBridge = bridge;
  return { passed: true, capabilities, methods, results, policy: Process.codeSigningPolicy };
};

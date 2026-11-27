import arktsBridge from 'libarktsbridge.so';

class FixtureMath {
  static add(a: number, b: number): number { return a + b; }
  static multiply(a: number, b: number): number { return a * b; }
  static echo(value: string): string { return `static:${value}`; }
  static invert(value: boolean): boolean { return !value; }
  static fail(message: string): string { throw new Error(`fixture:${message}`); }
}

class FixtureCounter {
  private value: number;
  private prefix: string;
  constructor(value: number, prefix: string) { this.value = value; this.prefix = prefix; }
  add(delta: number): number { this.value += delta; return this.value; }
  current(): number { return this.value; }
  echo(value: string): string { return `${this.prefix}:${value}`; }
  acceptNull(value: null): null { return value; }
  reset(): void { this.value = 0; }
}

let registered: boolean = false;
export function registerBridgeFixtures(): void {
  if (registered) return;
  arktsBridge.registerRoot('fixture.math', FixtureMath, 'static', [
    { name: 'add', parameters: ['number', 'number'], returnType: 'number' },
    { name: 'multiply', parameters: ['number', 'number'], returnType: 'number' },
    { name: 'echo', parameters: ['string'], returnType: 'string' },
    { name: 'invert', parameters: ['boolean'], returnType: 'boolean' },
    { name: 'fail', parameters: ['string'], returnType: 'string' }
  ]);
  arktsBridge.registerRoot('fixture.counterA', new FixtureCounter(10, 'A'), 'instance', [
    { name: 'add', parameters: ['number'], returnType: 'number' },
    { name: 'current', parameters: [], returnType: 'number' },
    { name: 'echo', parameters: ['string'], returnType: 'string' },
    { name: 'acceptNull', parameters: ['null'], returnType: 'null' },
    { name: 'reset', parameters: [], returnType: 'void' }
  ]);
  arktsBridge.registerRoot('fixture.counterB', new FixtureCounter(100, 'B'), 'instance', [
    { name: 'add', parameters: ['number'], returnType: 'number' },
    { name: 'current', parameters: [], returnType: 'number' },
    { name: 'echo', parameters: ['string'], returnType: 'string' }
  ]);
  registered = true;
}

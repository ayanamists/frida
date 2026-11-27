export interface MethodDescriptor {
  name: string;
  parameters: string[];
  returnType: string;
}
export const registerRoot: (name: string, receiver: Object, kind: string, methods: MethodDescriptor[]) => void;

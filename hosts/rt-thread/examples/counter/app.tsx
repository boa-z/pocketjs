import { createSignal } from "solid-js";
import { mount } from "@pocketjs/framework/solid";
import { Text, View } from "@pocketjs/framework/solid/components";
import { onFrame, onButtonPress } from "@pocketjs/framework/lifecycle";
import { BTN } from "@pocketjs/framework/input";

function Counter() {
  const [count, setCount] = createSignal(0);
  let frames = 0;
  onFrame(() => {
    if (++frames % 60 === 0) setCount(value => value + 1);
  });
  onButtonPress(BTN.CIRCLE, () => setCount(value => value + 1));
  // Diagnostic accessor exposes real Solid state; the native probe also checks pixels.
  (globalThis as any).__counterValue = count;
  return (
    <View class="w-full h-full bg-slate-950 items-center justify-center gap-4">
      <Text class="text-3xl text-white font-bold">PocketJS D13x</Text>
      <View class="w-64 h-32 bg-blue-600 rounded-xl items-center justify-center">
        <Text class="text-4xl text-white">{count()}</Text>
      </View>
      <Text class="text-xl text-slate-300">Solid TSX / .pocket / RGB565</Text>
    </View>
  );
}

mount(() => <Counter />);

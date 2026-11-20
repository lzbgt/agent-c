import React from "react";
import { createRoot } from "react-dom/client";
import useLocalStorageState from "../src/hooks/useLocalStorageState";

export function mount() {
  const node = document.createElement("div");document.body.append(node);
  const setters: Record<string, React.Dispatch<React.SetStateAction<string[]>>> = {};
  function Harness() {
    const [scope, setScope] = React.useState("first");
    const [queue, setQueue] = useLocalStorageState<string[]>("scope-test." + scope, []);
    setters[scope] = setQueue;
    (window as any).storageScopeTest = { scope, queue, setScope, setters };
    return <div>{scope}: {queue.join(",")}</div>;
  }
  createRoot(node).render(<React.StrictMode><Harness /></React.StrictMode>);
}

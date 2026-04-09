import React from "react";

const MEDIA_SELECTOR = "audio,video";
type AutoplayGlobal = typeof globalThis & { __agentui_autoplay_unlocked?: boolean };

export default function useAutoplayUnlock(allowAutoplay: boolean) {
  React.useEffect(() => {
    if (!allowAutoplay) return;
    const globalWithAutoplay = globalThis as AutoplayGlobal;
    const doc = typeof document !== "undefined" ? document : null;

    const tryPlayAll = () => {
      if (!doc) return;
      const nodes = Array.from(doc.querySelectorAll(MEDIA_SELECTOR));
      for (const node of nodes) {
        if (!(node instanceof HTMLMediaElement)) continue;
        const el = node;
        try {
          const result = el.play();
          if (result && typeof (result as Promise<void>).catch === "function") {
            (result as Promise<void>).catch(() => {});
          }
        } catch {
          // ignore
        }
      }
    };

    const unlock = () => {
      if (globalWithAutoplay.__agentui_autoplay_unlocked) return;
      globalWithAutoplay.__agentui_autoplay_unlocked = true;
      tryPlayAll();
    };

    if (globalWithAutoplay.__agentui_autoplay_unlocked) {
      tryPlayAll();
    }

    const handler = () => unlock();
    const capture = true;
    try {
      window.addEventListener("pointerdown", handler, { capture, once: true });
      window.addEventListener("keydown", handler, { capture, once: true });
    } catch {
      // ignore
    }

    return () => {
      try {
        window.removeEventListener("pointerdown", handler, capture);
        window.removeEventListener("keydown", handler, capture);
      } catch {
        // ignore
      }
    };
  }, [allowAutoplay]);
}

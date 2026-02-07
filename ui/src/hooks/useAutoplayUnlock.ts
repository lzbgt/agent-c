import React from "react";

const MEDIA_SELECTOR = "audio,video";

export default function useAutoplayUnlock(allowAutoplay: boolean) {
  React.useEffect(() => {
    const g: any = typeof globalThis !== "undefined" ? (globalThis as any) : {};
    if (!allowAutoplay) return;
    const doc = typeof document !== "undefined" ? document : null;

    const tryPlayAll = () => {
      if (!doc) return;
      const nodes = Array.from(doc.querySelectorAll(MEDIA_SELECTOR));
      for (const node of nodes) {
        const el = node as HTMLMediaElement;
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
      if (g.__agentui_autoplay_unlocked) return;
      g.__agentui_autoplay_unlocked = true;
      tryPlayAll();
    };

    if (g.__agentui_autoplay_unlocked) {
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

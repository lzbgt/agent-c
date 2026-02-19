import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
  },
  build: {
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (!id.includes("node_modules")) {
            return undefined;
          }
          const hasPkg = (pkg: string) => id.includes(`/node_modules/${pkg}/`);
          const hasAnyPkg = (pkgs: string[]) => pkgs.some(hasPkg);
          if (
            hasAnyPkg([
              "react",
              "react-dom",
              "scheduler",
              "use-sync-external-store",
              "object-assign",
              "loose-envify",
            ])
          ) {
            return "react";
          }
          if (
            id.includes("/node_modules/remark-") ||
            id.includes("/node_modules/rehype-") ||
            id.includes("/node_modules/hast-") ||
            id.includes("/node_modules/mdast-") ||
            id.includes("/node_modules/micromark") ||
            id.includes("/node_modules/unified") ||
            id.includes("/node_modules/vfile") ||
            id.includes("/node_modules/unist") ||
            id.includes("/node_modules/character-") ||
            id.includes("/node_modules/parse-entities") ||
            id.includes("/node_modules/markdown-table") ||
            id.includes("/node_modules/ccount") ||
            id.includes("/node_modules/stringify-entities") ||
            id.includes("/node_modules/trim-lines") ||
            id.includes("/node_modules/property-information") ||
            id.includes("/node_modules/space-separated-tokens") ||
            id.includes("/node_modules/comma-separated-tokens") ||
            hasPkg("react-markdown") ||
            hasPkg("highlight.js")
          ) {
            return "markdown";
          }
          if (hasPkg("@tanstack/react-query")) {
            return "react-query";
          }
          if (hasPkg("react-hook-form")) {
            return "forms";
          }
          if (hasPkg("zod")) {
            return "zod";
          }
          return undefined;
        },
      },
    },
  },
});

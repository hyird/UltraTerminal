import pkg from "./package.json" with { type: "json" };
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import svgr from "vite-plugin-svgr";
import Icons from "unplugin-icons/vite";
import { FileSystemIconLoader } from "unplugin-icons/loaders";
import { resolve } from "node:path";
import {
  readFileSync,
  writeFileSync,
  unlinkSync,
  rmdirSync,
  readdirSync,
} from "node:fs";
import { viteSingleFile } from "vite-plugin-singlefile";
export default defineConfig({
  define: { __APP_INFO__: JSON.stringify({ pkg }) },
  base: "/",
  publicDir: false,
  plugins: [
    svgr(),
    Icons({
      customCollections: { svg: FileSystemIconLoader("./src/icons/svg") },
      compiler: "jsx",
      jsx: "react",
    }),
    tailwindcss(),
    react(),
    // Keep chunks until Vite has collected dependency licenses.
    viteSingleFile({ deleteInlinedFiles: false }),
    {
      name: "inline-third-party-notices",
      apply: "build",
      closeBundle() {
        const output = resolve(import.meta.dirname, "../build/web");
        const notices = [
          readFileSync(
            resolve(import.meta.dirname, "public/template_origin.txt"),
            "utf8",
          ),
          readFileSync(
            resolve(
              import.meta.dirname,
              "public/licenses/react-antd-admin.txt",
            ),
            "utf8",
          ),
          readFileSync(resolve(output, ".vite/license.md"), "utf8"),
        ].join("\n\n");
        const escaped = notices
          .replaceAll("&", "&amp;")
          .replaceAll("<", "&lt;")
          .replaceAll(">", "&gt;");
        const file = resolve(output, "index.html");
        writeFileSync(
          file,
          readFileSync(file, "utf8").replace(
            "</body>",
            () =>
              `<template id="third-party-notices"><pre>${escaped}</pre></template></body>`,
          ),
        );
        unlinkSync(resolve(output, ".vite/license.md"));
        rmdirSync(resolve(output, ".vite"));
        for (const name of readdirSync(output)) {
          if (/\.(?:js|css)$/.test(name)) unlinkSync(resolve(output, name));
        }
        if (readdirSync(output).join() !== "index.html")
          throw new Error("Frontend build must contain only index.html");
      },
    },
  ],
  resolve: {
    alias: { "#src": resolve("src"), "#package.json": resolve("package.json") },
  },
  build: {
    outDir: "../build/web",
    emptyOutDir: true,
    sourcemap: false,
    license: true,
  },
});

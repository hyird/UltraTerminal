import pkg from "./package.json" with { type: "json" };
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import svgr from "vite-plugin-svgr";
import Icons from "unplugin-icons/vite";
import { FileSystemIconLoader } from "unplugin-icons/loaders";
import { resolve } from "node:path";
export default defineConfig({
  define: { __APP_INFO__: JSON.stringify({ pkg }) },
  base: "/",
  plugins: [
    svgr(),
    Icons({
      customCollections: { svg: FileSystemIconLoader("./src/icons/svg") },
      compiler: "jsx",
      jsx: "react",
    }),
    tailwindcss(),
    react(),
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

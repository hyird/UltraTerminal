import { viteSingleFile } from "vite-plugin-singlefile";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
export default defineConfig({
  plugins: [react(), tailwindcss(), viteSingleFile()],
  build: { outDir: "../build/web", emptyOutDir: true, sourcemap: false },
});

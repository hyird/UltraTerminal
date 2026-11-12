import { viteSingleFile } from "vite-plugin-singlefile";
import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
export default defineConfig({
  plugins: [
    react(),
    tailwindcss(),
    viteSingleFile({
      useRecommendedBuildConfig: false,
      inlinePattern: ["**/*.css"],
    }),
    {
      name: "frontend-bundle-contract",
      generateBundle(_options, bundle) {
        const chunks = Object.values(bundle).filter(
          (file) => file.type === "chunk",
        );
        const entry = chunks.find((file) => file.isEntry);
        const application = chunks.find((file) => !file.isEntry);
        if (
          chunks.length !== 2 ||
          !entry?.fileName.startsWith("web-assets/login-") ||
          !application?.fileName.startsWith("web-assets/app-") ||
          entry.imports.length !== 0 ||
          entry.dynamicImports.length !== 1 ||
          entry.dynamicImports[0] !== application.fileName ||
          application.dynamicImports.length !== 0
        ) {
          this.error(
            "Frontend must contain one login entry and one dynamically loaded application bundle.",
          );
        }
      },
    },
  ],
  build: {
    outDir: "../build/web",
    emptyOutDir: true,
    sourcemap: false,
    cssCodeSplit: false,
    chunkSizeWarningLimit: 1800,
    rolldownOptions: {
      output: {
        entryFileNames: "web-assets/login-[hash].js",
        chunkFileNames: "web-assets/app-[hash].js",
      },
    },
  },
});

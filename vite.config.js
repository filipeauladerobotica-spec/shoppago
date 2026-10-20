import { defineConfig } from "vite";

const assetVersion = Date.now().toString(36);

export default defineConfig({
  plugins: [{
    name: "shope-pago-asset-cache-buster",
    apply: "build",
    transformIndexHtml(html) {
      return html.replace(/(src|href)="\/(app\.(?:js|css))"/g,
        (_, attribute, file) => `${attribute}="/${file}?v=${assetVersion}"`);
    }
  }],
  server: {
    host: "127.0.0.1",
    port: 4173,
    strictPort: true,
    proxy: {
      "/api": process.env.SHOPE_PAGO_API_URL || "http://127.0.0.1:8081",
      "/auth": process.env.SHOPE_PAGO_API_URL || "http://127.0.0.1:8081"
    }
  },
  preview: {
    host: "127.0.0.1",
    port: 4173,
    strictPort: true
  },
  build: {
    rollupOptions: {
      output: {
        entryFileNames: "app.js",
        chunkFileNames: "chunks/[name]-[hash].js",
        assetFileNames: asset => asset.name?.endsWith(".css")
          ? "app.css"
          : asset.name === "login-shopping-bag.png"
            ? "login-shopping-bag.png"
            : asset.name === "shope-pago-logo.png"
              ? "shope-pago-logo.png"
          : "assets/[name]-[hash][extname]"
      }
    }
  }
});

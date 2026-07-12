# AegisGate Showcase — 前端（Vite 构建 + preview 服务，自带 /api 代理）
FROM node:22-alpine
WORKDIR /app

COPY package.json package-lock.json ./
RUN npm ci

COPY . .
RUN npm run build

EXPOSE 5173
# preview 服务读取 SHOWCASE_BACKEND_URL 代理 /api 到后端容器
CMD ["npm", "run", "preview", "--", "--host", "0.0.0.0", "--port", "5173"]

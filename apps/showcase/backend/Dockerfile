# AegisGate Showcase — 后端（Express + tsx 运行）
FROM node:22-alpine
WORKDIR /app

COPY package.json package-lock.json ./
RUN npm ci

COPY . .

ENV PORT=8090
EXPOSE 8090
CMD ["npm", "run", "start"]

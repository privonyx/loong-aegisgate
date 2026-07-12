/**
 * 应用配置加载与校验（SR-1：密钥仅存后端环境变量，绝不下发前端）。
 */

import { z } from 'zod';

const schema = z.object({
  AEGISGATE_BASE_URL: z.string().url(),
  AEGISGATE_API_KEY: z.string().min(1, 'AEGISGATE_API_KEY 不能为空'),
  SHOWCASE_MODEL: z.string().min(1),
  SHOWCASE_FALLBACK_MODEL: z.string().optional(),
  PORT: z.coerce.number().int().positive().default(8090),
  SHOWCASE_CORS_ORIGIN: z.string().default('http://localhost:5173'),
  // 单次网关请求超时（ms）。deepseek 等模型生成长文本（如多集大纲）+ 出站护栏可能较慢，默认放宽到 180s。
  SHOWCASE_TIMEOUT_MS: z.coerce.number().int().positive().default(180_000),
});

export type AppConfig = z.infer<typeof schema>;

export function loadConfig(env: NodeJS.ProcessEnv = process.env): AppConfig {
  const parsed = schema.safeParse(env);
  if (!parsed.success) {
    const issues = parsed.error.issues.map((i) => `${i.path.join('.')}: ${i.message}`).join('; ');
    throw new Error(`配置校验失败：${issues}。请检查 .env（参考 .env.example）。`);
  }
  return parsed.data;
}

/**
 * 后端入口：加载配置 → 构建依赖 → 注册场景 → 启动 HTTP 服务。
 */

import 'dotenv/config';
import { createAegisClient } from './aegis/client';
import { createApp } from './app';
import { loadConfig } from './config';
import { ObservabilityStore } from './observability/store';
import { ScenarioRegistry } from './runtime/registry';
import { registerScenarios } from './scenarios/index';

function main(): void {
  const config = loadConfig();
  const model = { primary: config.SHOWCASE_MODEL, fallback: config.SHOWCASE_FALLBACK_MODEL };

  const registry = new ScenarioRegistry();
  registerScenarios(registry, model);

  const client = createAegisClient({
    baseUrl: config.AEGISGATE_BASE_URL,
    apiKey: config.AEGISGATE_API_KEY,
    timeoutMs: config.SHOWCASE_TIMEOUT_MS,
  });

  const store = new ObservabilityStore();

  const app = createApp({
    registry,
    client,
    store,
    corsOrigin: config.SHOWCASE_CORS_ORIGIN,
    model,
  });

  app.listen(config.PORT, () => {
    // eslint-disable-next-line no-console
    console.log(
      `[showcase] backend listening on :${config.PORT} | 网关=${config.AEGISGATE_BASE_URL} | 场景数=${registry.list().length}`
    );
  });
}

main();

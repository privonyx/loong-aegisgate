/**
 * 场景注册入口：把所有场景插件注册进注册表。
 * 换业务 = 在此注册新插件，运行时与路由零改动。
 *
 * 注：具体场景插件在阶段 3（漫剧）/ 阶段 4（电商）接入。
 */

import type { ScenarioRegistry } from '../runtime/registry';
import { comicPlugin } from './comic/plugin';
import { ecommercePlugin } from './ecommerce/plugin';

export type ScenarioModelConfig = { primary: string; fallback?: string };

export function registerScenarios(registry: ScenarioRegistry, model: ScenarioModelConfig): void {
  registry.register(comicPlugin(model)); // 旗舰场景在前
  registry.register(ecommercePlugin(model));
}

/**
 * 场景插件注册表：启动时注册所有场景，运行时按 id 查询。
 * 保持运行时与具体场景解耦（换业务 = 注册新插件）。
 */

import type { ScenarioPlugin, ScenarioSummary } from '../scenarios/types';
import { toScenarioSummary } from '../scenarios/types';

export class ScenarioRegistry {
  private readonly plugins = new Map<string, ScenarioPlugin>();

  register(plugin: ScenarioPlugin): void {
    if (this.plugins.has(plugin.id)) {
      throw new Error(`场景插件 id 重复注册: ${plugin.id}`);
    }
    this.plugins.set(plugin.id, plugin);
  }

  get(id: string): ScenarioPlugin | undefined {
    return this.plugins.get(id);
  }

  /** 取插件，不存在则抛错（路由层用于返回 404）。 */
  require(id: string): ScenarioPlugin {
    const plugin = this.plugins.get(id);
    if (!plugin) {
      throw new Error(`未知场景插件: ${id}`);
    }
    return plugin;
  }

  has(id: string): boolean {
    return this.plugins.has(id);
  }

  list(): ScenarioPlugin[] {
    return [...this.plugins.values()];
  }

  /** 暴露给前端的摘要列表（不含 handler / dataset 内部细节）。 */
  summaries(): ScenarioSummary[] {
    return this.list().map(toScenarioSummary);
  }
}

import { describe, expect, it } from 'vitest';
import { ecommercePlugin } from '../src/scenarios/ecommerce/plugin';
import { ecommerceCatalog, type ProductCatalog } from '../src/scenarios/ecommerce/data';
import type { ScenarioContext } from '../src/scenarios/types';
import { runStep } from '../src/runtime/chatLoop';
import type { ChatResponse } from '../src/aegis/types';
import { MockAegisClient, meta } from './helpers/mockClient';

const model = { primary: 'gpt-4o-mini' };
const plugin = ecommercePlugin(model);

function ctx(): ScenarioContext {
  return { scenarioId: 'ecommerce', dataset: ecommerceCatalog, checkGuardrail: async () => ({ blocked: false }) };
}
function tool(name: string) {
  const t = plugin.tools.find((x) => x.spec.function.name === name);
  if (!t) throw new Error(`missing tool ${name}`);
  return t;
}

describe('ecommerce 插件验证骨架通用性', () => {
  it('与漫剧同结构：tools + uiSteps + 双引擎', () => {
    expect(plugin.id).toBe('ecommerce');
    expect(plugin.tools.length).toBeGreaterThanOrEqual(3);
    const engines = new Set(plugin.uiSteps.map((s) => s.engine));
    expect(engines.has('creation')).toBe(true);
    expect(engines.has('compliance')).toBe(true);
    expect(plugin.guardrail.ruleFile).toBe('ecommerce.yaml');
  });
});

describe('SR-3：ecommerce 工具 handler 纯函数', () => {
  it('searchProducts 按品类 + 价格过滤', async () => {
    const before = JSON.stringify(ecommerceCatalog);
    const res = await tool('searchProducts').handler({ category: '音频', maxPrice: 1000 }, ctx());
    const data = res.data as { count: number; products: { category: string }[] };
    expect(data.products.every((p) => p.category === '音频')).toBe(true);
    expect(JSON.stringify(ecommerceCatalog)).toBe(before);
  });

  it('getProductDetail 命中/未命中', async () => {
    expect((await tool('getProductDetail').handler({ productId: 'p1' }, ctx())).ok).toBe(true);
    expect((await tool('getProductDetail').handler({ productId: 'pX' }, ctx())).ok).toBe(false);
  });

  it('createMockOrder 构造订单（确定性 id，不持久化），库存不足拒绝', async () => {
    const ok = await tool('createMockOrder').handler({ productId: 'p1', quantity: 2 }, ctx());
    const order = ok.data as { orderId: string; total: number; status: string };
    expect(order.orderId).toBe('ORD-p1-2');
    expect(order.total).toBe(899 * 2);
    expect(order.status).toBe('created');
    const over = await tool('createMockOrder').handler({ productId: 'p5', quantity: 9999 }, ctx());
    expect(over.ok).toBe(false);
    // dataset 库存未被真实扣减（纯函数）
    expect((ecommerceCatalog as ProductCatalog).products.find((p) => p.id === 'p5')?.stock).toBe(18);
  });
});

describe('导购 generate 流程 + 直调下单 tool 步骤', () => {
  it('recommend：模型调用 searchProducts 后给出推荐', async () => {
    const withTool: ChatResponse = {
      content: null,
      toolCalls: [{ id: 't1', type: 'function', function: { name: 'searchProducts', arguments: '{"category":"音频"}' } }],
      meta: meta(),
      guardrail: { blocked: false },
    };
    const final: ChatResponse = { content: '推荐：静界 Pro 降噪耳机', meta: meta({ tokensSaved: 88, cacheHit: true }), guardrail: { blocked: false } };
    const client = new MockAegisClient({ chatResponses: [withTool, final] });
    const step = plugin.uiSteps.find((s) => s.id === 'recommend')!;
    const result = await runStep({ plugin, step, inputs: { need: '通勤降噪', budget: 1000 }, client });
    expect(result.output).toContain('静界');
    expect(result.toolInvocations.some((t) => t.name === 'searchProducts')).toBe(true);
    expect(result.events.reduce((s, e) => s + e.tokensSaved, 0)).toBe(88);
  });

  it('order：直调 createMockOrder（不经模型）', async () => {
    const client = new MockAegisClient();
    const step = plugin.uiSteps.find((s) => s.id === 'order')!;
    const result = await runStep({ plugin, step, inputs: { productId: 'p2', quantity: 1 }, client });
    expect(client.chatCalls).toHaveLength(0);
    expect(result.output).toContain('ORD-p2-1');
  });
});

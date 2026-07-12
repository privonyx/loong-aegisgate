/**
 * 电商导购工具。SR-3：handler 纯函数——createMockOrder 只构造订单对象，
 * 不做真实持久化 / 网络副作用（确定性 orderId 便于演示与测试）。
 */

import type { ScenarioContext, ToolDefinition } from '../types';
import type { ProductCatalog } from './data';

function catalog(ctx: ScenarioContext): ProductCatalog {
  return ctx.dataset as ProductCatalog;
}

const searchProducts: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'searchProducts',
      description: '按关键词 / 品类 / 价格上限搜索商品',
      parameters: {
        type: 'object',
        properties: {
          query: { type: 'string', description: '关键词' },
          category: { type: 'string', description: '品类，如 音频/外设/穿戴' },
          maxPrice: { type: 'number', description: '价格上限' },
        },
      },
    },
  },
  handler: async (args, ctx) => {
    const { products } = catalog(ctx);
    const query = typeof args.query === 'string' ? args.query.trim() : '';
    const category = typeof args.category === 'string' ? args.category.trim() : '';
    const maxPrice = typeof args.maxPrice === 'number' ? args.maxPrice : Number.POSITIVE_INFINITY;
    const matched = products.filter((p) => {
      const byQuery = query ? p.name.includes(query) || p.description.includes(query) : true;
      const byCategory = category ? p.category === category : true;
      const byPrice = p.price <= maxPrice;
      return byQuery && byCategory && byPrice;
    });
    return { ok: true, data: { count: matched.length, products: matched } };
  },
};

const getProductDetail: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'getProductDetail',
      description: '按商品 id 获取详情',
      parameters: {
        type: 'object',
        properties: { productId: { type: 'string' } },
        required: ['productId'],
      },
    },
  },
  handler: async (args, ctx) => {
    const { products } = catalog(ctx);
    const product = products.find((p) => p.id === args.productId);
    if (!product) return { ok: false, error: `未找到商品: ${String(args.productId)}` };
    return { ok: true, data: product };
  },
};

const createMockOrder: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'createMockOrder',
      description: '创建模拟订单（仅演示，不做真实持久化）',
      parameters: {
        type: 'object',
        properties: {
          productId: { type: 'string' },
          quantity: { type: 'number' },
        },
        required: ['productId'],
      },
    },
  },
  handler: async (args, ctx) => {
    const { products } = catalog(ctx);
    const product = products.find((p) => p.id === args.productId);
    if (!product) return { ok: false, error: `未找到商品: ${String(args.productId)}` };
    const quantity = typeof args.quantity === 'number' && args.quantity > 0 ? Math.floor(args.quantity) : 1;
    if (quantity > product.stock) return { ok: false, error: `库存不足（仅剩 ${product.stock}）` };
    return {
      ok: true,
      data: {
        orderId: `ORD-${product.id}-${quantity}`,
        productId: product.id,
        productName: product.name,
        quantity,
        total: product.price * quantity,
        status: 'created',
      },
    };
  },
};

export const ecommerceTools: ToolDefinition[] = [searchProducts, getProductDetail, createMockOrder];

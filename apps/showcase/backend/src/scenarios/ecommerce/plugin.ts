/**
 * 电商导购场景插件：验证通用骨架——与漫剧同结构（tools + uiSteps），
 * 仅替换 dataset / 工具 / 护栏，运行时与路由零改动。
 */

import type { ScenarioPlugin } from '../types';
import type { ScenarioModelConfig } from '../index';
import { ecommerceCatalog } from './data';
import { ecommerceTools } from './tools';

const SYSTEM_PROMPT = `你是「星河数码」的金牌导购助手。
要求：
1. 先用 searchProducts / getProductDetail 查询真实在售商品，不要编造不存在的型号或价格。
2. 结合用户需求与预算给出 1-3 个推荐，说明理由（参数/适用场景），并对比差异。
3. 不夸大宣传、不承诺疗效/绝对效果；用户确认后可用 createMockOrder 下单。
4. 用简体中文，专业且亲切。`;

export function ecommercePlugin(model: ScenarioModelConfig): ScenarioPlugin {
  return {
    id: 'ecommerce',
    meta: {
      name: '电商导购助手',
      description: '基于真实商品目录的智能导购 + 下单 + 评价合规预审。',
      icon: 'shopping-bag',
      accentColor: '#10b981',
    },
    systemPrompt: SYSTEM_PROMPT,
    tools: ecommerceTools,
    uiSteps: [
      {
        id: 'recommend',
        label: '智能导购推荐',
        description: '描述需求与预算，助手查询商品目录给出对比推荐。',
        engine: 'creation',
        kind: 'generate',
        promptTemplate:
          '我的需求：{{need}}。预算上限：{{budget}} 元。请用 searchProducts/getProductDetail 查询后推荐 1-3 款并对比说明。',
        inputs: [
          { name: 'need', label: '购物需求', type: 'text', required: true, placeholder: '例如：通勤降噪、长续航', default: '通勤降噪、长续航' },
          { name: 'budget', label: '预算上限(元)', type: 'number', required: true, default: 1000 },
        ],
      },
      {
        id: 'order',
        label: '创建模拟订单',
        description: '选定商品后创建模拟订单（仅演示，不做真实交易）。',
        engine: 'creation',
        kind: 'tool',
        tool: 'createMockOrder',
        inputs: [
          { name: 'productId', label: '商品 id', type: 'select', required: true, default: 'p1', options: [
            { value: 'p1', label: 'p1 静界 Pro 降噪耳机' },
            { value: 'p2', label: 'p2 轨迹 87 机械键盘' },
            { value: 'p3', label: 'p3 脉冲 GT 智能手表' },
            { value: 'p4', label: 'p4 闪能 充电宝' },
            { value: 'p5', label: 'p5 澄空 27 显示器' },
          ] },
          { name: 'quantity', label: '数量', type: 'number', required: true, default: 1 },
        ],
      },
      {
        id: 'compliance',
        label: '评价/文案合规预审',
        description: '把商品文案 / 用户评价送入护栏，拦截违禁词与虚假宣传。',
        engine: 'compliance',
        kind: 'guard',
        textInput: 'text',
        inputs: [
          { name: 'text', label: '待审文本', type: 'textarea', required: true, placeholder: '粘贴商品文案 / 用户评价……' },
        ],
      },
    ],
    dataset: ecommerceCatalog,
    guardrail: { ruleFile: 'ecommerce.yaml' },
    model,
  };
}

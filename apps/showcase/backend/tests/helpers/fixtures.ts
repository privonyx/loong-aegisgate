/**
 * 测试用最小场景插件（覆盖 generate / tool / guard 三种步骤与一个读工具）。
 */

import type { ScenarioPlugin, ToolDefinition } from '../../src/scenarios/types';

const echoTool: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'getFacts',
      description: '返回测试用事实数据',
      parameters: { type: 'object', properties: { topic: { type: 'string' } } },
    },
  },
  handler: async (args) => ({ ok: true, data: { topic: args.topic ?? 'none', fact: 42 } }),
};

export function makeTestPlugin(overrides: Partial<ScenarioPlugin> = {}): ScenarioPlugin {
  return {
    id: 'test',
    meta: { name: '测试场景', description: '单测用', icon: 'flask', accentColor: '#888' },
    systemPrompt: '你是测试助手。',
    tools: [echoTool],
    uiSteps: [
      {
        id: 'gen',
        label: '生成',
        engine: 'creation',
        kind: 'generate',
        promptTemplate: '主题：{{topic}}',
        inputs: [{ name: 'topic', label: '主题', type: 'text', required: true }],
      },
      {
        id: 'lookup',
        label: '查询',
        engine: 'creation',
        kind: 'tool',
        tool: 'getFacts',
        inputs: [{ name: 'topic', label: '主题', type: 'text' }],
      },
      {
        id: 'check',
        label: '合规预审',
        engine: 'compliance',
        kind: 'guard',
        textInput: 'text',
        inputs: [{ name: 'text', label: '文本', type: 'textarea' }],
      },
    ],
    dataset: { sample: true },
    guardrail: { ruleFile: 'test.yaml' },
    model: { primary: 'test-model', fallback: 'test-fallback' },
    ...overrides,
  };
}

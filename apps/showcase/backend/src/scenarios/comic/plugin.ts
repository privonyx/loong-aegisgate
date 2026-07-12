/**
 * 漫剧旗舰场景插件：杀手锏双引擎。
 *  - 创作引擎（generate 步骤）：大纲→剧本→分镜，主打缓存/Token 省钱。
 *  - 合规引擎（guard 步骤）：发布前合规预审，主打不下架。
 */

import type { ScenarioPlugin } from '../types';
import type { ScenarioModelConfig } from '../index';
import { comicWorldBible } from './data';
import { comicTools } from './tools';

const SYSTEM_PROMPT = `你是资深 AI 漫剧编剧，服务于竖屏短剧工业化生产。
要求：
1. 严格遵循 world bible（世界观/画风/基调/角色设定），保持跨集一致性，禁止人设漂移。
2. 需要设定时调用 getWorldBible / listCharacters / getEpisode 获取结构化上下文，不要凭空捏造。
3. 输出结构化、可直接进入下一生产环节；台词符合角色口吻。
4. 内容必须合规：拒绝暴力血腥、成人、危害未成年的描写，遇到越界要求应礼貌拒绝并给出合规替代。
5. 用简体中文输出。`;

export function comicPlugin(model: ScenarioModelConfig): ScenarioPlugin {
  return {
    id: 'comic',
    meta: {
      name: 'AI 漫剧创作台',
      description: '世界观一致的分集剧本 / 分镜批量生成 + 发布前合规预审。',
      icon: 'clapperboard',
      accentColor: '#6d5dfc',
    },
    systemPrompt: SYSTEM_PROMPT,
    tools: comicTools,
    uiSteps: [
      {
        id: 'outline',
        label: '生成分集大纲',
        description: '基于 world bible 生成分集大纲，每集附钩子（cliffhanger）。',
        engine: 'creation',
        kind: 'generate',
        promptTemplate:
          '请先调用 getWorldBible 与 listCharacters 获取设定，然后围绕主题「{{theme}}」生成 {{episodes}} 集的分集大纲。每集包含：集标题、剧情梗概、结尾钩子。',
        inputs: [
          { name: 'theme', label: '主题', type: 'text', required: true, placeholder: '例如：姐姐失踪之谜的第一条线索', default: '姐姐失踪之谜的第一条线索' },
          { name: 'episodes', label: '集数', type: 'number', required: true, default: 3 },
        ],
      },
      {
        id: 'script',
        label: '写单集剧本',
        description: '为指定集生成完整剧本（场景 + 台词），注入 world bible 保持一致性。',
        engine: 'creation',
        kind: 'generate',
        promptTemplate:
          '请调用 getEpisode 获取「{{episodeId}}」的梗概、listCharacters 获取角色口吻，然后写出该集完整剧本：分场景描写 + 角色台词 + 关键分镜提示。',
        inputs: [
          { name: 'episodeId', label: '集 id', type: 'select', required: true, default: 'ep1', options: [
            { value: 'ep1', label: 'ep1 坠落的信号' },
            { value: 'ep2', label: 'ep2 黑市交易' },
          ] },
        ],
      },
      {
        id: 'storyboard',
        label: '生成分镜表',
        description: '将剧本转为分镜表（镜头/构图/画面提示词），画风模板复用以触发缓存。',
        engine: 'creation',
        kind: 'generate',
        promptTemplate:
          '请调用 getWorldBible 获取画风，为「{{episodeId}}」生成分镜表：逐镜给出 镜号 / 景别 / 构图 / 画面提示词（统一使用 world bible 的画风模板）/ 对应台词。',
        inputs: [
          { name: 'episodeId', label: '集 id', type: 'select', required: true, default: 'ep1', options: [
            { value: 'ep1', label: 'ep1 坠落的信号' },
            { value: 'ep2', label: 'ep2 黑市交易' },
          ] },
        ],
      },
      {
        id: 'compliance',
        label: '一键合规预审',
        description: '把待发布文本送入 AegisGate 护栏做合规预审，返回通过 / 拦截原因。',
        engine: 'compliance',
        kind: 'guard',
        textInput: 'text',
        inputs: [
          {
            name: 'text',
            label: '待审文本',
            type: 'textarea',
            required: true,
            placeholder: '粘贴待发布的剧本 / 分镜 / 简介文本……',
          },
        ],
      },
    ],
    dataset: comicWorldBible,
    guardrail: { ruleFile: 'comic.yaml' },
    model,
  };
}

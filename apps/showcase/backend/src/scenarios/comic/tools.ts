/**
 * 漫剧创作引擎「读工具」：供模型在生成时调用以获取结构化设定。
 * SR-3：handler 纯函数——只读 dataset，无文件写 / 命令执行 / 网络副作用。
 */

import type { ScenarioContext, ToolDefinition } from '../types';
import type { WorldBible } from './data';

function bible(ctx: ScenarioContext): WorldBible {
  return ctx.dataset as WorldBible;
}

const getWorldBible: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'getWorldBible',
      description: '获取漫剧 IP 的世界观、画风、基调（保持跨集一致性的设定来源）',
      parameters: { type: 'object', properties: {} },
    },
  },
  handler: async (_args, ctx) => {
    const wb = bible(ctx);
    return {
      ok: true,
      data: {
        ipName: wb.ipName,
        logline: wb.logline,
        worldview: wb.worldview,
        artStyle: wb.artStyle,
        tone: wb.tone,
      },
    };
  },
};

const listCharacters: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'listCharacters',
      description: '列出角色设定（姓名/人设/口吻/外观提示词），可按姓名筛选',
      parameters: {
        type: 'object',
        properties: { name: { type: 'string', description: '可选：按角色名筛选' } },
      },
    },
  },
  handler: async (args, ctx) => {
    const wb = bible(ctx);
    const name = typeof args.name === 'string' ? args.name.trim() : '';
    const characters = name ? wb.characters.filter((c) => c.name.includes(name)) : wb.characters;
    return { ok: true, data: { characters } };
  },
};

const getEpisode: ToolDefinition = {
  spec: {
    type: 'function',
    function: {
      name: 'getEpisode',
      description: '按集 id 获取示例剧情梗概（如 ep1/ep2），用于承接上下文',
      parameters: {
        type: 'object',
        properties: { episodeId: { type: 'string', description: '集 id，如 ep1' } },
        required: ['episodeId'],
      },
    },
  },
  handler: async (args, ctx) => {
    const wb = bible(ctx);
    const episodeId = typeof args.episodeId === 'string' ? args.episodeId : '';
    const episode = wb.episodes.find((e) => e.id === episodeId);
    if (!episode) {
      return { ok: false, error: `未找到集: ${episodeId}` };
    }
    return { ok: true, data: episode };
  },
};

export const comicTools: ToolDefinition[] = [getWorldBible, listCharacters, getEpisode];

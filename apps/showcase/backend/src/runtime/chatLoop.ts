/**
 * 场景运行时：统一编排引导步骤（creative D-C4）。
 *
 * 三种步骤执行模式：
 *  - generate：模型生成（创作引擎）——system 人设 + promptTemplate 插值 + 暴露
 *    工具给模型，跑 Function-Calling 多轮循环，每轮经 AegisGate。
 *  - tool：直接调用绑定工具 handler（纯数据操作，不经模型）。
 *  - guard：把候选文本送入网关护栏做合规预审（合规引擎）。
 *
 * 每轮抽取「价值事件」（缓存省量 / 路由模型 / 延迟 / 护栏决策）供价值面板。
 */

import type {
  AegisChatClient,
  ChatMessage,
  GuardrailDecision,
  ToolCall,
} from '../aegis/types';
import type {
  ScenarioContext,
  ScenarioEngine,
  ScenarioPlugin,
  ToolDefinition,
  UiStep,
} from '../scenarios/types';

export interface ValueEvent {
  ts: number;
  scenarioId: string;
  stepId: string;
  engine: ScenarioEngine;
  model: string;
  tokensSaved: number;
  cacheHit: boolean;
  latencyMs: number;
  guardrailBlocked: boolean;
  guardrailReason?: string;
}

export interface ToolInvocation {
  name: string;
  args: Record<string, unknown>;
  ok: boolean;
  error?: string;
}

export interface RunStepResult {
  output: string;
  events: ValueEvent[];
  toolInvocations: ToolInvocation[];
  guardrail?: GuardrailDecision;
  rounds: number;
}

export interface RunStepParams {
  plugin: ScenarioPlugin;
  step: UiStep;
  inputs: Record<string, unknown>;
  client: AegisChatClient;
  maxRounds?: number;
  onEvent?: (event: ValueEvent) => void;
  /**
   * 文本增量回调（用于 SSE 逐字输出）。仅 generate 步骤、且 client 实现了
   * chatStream 时生效；否则按非流式一次性返回。
   */
  onToken?: (text: string) => void;
}

const DEFAULT_MAX_ROUNDS = 5;

export function interpolate(template: string, vars: Record<string, unknown>): string {
  return template.replace(/\{\{\s*(\w+)\s*\}\}/g, (_match, key: string) => {
    const value = vars[key];
    return value === undefined || value === null ? '' : String(value);
  });
}

function buildContext(plugin: ScenarioPlugin, client: AegisChatClient): ScenarioContext {
  return {
    scenarioId: plugin.id,
    dataset: plugin.dataset,
    checkGuardrail: async (text: string) => {
      const result = await client.checkGuardrail(text, plugin.model.primary);
      return result.guardrail;
    },
  };
}

function findTool(plugin: ScenarioPlugin, name: string): ToolDefinition | undefined {
  return plugin.tools.find((t) => t.spec.function.name === name);
}

export async function runStep(params: RunStepParams): Promise<RunStepResult> {
  const { plugin, step } = params;
  const ctx = buildContext(plugin, params.client);
  const events: ValueEvent[] = [];
  const toolInvocations: ToolInvocation[] = [];
  const emit = (event: ValueEvent): void => {
    events.push(event);
    params.onEvent?.(event);
  };

  let result: RunStepResult;
  switch (step.kind) {
    case 'guard':
      result = await runGuardStep({ plugin, step, inputs: params.inputs, client: params.client, emit });
      break;
    case 'tool':
      result = await runToolStep({ plugin, step, inputs: params.inputs, ctx, toolInvocations });
      break;
    case 'generate':
    default:
      result = await runGenerateStep({
        plugin,
        step,
        inputs: params.inputs,
        client: params.client,
        ctx,
        maxRounds: params.maxRounds ?? DEFAULT_MAX_ROUNDS,
        emit,
        toolInvocations,
        onToken: params.onToken,
      });
      break;
  }
  // 统一回填运行时累积的价值事件（子函数内只负责 emit）。
  return { ...result, events };
}

/** 合规引擎：把文本送入网关护栏，报告决策。 */
async function runGuardStep(args: {
  plugin: ScenarioPlugin;
  step: UiStep;
  inputs: Record<string, unknown>;
  client: AegisChatClient;
  emit: (event: ValueEvent) => void;
}): Promise<RunStepResult> {
  const { plugin, step, inputs, client, emit } = args;
  const textKey = step.textInput ?? 'text';
  const text = String(inputs[textKey] ?? '');
  const { guardrail, meta } = await client.checkGuardrail(text, plugin.model.primary);

  emit({
    ts: Date.now(),
    scenarioId: plugin.id,
    stepId: step.id,
    engine: step.engine,
    model: meta.model,
    tokensSaved: meta.tokensSaved,
    cacheHit: meta.cacheHit,
    latencyMs: meta.latencyMs,
    guardrailBlocked: guardrail.blocked,
    guardrailReason: guardrail.reason,
  });

  const output = guardrail.blocked
    ? `🛡️ 合规预审未通过：${guardrail.reason ?? '内容触发护栏'}`
    : '✅ 合规预审通过：内容符合发布规范。';

  return { output, events: [], toolInvocations: [], guardrail, rounds: 1 };
}

/** 直接工具调用（纯数据操作，不经模型）。 */
async function runToolStep(args: {
  plugin: ScenarioPlugin;
  step: UiStep;
  inputs: Record<string, unknown>;
  ctx: ScenarioContext;
  toolInvocations: ToolInvocation[];
}): Promise<RunStepResult> {
  const { plugin, step, inputs, ctx, toolInvocations } = args;
  const toolName = step.tool;
  if (!toolName) {
    throw new Error(`步骤 ${step.id} 为 tool 类型但未绑定 tool`);
  }
  const tool = findTool(plugin, toolName);
  if (!tool) {
    throw new Error(`步骤 ${step.id} 绑定的工具不存在: ${toolName}`);
  }
  const result = await tool.handler(inputs, ctx);
  toolInvocations.push({ name: toolName, args: inputs, ok: result.ok, error: result.error });
  const output = result.ok
    ? JSON.stringify(result.data, null, 2)
    : `工具执行失败：${result.error ?? '未知错误'}`;
  return { output, events: [], toolInvocations, rounds: 0 };
}

/** 创作引擎：模型生成 + Function-Calling 多轮循环。 */
async function runGenerateStep(args: {
  plugin: ScenarioPlugin;
  step: UiStep;
  inputs: Record<string, unknown>;
  client: AegisChatClient;
  ctx: ScenarioContext;
  maxRounds: number;
  emit: (event: ValueEvent) => void;
  toolInvocations: ToolInvocation[];
  onToken?: (text: string) => void;
}): Promise<RunStepResult> {
  const { plugin, step, inputs, client, ctx, maxRounds, emit, toolInvocations, onToken } = args;

  const userPrompt = interpolate(step.promptTemplate ?? '', inputs);
  const messages: ChatMessage[] = [
    { role: 'system', content: plugin.systemPrompt },
    { role: 'user', content: userPrompt },
  ];
  const toolSpecs = plugin.tools.map((t) => t.spec);
  const useStream = typeof onToken === 'function' && typeof client.chatStream === 'function';

  let rounds = 0;
  let lastContent = '';

  while (rounds < maxRounds) {
    rounds += 1;
    const req = {
      model: plugin.model.primary,
      messages,
      tools: toolSpecs.length > 0 ? toolSpecs : undefined,
    };
    const response = useStream
      ? await client.chatStream!(req, (delta) => onToken!(delta.content))
      : await client.chat(req);

    emit({
      ts: Date.now(),
      scenarioId: plugin.id,
      stepId: step.id,
      engine: step.engine,
      model: response.meta.model,
      tokensSaved: response.meta.tokensSaved,
      cacheHit: response.meta.cacheHit,
      latencyMs: response.meta.latencyMs,
      guardrailBlocked: response.guardrail?.blocked ?? false,
      guardrailReason: response.guardrail?.reason,
    });

    if (response.guardrail?.blocked) {
      return {
        output: `🛡️ 已被护栏拦截：${response.guardrail.reason ?? '内容触发安全护栏'}`,
        events: [],
        toolInvocations,
        guardrail: response.guardrail,
        rounds,
      };
    }

    const toolCalls = response.toolCalls ?? [];
    if (toolCalls.length === 0) {
      lastContent = response.content ?? '';
      return { output: lastContent, events: [], toolInvocations, rounds };
    }

    // 把 assistant 的 tool_calls 追加，再逐个执行 handler 回灌
    messages.push({ role: 'assistant', content: response.content, tool_calls: toolCalls });
    for (const call of toolCalls) {
      const toolMessage = await executeToolCall(plugin, call, ctx, toolInvocations);
      messages.push(toolMessage);
    }
  }

  return {
    output: lastContent || '（已达最大工具调用轮次，返回当前结果）',
    events: [],
    toolInvocations,
    rounds,
  };
}

async function executeToolCall(
  plugin: ScenarioPlugin,
  call: ToolCall,
  ctx: ScenarioContext,
  toolInvocations: ToolInvocation[]
): Promise<ChatMessage> {
  const name = call.function.name;
  const tool = findTool(plugin, name);
  let parsedArgs: Record<string, unknown> = {};
  try {
    parsedArgs = call.function.arguments ? JSON.parse(call.function.arguments) : {};
  } catch {
    parsedArgs = {};
  }

  if (!tool) {
    toolInvocations.push({ name, args: parsedArgs, ok: false, error: 'unknown tool' });
    return {
      role: 'tool',
      tool_call_id: call.id,
      name,
      content: JSON.stringify({ ok: false, error: `unknown tool: ${name}` }),
    };
  }

  try {
    const result = await tool.handler(parsedArgs, ctx);
    toolInvocations.push({ name, args: parsedArgs, ok: result.ok, error: result.error });
    return {
      role: 'tool',
      tool_call_id: call.id,
      name,
      content: JSON.stringify(result),
    };
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    toolInvocations.push({ name, args: parsedArgs, ok: false, error: message });
    return {
      role: 'tool',
      tool_call_id: call.id,
      name,
      content: JSON.stringify({ ok: false, error: message }),
    };
  }
}

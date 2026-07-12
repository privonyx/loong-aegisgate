/**
 * 场景插件契约（通用骨架核心抽象）。
 *
 * 设计原则（creative D-C4「工具即能力 + 运行时统一编排」）：插件只声明
 * `tools`（能力 + handler）与 `uiSteps`（引导式创作台步骤），运行时统一
 * 跑 Function-Calling 循环 / 护栏预审并抽取价值事件。换业务 = 换插件，
 * 运行时零改动。
 */

export type JSONSchema = Record<string, unknown>;

/** OpenAI Function-Calling 工具描述。 */
export interface ToolSpec {
  type: 'function';
  function: {
    name: string;
    description: string;
    parameters: JSONSchema;
  };
}

export interface ToolResult {
  ok: boolean;
  data?: unknown;
  error?: string;
}

/**
 * 工具执行上下文：只暴露场景数据集与受控的护栏检查能力。
 * 工具 handler **不得**有文件系统写 / 命令执行 / 任意网络副作用（SR-3）。
 */
export interface ScenarioContext {
  scenarioId: string;
  dataset: unknown;
  /** 受控护栏检查能力（由运行时注入，指向 AegisGate）。 */
  checkGuardrail: (text: string) => Promise<{ blocked: boolean; reason?: string; code?: string }>;
}

export interface ToolDefinition {
  spec: ToolSpec;
  handler: (args: Record<string, unknown>, ctx: ScenarioContext) => Promise<ToolResult>;
}

export type UiInputType = 'text' | 'number' | 'textarea' | 'select';

export interface UiInputSpec {
  name: string;
  label: string;
  type: UiInputType;
  required?: boolean;
  placeholder?: string;
  options?: { value: string; label: string }[];
  default?: string | number;
}

/** 引导步骤的执行类型。 */
export type UiStepKind =
  | 'generate' // 模型生成（创作引擎）：用 promptTemplate + 暴露 tools 给模型
  | 'tool' // 直接调用绑定工具（纯数据操作，不经模型）
  | 'guard'; // 护栏预审（合规引擎）：把文本送入网关护栏

export type ScenarioEngine = 'creation' | 'compliance';

export interface UiStep {
  id: string;
  label: string;
  description?: string;
  engine: ScenarioEngine;
  kind: UiStepKind;
  /** kind=generate：提示模板，{{var}} 由 inputs 插值。 */
  promptTemplate?: string;
  /** kind=tool：绑定的工具名（function.name）。 */
  tool?: string;
  /** kind=guard：待预审文本取自此 input 名（默认 'text'）。 */
  textInput?: string;
  inputs?: UiInputSpec[];
}

export interface ScenarioMeta {
  name: string;
  description: string;
  icon: string;
  accentColor: string;
}

export interface ScenarioPlugin {
  id: string;
  meta: ScenarioMeta;
  systemPrompt: string;
  tools: ToolDefinition[];
  uiSteps: UiStep[];
  dataset: unknown;
  guardrail: { ruleFile: string };
  model: { primary: string; fallback?: string };
}

/** 暴露给前端的场景摘要（不含 handler / dataset 内部细节）。 */
export interface ScenarioSummary {
  id: string;
  meta: ScenarioMeta;
  uiSteps: Array<Omit<UiStep, never>>;
  model: { primary: string; fallback?: string };
}

export function toScenarioSummary(plugin: ScenarioPlugin): ScenarioSummary {
  return {
    id: plugin.id,
    meta: plugin.meta,
    uiSteps: plugin.uiSteps,
    model: plugin.model,
  };
}

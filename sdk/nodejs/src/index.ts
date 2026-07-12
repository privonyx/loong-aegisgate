/**
 * @aegisgate/sdk - AegisGate AI Gateway Node.js/TypeScript SDK
 * @module @aegisgate/sdk
 */

export { AegisGateClient } from './client.js';
export type { AegisGateClientConfig, ChatOptions, ChatResult } from './types.js';
export type {
  ChatMessage,
  ChatCompletionRequest,
  ChatCompletionResponse,
  ChatCompletionChunk,
  ModelListResponse,
  HealthResponse,
  Usage,
} from './types.js';
export {
  AegisGateError,
  AegisGateAPIError,
  AegisGateAuthenticationError,
  AegisGateRateLimitError,
  AegisGateSecurityError,
  AegisGateConnectionError,
  AegisGateTimeoutError,
} from './errors.js';
export { MockAegisGateClient } from './testing.js';
export type { MockClientCallRecord } from './testing.js';

/**
 * AegisGate SDK 自定义异常类
 */

/** AegisGate SDK 基础异常 */
export class AegisGateError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'AegisGateError';
    Object.setPrototypeOf(this, AegisGateError.prototype);
  }
}

/** API 请求失败异常 */
export class AegisGateAPIError extends AegisGateError {
  readonly statusCode?: number;
  readonly errorCode?: string;
  /** 网关业务错误码（AegisGate 扩展） */
  readonly aegisCode?: string;
  readonly responseBody?: string;

  constructor(
    message: string,
    options?: {
      statusCode?: number;
      errorCode?: string;
      aegisCode?: string;
      responseBody?: string;
    }
  ) {
    super(message);
    this.name = 'AegisGateAPIError';
    this.statusCode = options?.statusCode;
    this.errorCode = options?.errorCode;
    this.aegisCode = options?.aegisCode;
    this.responseBody = options?.responseBody;
    Object.setPrototypeOf(this, AegisGateAPIError.prototype);
  }
}

/** 安全策略拒绝（403） */
export class AegisGateSecurityError extends AegisGateAPIError {
  constructor(
    message: string,
    options?: { errorCode?: string; aegisCode?: string; responseBody?: string }
  ) {
    super(message, { ...options, statusCode: 403 });
    this.name = 'AegisGateSecurityError';
    Object.setPrototypeOf(this, AegisGateSecurityError.prototype);
  }
}

/** 认证失败（401） */
export class AegisGateAuthenticationError extends AegisGateAPIError {
  constructor(message = '认证失败：无效或过期的 API Key', options?: { responseBody?: string }) {
    super(message, { ...options, statusCode: 401 });
    this.name = 'AegisGateAuthenticationError';
    Object.setPrototypeOf(this, AegisGateAuthenticationError.prototype);
  }
}

/** 请求频率超限（429） */
export class AegisGateRateLimitError extends AegisGateAPIError {
  constructor(message = '请求频率超限，请稍后重试', options?: { responseBody?: string }) {
    super(message, { ...options, statusCode: 429 });
    this.name = 'AegisGateRateLimitError';
    Object.setPrototypeOf(this, AegisGateRateLimitError.prototype);
  }
}

/** 网络连接异常 */
export class AegisGateConnectionError extends AegisGateError {
  constructor(message = '网络连接失败', cause?: unknown) {
    super(message);
    this.name = 'AegisGateConnectionError';
    if (cause instanceof Error) {
      this.cause = cause;
    }
    Object.setPrototypeOf(this, AegisGateConnectionError.prototype);
  }
}

/** 请求超时异常 */
export class AegisGateTimeoutError extends AegisGateError {
  constructor(message = '请求超时') {
    super(message);
    this.name = 'AegisGateTimeoutError';
    Object.setPrototypeOf(this, AegisGateTimeoutError.prototype);
  }
}

"""AegisGate SDK 自定义异常类。"""

from __future__ import annotations


class AegisGateError(Exception):
    """AegisGate SDK 基础异常。"""

    def __init__(self, message: str, *args: object) -> None:
        self.message = message
        super().__init__(message, *args)


class AegisGateAPIError(AegisGateError):
    """API 请求失败异常。"""

    def __init__(
        self,
        message: str,
        *,
        status_code: int | None = None,
        error_code: str | None = None,
        aegis_code: str | None = None,
        response_body: str | None = None,
    ) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.error_code = error_code
        self.aegis_code = aegis_code
        self.response_body = response_body


class AegisGateSecurityError(AegisGateAPIError):
    """安全策略拒绝（403）。"""

    def __init__(
        self,
        message: str,
        *,
        error_code: str | None = None,
        aegis_code: str | None = None,
        response_body: str | None = None,
    ) -> None:
        super().__init__(
            message,
            status_code=403,
            error_code=error_code,
            aegis_code=aegis_code,
            response_body=response_body,
        )


class AegisGateAuthenticationError(AegisGateAPIError):
    """认证失败（401）。"""

    pass


class AegisGateRateLimitError(AegisGateAPIError):
    """请求频率超限（429）。"""

    retry_after: float | None = None


class AegisGateConnectionError(AegisGateError):
    """网络连接异常。"""

    pass


class AegisGateTimeoutError(AegisGateError):
    """请求超时异常。"""

    pass

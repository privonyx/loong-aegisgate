"""AegisGate AI Gateway Python SDK。"""

__version__ = "0.1.0"

from .exceptions import (
    AegisGateAPIError,
    AegisGateAuthenticationError,
    AegisGateConnectionError,
    AegisGateError,
    AegisGateRateLimitError,
    AegisGateSecurityError,
    AegisGateTimeoutError,
)
from .types import (
    ChatChoice,
    ChatChoiceMessage,
    ChatCompletion,
    ChatCompletionChunk,
    HealthResponse,
    Message,
    ModelInfo,
    ModelList,
    ReloadResponse,
    TokenUsage,
)

__all__ = [
    "AegisGateClient",
    "AsyncAegisGateClient",
    "MockAegisGateClient",
    "AegisGateError",
    "AegisGateAPIError",
    "AegisGateAuthenticationError",
    "AegisGateRateLimitError",
    "AegisGateSecurityError",
    "AegisGateConnectionError",
    "AegisGateTimeoutError",
    "Message",
    "ChatCompletion",
    "ChatCompletionChunk",
    "ChatChoice",
    "ChatChoiceMessage",
    "TokenUsage",
    "ModelInfo",
    "ModelList",
    "HealthResponse",
    "ReloadResponse",
]


def __getattr__(name: str):
    """延迟加载依赖 httpx 的客户端与测试 Mock，便于在无 httpx 环境下仅测 Mock。"""
    if name == "AegisGateClient":
        from .client import AegisGateClient

        return AegisGateClient
    if name == "AsyncAegisGateClient":
        from .client import AsyncAegisGateClient

        return AsyncAegisGateClient
    if name == "MockAegisGateClient":
        from .testing import MockAegisGateClient

        return MockAegisGateClient
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


def __dir__() -> list[str]:
    return sorted(__all__)

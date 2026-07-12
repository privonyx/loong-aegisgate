"""AegisGate SDK 请求与响应类型定义。"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


# --- Chat Completions ---


@dataclass
class Message:
    """聊天消息。"""

    role: str  # "system" | "user" | "assistant"
    content: str


@dataclass
class TokenUsage:
    """Token 使用统计。"""

    prompt_tokens: int = 0
    completion_tokens: int = 0
    total_tokens: int = 0


@dataclass
class ChatChoiceMessage:
    """Chat 响应中的单条消息。"""

    role: str
    content: str


@dataclass
class ChatChoice:
    """Chat 响应中的单个选项。"""

    index: int
    message: ChatChoiceMessage
    finish_reason: str


@dataclass
class ChatCompletion:
    """Chat 完成响应（非流式）。"""

    id: str
    object: str
    model: str
    choices: list[ChatChoice]
    usage: TokenUsage | None = None

    @property
    def content(self) -> str:
        """获取第一条 assistant 回复的文本内容。"""
        if self.choices:
            return self.choices[0].message.content
        return ""


@dataclass
class ChatCompletionChunk:
    """Chat 流式响应中的单块。"""

    id: str
    object: str
    model: str
    choices: list[dict[str, Any]]


# --- Models ---


@dataclass
class ModelInfo:
    """模型信息。"""

    id: str
    object: str = "model"
    owned_by: str = ""


@dataclass
class ModelList:
    """模型列表响应。"""

    object: str = "list"
    data: list[ModelInfo] = field(default_factory=list)


# --- Health ---


@dataclass
class HealthResponse:
    """健康检查响应。"""

    status: str
    version: str


# --- Admin ---


@dataclass
class ReloadResponse:
    """配置重载响应。"""

    status: str
    message: str

"""无 HTTP 依赖的 Mock 客户端，供单元测试使用。"""

from __future__ import annotations

import itertools
from dataclasses import asdict
from typing import Any, Iterator

from .exceptions import AegisGateAPIError
from .types import (
    ChatChoice,
    ChatChoiceMessage,
    ChatCompletion,
    ChatCompletionChunk,
    HealthResponse,
    Message,
    ModelInfo,
    ModelList,
    TokenUsage,
)


def _prompt_from_messages(messages: list[Message] | list[dict[str, str]]) -> str:
    for m in reversed(messages):
        if isinstance(m, Message):
            if m.role == "user":
                return m.content
        elif m.get("role") == "user":
            return str(m.get("content", ""))
    return ""


def _estimate_usage(prompt: str, completion: str) -> TokenUsage:
    pt = max(1, len(prompt) // 4)
    ct = max(1, len(completion) // 4)
    return TokenUsage(
        prompt_tokens=pt,
        completion_tokens=ct,
        total_tokens=pt + ct,
    )


def _full_prompt_for_usage(messages: list[Message] | list[dict[str, str]]) -> str:
    parts: list[str] = []
    for m in messages:
        if isinstance(m, Message):
            parts.append(m.content)
        else:
            parts.append(str(m.get("content", "")))
    return "\n".join(parts)


class MockAegisGateClient:
    """
    不继承 ``AegisGateClient``，避免测试环境依赖 httpx。
    方法签名与同步客户端一致（``chat`` / ``chat_completions`` / ``models`` / ``health``）。
    """

    _id_seq = itertools.count(1)

    def __init__(self) -> None:
        self._chat_mocks: list[tuple[str, str, str]] = []
        self._error_mocks: list[tuple[str, str, str]] = []
        self._stream_mocks: list[tuple[str, list[str], str]] = []
        self._default_content: str | None = None
        self._default_model: str = "mock-model"
        self._calls: list[dict[str, Any]] = []

    def mock_chat(
        self, prompt_pattern: str, *, content: str, model: str = "mock-model"
    ) -> None:
        """注册子串匹配的非流式回复。"""
        self._chat_mocks.append((prompt_pattern, content, model))

    def mock_error(
        self, prompt_pattern: str, *, aegis_code: str, message: str
    ) -> None:
        """注册子串匹配的错误响应。"""
        self._error_mocks.append((prompt_pattern, aegis_code, message))

    def mock_stream(
        self,
        prompt_pattern: str,
        *,
        chunks: list[str],
        model: str = "mock-model",
    ) -> None:
        """注册子串匹配的流式分块（按 ``chunks`` 顺序产出）。"""
        self._stream_mocks.append((prompt_pattern, list(chunks), model))

    def set_default_response(self, content: str, model: str = "mock-model") -> None:
        """未匹配任何 mock 时的默认非流式回复。"""
        self._default_content = content
        self._default_model = model

    @property
    def calls(self) -> list[dict[str, Any]]:
        return list(self._calls)

    @property
    def call_count(self) -> int:
        return len(self._calls)

    def reset(self) -> None:
        self._chat_mocks.clear()
        self._error_mocks.clear()
        self._stream_mocks.clear()
        self._default_content = None
        self._default_model = "mock-model"
        self._calls.clear()

    def _first_match_pattern(
        self, prompt: str, entries: list[tuple[str, ...]]
    ) -> tuple[Any, ...] | None:
        for row in entries:
            pat = row[0]
            if pat and pat in prompt:
                return row
        return None

    def _next_completion_id(self) -> str:
        return f"mock-chatcmpl-{next(self._id_seq)}"

    def _make_completion(
        self, content: str, model: str, messages: list[Message] | list[dict[str, str]]
    ) -> ChatCompletion:
        prompt = _full_prompt_for_usage(messages)
        usage = _estimate_usage(prompt, content)
        return ChatCompletion(
            id=self._next_completion_id(),
            object="chat.completion",
            model=model,
            choices=[
                ChatChoice(
                    index=0,
                    message=ChatChoiceMessage(role="assistant", content=content),
                    finish_reason="stop",
                )
            ],
            usage=usage,
        )

    def _resolve_error(self, prompt: str) -> None:
        hit = self._first_match_pattern(prompt, self._error_mocks)
        if hit is None:
            return
        _pattern, code, msg = hit
        raise AegisGateAPIError(
            msg,
            status_code=400,
            error_code=code,
            aegis_code=code,
            response_body=None,
        )

    def chat(
        self,
        content: str,
        *,
        model: str = "gpt-4o",
        system: str | None = None,
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> ChatCompletion:
        self._calls.append(
            {
                "method": "chat",
                "content": content,
                "model": model,
                "system": system,
                "temperature": temperature,
                "max_tokens": max_tokens,
            }
        )
        messages: list[dict[str, str]] = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": content})
        return self._complete(messages, model, temperature, max_tokens, stream=False)

    def chat_completions(
        self,
        messages: list[Message] | list[dict[str, str]],
        *,
        model: str = "gpt-4o",
        temperature: float = 0.7,
        max_tokens: int = 1000,
        stream: bool = False,
        extra: dict[str, Any] | None = None,
    ) -> ChatCompletion | Iterator[dict[str, Any]]:
        record: dict[str, Any] = {
            "method": "chat_completions",
            "messages": [
                {"role": m.role, "content": m.content}
                if isinstance(m, Message)
                else dict(m)
                for m in messages
            ],
            "model": model,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": stream,
        }
        if extra:
            record["extra"] = dict(extra)
        self._calls.append(record)

        msgs: list[dict[str, str]] = [
            {"role": m.role, "content": m.content} if isinstance(m, Message) else m
            for m in messages
        ]
        if stream:
            return self._stream_iter(msgs, model, temperature, max_tokens)
        return self._complete(msgs, model, temperature, max_tokens, stream=False)

    def _complete(
        self,
        messages: list[dict[str, str]],
        model: str,
        temperature: float,
        max_tokens: int,
        stream: bool,
    ) -> ChatCompletion:
        del temperature, max_tokens, stream  # mock 忽略
        prompt = _prompt_from_messages(messages)
        self._resolve_error(prompt)

        hit = self._first_match_pattern(prompt, self._chat_mocks)
        if hit is not None:
            _p, content, mdl = hit
            return self._make_completion(content, mdl, messages)

        if self._default_content is not None:
            return self._make_completion(
                self._default_content, self._default_model, messages
            )

        raise AegisGateAPIError(
            f"未匹配的 prompt，请使用 mock_chat / set_default_response: {prompt!r}",
            status_code=404,
            error_code="MOCK_UNMATCHED",
            aegis_code="MOCK_UNMATCHED",
        )

    def _stream_iter(
        self,
        messages: list[dict[str, str]],
        model: str,
        temperature: float,
        max_tokens: int,
    ) -> Iterator[dict[str, Any]]:
        del temperature, max_tokens
        prompt = _prompt_from_messages(messages)
        self._resolve_error(prompt)

        hit = self._first_match_pattern(prompt, self._stream_mocks)
        chunks: list[str]
        mdl: str
        if hit is not None:
            _p, chunks, mdl = hit
        else:
            # 无流式 mock 时，用普通 mock 或默认拆成单块
            chat_hit = self._first_match_pattern(prompt, self._chat_mocks)
            if chat_hit is not None:
                chunks = [chat_hit[1]]
                mdl = chat_hit[2]
            elif self._default_content is not None:
                chunks = [self._default_content]
                mdl = self._default_model
            else:
                raise AegisGateAPIError(
                    f"未匹配的流式 prompt: {prompt!r}",
                    status_code=404,
                    error_code="MOCK_UNMATCHED",
                    aegis_code="MOCK_UNMATCHED",
                )

        def gen() -> Iterator[dict[str, Any]]:
            cid = self._next_completion_id()
            n = len(chunks)
            for i, text in enumerate(chunks):
                finish: str | None = "stop" if i == n - 1 else None
                chunk = ChatCompletionChunk(
                    id=cid,
                    object="chat.completion.chunk",
                    model=mdl,
                    choices=[
                        {
                            "index": 0,
                            "delta": {"content": text},
                            "finish_reason": finish,
                        }
                    ],
                )
                yield asdict(chunk)

        return gen()

    def models(self) -> ModelList:
        self._calls.append({"method": "models"})
        return ModelList(
            object="list",
            data=[
                ModelInfo(id="mock-model", object="model", owned_by="aegisgate-mock"),
            ],
        )

    def health(self) -> HealthResponse:
        self._calls.append({"method": "health"})
        return HealthResponse(status="ok", version="mock")

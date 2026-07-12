"""AegisGate AI Gateway Python 客户端。"""

from __future__ import annotations

import asyncio
import json
import random
import time
from typing import Any, AsyncIterator, Callable, Iterator

import httpx

from .exceptions import (
    AegisGateAPIError,
    AegisGateAuthenticationError,
    AegisGateConnectionError,
    AegisGateRateLimitError,
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

_RETRYABLE_STATUS_CODES = frozenset({429, 500, 502, 503, 504})


def _parse_retry_after(response: httpx.Response) -> float | None:
    """从 Retry-After 头解析等待秒数。"""
    header = response.headers.get("retry-after")
    if header is None:
        return None
    try:
        return float(header)
    except ValueError:
        return None


def _backoff_delay(attempt: int, base_delay: float, *, jitter: bool = True) -> float:
    """指数退避 + 可选随机 jitter。"""
    delay = base_delay * (2 ** attempt)
    if jitter:
        delay = delay * (0.5 + random.random() * 0.5)  # noqa: S311
    return delay


def _parse_chat_completion(data: dict[str, Any]) -> ChatCompletion:
    """将 JSON 解析为 ChatCompletion。"""
    choices = []
    for c in data.get("choices", []):
        msg = c.get("message", {})
        choices.append(
            ChatChoice(
                index=c.get("index", 0),
                message=ChatChoiceMessage(
                    role=msg.get("role", "assistant"),
                    content=msg.get("content", ""),
                ),
                finish_reason=c.get("finish_reason", "stop"),
            )
        )
    usage_data = data.get("usage")
    usage = (
        TokenUsage(
            prompt_tokens=usage_data.get("prompt_tokens", 0),
            completion_tokens=usage_data.get("completion_tokens", 0),
            total_tokens=usage_data.get("total_tokens", 0),
        )
        if usage_data
        else None
    )
    return ChatCompletion(
        id=data.get("id", ""),
        object=data.get("object", "chat.completion"),
        model=data.get("model", ""),
        choices=choices,
        usage=usage,
    )


def _parse_chat_chunk(line: str) -> ChatCompletionChunk | None:
    """解析 SSE 行中的 JSON 块。"""
    if line.startswith("data: "):
        payload = line[6:].strip()
        if payload == "[DONE]":
            return None
        try:
            data = json.loads(payload)
            return ChatCompletionChunk(
                id=data.get("id", ""),
                object=data.get("object", ""),
                model=data.get("model", ""),
                choices=data.get("choices", []),
            )
        except json.JSONDecodeError:
            return None
    return None


def _handle_response(response: httpx.Response) -> None:
    """根据 HTTP 状态码抛出相应异常，支持 Retry-After 头。"""
    if response.is_success:
        return
    try:
        body = response.json()
        msg = body.get("message", body.get("error", response.text))
        code = body.get("code", body.get("error_code", ""))
    except Exception:
        body = response.text
        msg = response.text
        code = ""

    retry_after = _parse_retry_after(response)

    if response.status_code == 401:
        raise AegisGateAuthenticationError(
            msg or "认证失败",
            status_code=401,
            error_code=code,
            response_body=response.text,
        )
    if response.status_code == 429:
        err = AegisGateRateLimitError(
            msg or "请求频率超限",
            status_code=429,
            error_code=code,
            response_body=response.text,
        )
        err.retry_after = retry_after  # type: ignore[attr-defined]
        raise err
    raise AegisGateAPIError(
        msg or f"API 请求失败: {response.status_code}",
        status_code=response.status_code,
        error_code=code,
        response_body=response.text,
    )


class _BaseClient:
    """客户端基类，封装通用逻辑。"""

    def __init__(
        self,
        *,
        api_key: str | None = None,
        base_url: str = "http://localhost:8080",
        timeout: float = 60.0,
        connect_timeout: float | None = None,
        read_timeout: float | None = None,
        max_retries: int = 3,
        retry_delay: float = 1.0,
        retry_jitter: bool = True,
        retry_on_status: frozenset[int] | None = None,
        trace_id: str | None = None,
        trace_headers: dict[str, str] | None = None,
        default_headers: dict[str, str] | None = None,
        pool_max_connections: int | None = None,
        pool_max_keepalive: int | None = None,
    ) -> None:
        self.api_key = api_key
        self.base_url = base_url.rstrip("/")
        self.max_retries = max_retries
        self.retry_delay = retry_delay
        self.retry_jitter = retry_jitter
        self.retry_on_status = retry_on_status or _RETRYABLE_STATUS_CODES

        self._timeout = httpx.Timeout(
            timeout,
            connect=connect_timeout,
            read=read_timeout,
        )

        self._trace_id = trace_id
        self._trace_headers = trace_headers or {}
        self._default_headers = default_headers or {}

        self._pool_limits = httpx.Limits(
            max_connections=pool_max_connections or 100,
            max_keepalive_connections=pool_max_keepalive or 20,
        )

    def _headers(self, *, auth: bool = True) -> dict[str, str]:
        h: dict[str, str] = {"Content-Type": "application/json"}
        if auth and self.api_key:
            h["Authorization"] = f"Bearer {self.api_key}"
        h.update(self._default_headers)
        h.update(self._trace_headers)
        if self._trace_id:
            h.setdefault("X-Trace-Id", self._trace_id)
        return h

    def _is_retryable_error(self, exc: Exception) -> bool:
        """判断异常是否可以重试。"""
        if isinstance(exc, (httpx.ConnectError, httpx.ConnectTimeout)):
            return True
        if isinstance(exc, httpx.ReadTimeout):
            return True
        if isinstance(exc, AegisGateRateLimitError):
            return True
        if isinstance(exc, AegisGateAPIError):
            return exc.status_code is not None and exc.status_code in self.retry_on_status
        return False

    def _retry(self, fn: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        """同步重试逻辑（指数退避 + jitter + Retry-After 支持）。"""
        last_err: Exception | None = None
        for attempt in range(self.max_retries):
            try:
                return fn(*args, **kwargs)
            except (httpx.ConnectError, httpx.ConnectTimeout, httpx.ReadTimeout) as e:
                last_err = e
                if attempt < self.max_retries - 1:
                    time.sleep(_backoff_delay(attempt, self.retry_delay, jitter=self.retry_jitter))
                else:
                    if isinstance(e, (httpx.ReadTimeout, httpx.ConnectTimeout)):
                        raise AegisGateTimeoutError(str(e)) from e
                    raise AegisGateConnectionError(str(e)) from e
            except AegisGateRateLimitError as e:
                last_err = e
                if attempt < self.max_retries - 1:
                    delay = _backoff_delay(attempt, self.retry_delay, jitter=self.retry_jitter)
                    time.sleep(delay)
                else:
                    raise
            except AegisGateAPIError as e:
                if e.status_code is not None and e.status_code in self.retry_on_status:
                    last_err = e
                    if attempt < self.max_retries - 1:
                        time.sleep(_backoff_delay(attempt, self.retry_delay, jitter=self.retry_jitter))
                    else:
                        raise
                else:
                    raise
            except AegisGateAuthenticationError:
                raise
        if last_err:
            raise last_err
        return None  # unreachable


class AegisGateClient(_BaseClient):
    """AegisGate 同步客户端。

    支持连接池复用、分离的 connect/read 超时、指数退避 + jitter 重试、
    429/5xx 自动重试、Retry-After 头尊重、分布式追踪头注入。
    """

    def __init__(
        self,
        *,
        api_key: str | None = None,
        base_url: str = "http://localhost:8080",
        timeout: float = 60.0,
        connect_timeout: float | None = None,
        read_timeout: float | None = None,
        max_retries: int = 3,
        retry_delay: float = 1.0,
        retry_jitter: bool = True,
        retry_on_status: frozenset[int] | None = None,
        trace_id: str | None = None,
        trace_headers: dict[str, str] | None = None,
        default_headers: dict[str, str] | None = None,
        pool_max_connections: int | None = None,
        pool_max_keepalive: int | None = None,
    ) -> None:
        super().__init__(
            api_key=api_key,
            base_url=base_url,
            timeout=timeout,
            connect_timeout=connect_timeout,
            read_timeout=read_timeout,
            max_retries=max_retries,
            retry_delay=retry_delay,
            retry_jitter=retry_jitter,
            retry_on_status=retry_on_status,
            trace_id=trace_id,
            trace_headers=trace_headers,
            default_headers=default_headers,
            pool_max_connections=pool_max_connections,
            pool_max_keepalive=pool_max_keepalive,
        )
        self._client: httpx.Client | None = None

    @property
    def _http(self) -> httpx.Client:
        if self._client is None:
            self._client = httpx.Client(
                base_url=self.base_url,
                timeout=self._timeout,
                headers=self._headers(),
                limits=self._pool_limits,
            )
        return self._client

    def close(self) -> None:
        """关闭 HTTP 客户端，释放连接池资源。"""
        if self._client is not None:
            self._client.close()
            self._client = None

    def __enter__(self) -> AegisGateClient:
        return self

    def __exit__(self, *args: object) -> None:
        self.close()

    def chat(
        self,
        content: str,
        *,
        model: str = "gpt-4o",
        system: str | None = None,
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> ChatCompletion:
        """
        发送单轮对话并返回完整响应。

        Args:
            content: 用户消息内容
            model: 模型 ID
            system: 可选系统提示
            temperature: 采样温度
            max_tokens: 最大生成 token 数

        Returns:
            ChatCompletion 响应对象，可通过 .content 获取回复文本
        """
        messages: list[dict[str, str]] = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": content})
        return self.chat_completions(
            messages=messages,
            model=model,
            temperature=temperature,
            max_tokens=max_tokens,
            stream=False,
        )

    def chat_completions(
        self,
        messages: list[Message] | list[dict[str, str]],
        *,
        model: str = "gpt-4o",
        temperature: float = 0.7,
        max_tokens: int = 1000,
        stream: bool = False,
        extra: dict[str, Any] | None = None,
    ) -> ChatCompletion:
        """
        调用 OpenAI 兼容的 chat completions API（非流式）。

        Args:
            messages: 消息列表
            model: 模型 ID
            temperature: 采样温度
            max_tokens: 最大生成 token 数
            stream: 是否流式（此处必须为 False，流式请用 stream_chat）
            extra: 额外请求参数

        Returns:
            ChatCompletion 响应
        """
        msgs = [
            {"role": m.role, "content": m.content} if isinstance(m, Message) else m
            for m in messages
        ]
        body: dict[str, Any] = {
            "model": model,
            "messages": msgs,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": False,
        }
        if extra:
            body.update(extra)

        def _do() -> ChatCompletion:
            r = self._http.post(
                "/v1/chat/completions",
                json=body,
            )
            _handle_response(r)
            return _parse_chat_completion(r.json())

        return self._retry(_do)

    def stream_chat(
        self,
        content: str,
        *,
        model: str = "gpt-4o",
        system: str | None = None,
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> Iterator[ChatCompletionChunk]:
        """
        流式发送单轮对话，逐块返回响应。

        Yields:
            ChatCompletionChunk 流式块
        """
        messages: list[dict[str, str]] = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": content})
        yield from self.stream_chat_completions(
            messages=messages,
            model=model,
            temperature=temperature,
            max_tokens=max_tokens,
        )

    def stream_chat_completions(
        self,
        messages: list[Message] | list[dict[str, str]],
        *,
        model: str = "gpt-4o",
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> Iterator[ChatCompletionChunk]:
        """
        流式调用 chat completions API（SSE）。

        Yields:
            ChatCompletionChunk 流式块
        """
        msgs = [
            {"role": m.role, "content": m.content} if isinstance(m, Message) else m
            for m in messages
        ]
        body = {
            "model": model,
            "messages": msgs,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": True,
        }
        with self._http.stream(
            "POST",
            "/v1/chat/completions",
            json=body,
        ) as r:
            _handle_response(r)
            for line in r.iter_lines():
                chunk = _parse_chat_chunk(line)
                if chunk is not None:
                    yield chunk

    def models(self) -> ModelList:
        """获取可用模型列表。"""
        def _do() -> ModelList:
            r = self._http.get("/v1/models")
            _handle_response(r)
            data = r.json()
            return ModelList(
                object=data.get("object", "list"),
                data=[
                    ModelInfo(
                        id=m.get("id", ""),
                        object=m.get("object", "model"),
                        owned_by=m.get("owned_by", ""),
                    )
                    for m in data.get("data", [])
                ],
            )
        return self._retry(_do)

    def health(self) -> HealthResponse:
        """健康检查（无需认证）。"""
        r = self._http.get("/health")
        _handle_response(r)
        data = r.json()
        return HealthResponse(
            status=data.get("status", ""),
            version=data.get("version", ""),
        )

    def metrics(self) -> str:
        """获取 Prometheus 格式的指标。"""
        r = self._http.get("/metrics")
        _handle_response(r)
        return r.text

    def reload(self) -> ReloadResponse:
        """重载网关配置（管理员接口）。"""
        r = self._http.post("/admin/reload")
        _handle_response(r)
        data = r.json()
        return ReloadResponse(
            status=data.get("status", ""),
            message=data.get("message", ""),
        )


class AsyncAegisGateClient(_BaseClient):
    """AegisGate 异步客户端。

    支持连接池复用、分离的 connect/read 超时、指数退避 + jitter 重试、
    429/5xx 自动重试、Retry-After 头尊重、分布式追踪头注入。
    """

    def __init__(
        self,
        *,
        api_key: str | None = None,
        base_url: str = "http://localhost:8080",
        timeout: float = 60.0,
        connect_timeout: float | None = None,
        read_timeout: float | None = None,
        max_retries: int = 3,
        retry_delay: float = 1.0,
        retry_jitter: bool = True,
        retry_on_status: frozenset[int] | None = None,
        trace_id: str | None = None,
        trace_headers: dict[str, str] | None = None,
        default_headers: dict[str, str] | None = None,
        pool_max_connections: int | None = None,
        pool_max_keepalive: int | None = None,
    ) -> None:
        super().__init__(
            api_key=api_key,
            base_url=base_url,
            timeout=timeout,
            connect_timeout=connect_timeout,
            read_timeout=read_timeout,
            max_retries=max_retries,
            retry_delay=retry_delay,
            retry_jitter=retry_jitter,
            retry_on_status=retry_on_status,
            trace_id=trace_id,
            trace_headers=trace_headers,
            default_headers=default_headers,
            pool_max_connections=pool_max_connections,
            pool_max_keepalive=pool_max_keepalive,
        )
        self._client: httpx.AsyncClient | None = None

    @property
    def _http(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(
                base_url=self.base_url,
                timeout=self._timeout,
                headers=self._headers(),
                limits=self._pool_limits,
            )
        return self._client

    async def close(self) -> None:
        """关闭 HTTP 客户端，释放连接池资源。"""
        if self._client is not None:
            await self._client.aclose()
            self._client = None

    async def __aenter__(self) -> AsyncAegisGateClient:
        return self

    async def __aexit__(self, *args: object) -> None:
        await self.close()

    async def _retry_async(self, fn: Callable[..., Any], *args: Any, **kwargs: Any) -> Any:
        """异步重试逻辑（指数退避 + jitter + Retry-After 支持）。"""
        last_err: Exception | None = None
        for attempt in range(self.max_retries):
            try:
                return await fn(*args, **kwargs)
            except (httpx.ConnectError, httpx.ConnectTimeout, httpx.ReadTimeout) as e:
                last_err = e
                if attempt < self.max_retries - 1:
                    await asyncio.sleep(_backoff_delay(attempt, self.retry_delay, jitter=self.retry_jitter))
                else:
                    if isinstance(e, (httpx.ReadTimeout, httpx.ConnectTimeout)):
                        raise AegisGateTimeoutError(str(e)) from e
                    raise AegisGateConnectionError(str(e)) from e
            except AegisGateRateLimitError as e:
                last_err = e
                if attempt < self.max_retries - 1:
                    delay = _backoff_delay(attempt, self.retry_delay, jitter=self.retry_jitter)
                    await asyncio.sleep(delay)
                else:
                    raise
            except AegisGateAPIError as e:
                if e.status_code is not None and e.status_code in self.retry_on_status:
                    last_err = e
                    if attempt < self.max_retries - 1:
                        await asyncio.sleep(_backoff_delay(attempt, self.retry_delay, jitter=self.retry_jitter))
                    else:
                        raise
                else:
                    raise
            except AegisGateAuthenticationError:
                raise
        if last_err:
            raise last_err
        return None

    async def chat(
        self,
        content: str,
        *,
        model: str = "gpt-4o",
        system: str | None = None,
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> ChatCompletion:
        """异步发送单轮对话。"""
        messages: list[dict[str, str]] = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": content})
        return await self.chat_completions(
            messages=messages,
            model=model,
            temperature=temperature,
            max_tokens=max_tokens,
            stream=False,
        )

    async def chat_completions(
        self,
        messages: list[Message] | list[dict[str, str]],
        *,
        model: str = "gpt-4o",
        temperature: float = 0.7,
        max_tokens: int = 1000,
        stream: bool = False,
        extra: dict[str, Any] | None = None,
    ) -> ChatCompletion:
        """异步调用 chat completions API（非流式）。"""
        msgs = [
            {"role": m.role, "content": m.content} if isinstance(m, Message) else m
            for m in messages
        ]
        body: dict[str, Any] = {
            "model": model,
            "messages": msgs,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": False,
        }
        if extra:
            body.update(extra)

        async def _do() -> ChatCompletion:
            r = await self._http.post("/v1/chat/completions", json=body)
            _handle_response(r)
            return _parse_chat_completion(r.json())

        return await self._retry_async(_do)

    async def stream_chat(
        self,
        content: str,
        *,
        model: str = "gpt-4o",
        system: str | None = None,
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> AsyncIterator[ChatCompletionChunk]:
        """异步流式单轮对话。"""
        messages: list[dict[str, str]] = []
        if system:
            messages.append({"role": "system", "content": system})
        messages.append({"role": "user", "content": content})
        async for chunk in self.stream_chat_completions(
            messages=messages,
            model=model,
            temperature=temperature,
            max_tokens=max_tokens,
        ):
            yield chunk

    async def stream_chat_completions(
        self,
        messages: list[Message] | list[dict[str, str]],
        *,
        model: str = "gpt-4o",
        temperature: float = 0.7,
        max_tokens: int = 1000,
    ) -> AsyncIterator[ChatCompletionChunk]:
        """异步流式 chat completions。"""
        msgs = [
            {"role": m.role, "content": m.content} if isinstance(m, Message) else m
            for m in messages
        ]
        body = {
            "model": model,
            "messages": msgs,
            "temperature": temperature,
            "max_tokens": max_tokens,
            "stream": True,
        }
        async with self._http.stream("POST", "/v1/chat/completions", json=body) as r:
            _handle_response(r)
            async for line in r.aiter_lines():
                chunk = _parse_chat_chunk(line)
                if chunk is not None:
                    yield chunk

    async def models(self) -> ModelList:
        """异步获取模型列表。"""
        async def _do() -> ModelList:
            r = await self._http.get("/v1/models")
            _handle_response(r)
            data = r.json()
            return ModelList(
                object=data.get("object", "list"),
                data=[
                    ModelInfo(
                        id=m.get("id", ""),
                        object=m.get("object", "model"),
                        owned_by=m.get("owned_by", ""),
                    )
                    for m in data.get("data", [])
                ],
            )
        return await self._retry_async(_do)

    async def health(self) -> HealthResponse:
        """异步健康检查。"""
        r = await self._http.get("/health")
        _handle_response(r)
        data = r.json()
        return HealthResponse(
            status=data.get("status", ""),
            version=data.get("version", ""),
        )

    async def metrics(self) -> str:
        """异步获取 Prometheus 指标。"""
        r = await self._http.get("/metrics")
        _handle_response(r)
        return r.text

    async def reload(self) -> ReloadResponse:
        """异步重载配置。"""
        r = await self._http.post("/admin/reload")
        _handle_response(r)
        data = r.json()
        return ReloadResponse(
            status=data.get("status", ""),
            message=data.get("message", ""),
        )

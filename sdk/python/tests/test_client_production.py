"""SDK 生产化增强功能测试 — 重试、超时、连接池、追踪注入。"""

from __future__ import annotations

import time
from unittest.mock import MagicMock, patch

import httpx
import pytest

from aegisgate.client import (
    AegisGateClient,
    AsyncAegisGateClient,
    _backoff_delay,
    _parse_retry_after,
    _BaseClient,
    _RETRYABLE_STATUS_CODES,
)
from aegisgate.exceptions import (
    AegisGateAPIError,
    AegisGateAuthenticationError,
    AegisGateConnectionError,
    AegisGateRateLimitError,
    AegisGateTimeoutError,
)


# ─── 工具函数测试 ───

class TestBackoffDelay:
    def test_exponential_growth(self):
        d0 = _backoff_delay(0, 1.0, jitter=False)
        d1 = _backoff_delay(1, 1.0, jitter=False)
        d2 = _backoff_delay(2, 1.0, jitter=False)
        assert d0 == pytest.approx(1.0)
        assert d1 == pytest.approx(2.0)
        assert d2 == pytest.approx(4.0)

    def test_jitter_range(self):
        delays = [_backoff_delay(1, 1.0, jitter=True) for _ in range(100)]
        assert all(1.0 <= d <= 2.0 for d in delays)

    def test_custom_base_delay(self):
        d = _backoff_delay(0, 0.5, jitter=False)
        assert d == pytest.approx(0.5)


class TestParseRetryAfter:
    def test_valid_header(self):
        resp = MagicMock(spec=httpx.Response)
        resp.headers = {"retry-after": "3"}
        assert _parse_retry_after(resp) == pytest.approx(3.0)

    def test_float_header(self):
        resp = MagicMock(spec=httpx.Response)
        resp.headers = {"retry-after": "1.5"}
        assert _parse_retry_after(resp) == pytest.approx(1.5)

    def test_missing_header(self):
        resp = MagicMock(spec=httpx.Response)
        resp.headers = {}
        assert _parse_retry_after(resp) is None

    def test_invalid_header(self):
        resp = MagicMock(spec=httpx.Response)
        resp.headers = {"retry-after": "not-a-number"}
        assert _parse_retry_after(resp) is None


# ─── 客户端初始化测试 ───

class TestClientInit:
    def test_default_config(self):
        c = AegisGateClient(api_key="test-key")
        assert c.max_retries == 3
        assert c.retry_delay == 1.0
        assert c.retry_jitter is True
        assert c.retry_on_status == _RETRYABLE_STATUS_CODES
        assert c._trace_id is None
        assert c._trace_headers == {}
        assert c._default_headers == {}
        c.close()

    def test_custom_timeout_separation(self):
        c = AegisGateClient(
            api_key="k",
            timeout=30.0,
            connect_timeout=5.0,
            read_timeout=120.0,
        )
        assert c._timeout.connect == 5.0
        assert c._timeout.read == 120.0
        c.close()

    def test_custom_pool_limits(self):
        c = AegisGateClient(
            api_key="k",
            pool_max_connections=200,
            pool_max_keepalive=50,
        )
        assert c._pool_limits.max_connections == 200
        assert c._pool_limits.max_keepalive_connections == 50
        c.close()

    def test_trace_id_injection(self):
        c = AegisGateClient(api_key="k", trace_id="abc-123")
        headers = c._headers()
        assert headers["X-Trace-Id"] == "abc-123"
        c.close()

    def test_trace_headers_injection(self):
        c = AegisGateClient(
            api_key="k",
            trace_headers={"traceparent": "00-abc-def-01"},
        )
        headers = c._headers()
        assert headers["traceparent"] == "00-abc-def-01"
        c.close()

    def test_default_headers(self):
        c = AegisGateClient(
            api_key="k",
            default_headers={"X-Custom": "value"},
        )
        headers = c._headers()
        assert headers["X-Custom"] == "value"
        c.close()

    def test_custom_retry_on_status(self):
        custom = frozenset({429, 502})
        c = AegisGateClient(api_key="k", retry_on_status=custom)
        assert c.retry_on_status == custom
        c.close()

    def test_retry_jitter_disabled(self):
        c = AegisGateClient(api_key="k", retry_jitter=False)
        assert c.retry_jitter is False
        c.close()


# ─── 异步客户端初始化测试 ───

class TestAsyncClientInit:
    def test_default_config(self):
        c = AsyncAegisGateClient(api_key="test-key")
        assert c.max_retries == 3
        assert c.retry_jitter is True

    def test_pool_limits(self):
        c = AsyncAegisGateClient(
            api_key="k",
            pool_max_connections=50,
            pool_max_keepalive=10,
        )
        assert c._pool_limits.max_connections == 50
        assert c._pool_limits.max_keepalive_connections == 10

    def test_trace_injection(self):
        c = AsyncAegisGateClient(
            api_key="k",
            trace_id="trace-xyz",
            trace_headers={"b3": "0"},
        )
        headers = c._headers()
        assert headers["X-Trace-Id"] == "trace-xyz"
        assert headers["b3"] == "0"


# ─── 重试逻辑测试 ───

class TestRetryLogic:
    def test_retries_on_connect_error(self):
        c = AegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise httpx.ConnectError("connection refused")

        with pytest.raises(AegisGateConnectionError):
            c._retry(failing_fn)
        assert call_count == 3

    def test_retries_on_read_timeout(self):
        c = AegisGateClient(api_key="k", max_retries=2, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise httpx.ReadTimeout("read timed out")

        with pytest.raises(AegisGateTimeoutError):
            c._retry(failing_fn)
        assert call_count == 2

    def test_retries_on_connect_timeout(self):
        c = AegisGateClient(api_key="k", max_retries=2, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise httpx.ConnectTimeout("connect timed out")

        with pytest.raises(AegisGateTimeoutError):
            c._retry(failing_fn)
        assert call_count == 2

    def test_retries_on_rate_limit(self):
        c = AegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise AegisGateRateLimitError("rate limited", status_code=429)

        with pytest.raises(AegisGateRateLimitError):
            c._retry(failing_fn)
        assert call_count == 3

    def test_retries_on_5xx_error(self):
        c = AegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise AegisGateAPIError("server error", status_code=502)

        with pytest.raises(AegisGateAPIError):
            c._retry(failing_fn)
        assert call_count == 3

    def test_no_retry_on_auth_error(self):
        c = AegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise AegisGateAuthenticationError("unauthorized", status_code=401)

        with pytest.raises(AegisGateAuthenticationError):
            c._retry(failing_fn)
        assert call_count == 1

    def test_no_retry_on_4xx_non_retryable(self):
        c = AegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def failing_fn():
            nonlocal call_count
            call_count += 1
            raise AegisGateAPIError("bad request", status_code=400)

        with pytest.raises(AegisGateAPIError):
            c._retry(failing_fn)
        assert call_count == 1

    def test_succeeds_after_transient_failure(self):
        c = AegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        def sometimes_failing():
            nonlocal call_count
            call_count += 1
            if call_count < 3:
                raise httpx.ConnectError("connection refused")
            return "success"

        result = c._retry(sometimes_failing)
        assert result == "success"
        assert call_count == 3


# ─── 异步重试逻辑测试 ───

class TestAsyncRetryLogic:
    @pytest.mark.asyncio
    async def test_retries_on_connect_error(self):
        c = AsyncAegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        async def failing_fn():
            nonlocal call_count
            call_count += 1
            raise httpx.ConnectError("connection refused")

        with pytest.raises(AegisGateConnectionError):
            await c._retry_async(failing_fn)
        assert call_count == 3

    @pytest.mark.asyncio
    async def test_retries_on_rate_limit(self):
        c = AsyncAegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        async def failing_fn():
            nonlocal call_count
            call_count += 1
            raise AegisGateRateLimitError("rate limited", status_code=429)

        with pytest.raises(AegisGateRateLimitError):
            await c._retry_async(failing_fn)
        assert call_count == 3

    @pytest.mark.asyncio
    async def test_no_retry_on_auth_error(self):
        c = AsyncAegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        async def failing_fn():
            nonlocal call_count
            call_count += 1
            raise AegisGateAuthenticationError("unauthorized", status_code=401)

        with pytest.raises(AegisGateAuthenticationError):
            await c._retry_async(failing_fn)
        assert call_count == 1

    @pytest.mark.asyncio
    async def test_succeeds_after_transient_failure(self):
        c = AsyncAegisGateClient(api_key="k", max_retries=3, retry_delay=0.01, retry_jitter=False)
        call_count = 0

        async def sometimes_failing():
            nonlocal call_count
            call_count += 1
            if call_count < 2:
                raise httpx.ConnectError("fail")
            return "ok"

        result = await c._retry_async(sometimes_failing)
        assert result == "ok"
        assert call_count == 2


# ─── 连接池复用测试 ───

class TestConnectionPool:
    def test_http_client_reused(self):
        c = AegisGateClient(api_key="k")
        client1 = c._http
        client2 = c._http
        assert client1 is client2
        c.close()

    def test_close_releases_client(self):
        c = AegisGateClient(api_key="k")
        _ = c._http
        assert c._client is not None
        c.close()
        assert c._client is None

    def test_context_manager(self):
        with AegisGateClient(api_key="k") as c:
            _ = c._http
            assert c._client is not None
        assert c._client is None


# ─── 追踪头注入测试 ───

class TestTraceInjection:
    def test_trace_id_in_headers(self):
        c = AegisGateClient(api_key="k", trace_id="req-001")
        h = c._headers()
        assert h["X-Trace-Id"] == "req-001"
        c.close()

    def test_trace_headers_override(self):
        c = AegisGateClient(
            api_key="k",
            trace_id="req-001",
            trace_headers={"X-Trace-Id": "custom-id", "traceparent": "00-abc-def-01"},
        )
        h = c._headers()
        assert h["X-Trace-Id"] == "custom-id"
        assert h["traceparent"] == "00-abc-def-01"
        c.close()

    def test_default_headers_present(self):
        c = AegisGateClient(
            api_key="k",
            default_headers={"X-App-Name": "my-app", "X-Env": "prod"},
        )
        h = c._headers()
        assert h["X-App-Name"] == "my-app"
        assert h["X-Env"] == "prod"
        c.close()

    def test_auth_header_present(self):
        c = AegisGateClient(api_key="secret-key")
        h = c._headers()
        assert h["Authorization"] == "Bearer secret-key"
        c.close()

    def test_no_auth_when_disabled(self):
        c = AegisGateClient(api_key="secret-key")
        h = c._headers(auth=False)
        assert "Authorization" not in h
        c.close()


# ─── retryable error 判断测试 ───

class TestIsRetryableError:
    def test_connect_error_is_retryable(self):
        c = _BaseClient(api_key="k")
        assert c._is_retryable_error(httpx.ConnectError("err")) is True

    def test_read_timeout_is_retryable(self):
        c = _BaseClient(api_key="k")
        assert c._is_retryable_error(httpx.ReadTimeout("err")) is True

    def test_rate_limit_is_retryable(self):
        c = _BaseClient(api_key="k")
        assert c._is_retryable_error(AegisGateRateLimitError("err", status_code=429)) is True

    def test_502_is_retryable(self):
        c = _BaseClient(api_key="k")
        assert c._is_retryable_error(AegisGateAPIError("err", status_code=502)) is True

    def test_400_is_not_retryable(self):
        c = _BaseClient(api_key="k")
        assert c._is_retryable_error(AegisGateAPIError("err", status_code=400)) is False

    def test_custom_retryable_status(self):
        c = _BaseClient(api_key="k", retry_on_status=frozenset({418}))
        assert c._is_retryable_error(AegisGateAPIError("err", status_code=418)) is True
        assert c._is_retryable_error(AegisGateAPIError("err", status_code=502)) is False

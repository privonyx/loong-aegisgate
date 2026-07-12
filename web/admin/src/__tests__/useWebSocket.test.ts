// TASK-20260602-01 Epic 6 — useWebSocket hook unit tests.
//
// 覆盖:
//   1. enabled=false 时不创建 WebSocket（onMessage 不被调用）
//   2. enabled=true 时创建连接 + 收到 message 调用 onMessage
//   3. JSON 解析失败时不抛错（不调用 onMessage）
//   4. close 后 3s 内重连

import { renderHook } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { useWebSocket } from '../hooks/useWebSocket';

// Mock WebSocket：捕获每次 new WebSocket(...) 的实例
class MockWebSocket {
  static OPEN = 1;
  static CLOSED = 3;
  static instances: MockWebSocket[] = [];
  url: string;
  readyState: number = 0;
  onopen: ((this: MockWebSocket) => void) | null = null;
  onmessage: ((this: MockWebSocket, ev: { data: string }) => void) | null = null;
  onclose: ((this: MockWebSocket) => void) | null = null;
  onerror: ((this: MockWebSocket) => void) | null = null;
  send = vi.fn();
  close = vi.fn(() => {
    this.readyState = MockWebSocket.CLOSED;
    this.onclose?.call(this);
  });
  constructor(url: string) {
    this.url = url;
    MockWebSocket.instances.push(this);
    // 同步触发 open（模拟立即握手）
    queueMicrotask(() => {
      this.readyState = MockWebSocket.OPEN;
      this.onopen?.call(this);
    });
  }
}

beforeEach(() => {
  MockWebSocket.instances.length = 0;
  (globalThis as unknown as { WebSocket: typeof MockWebSocket }).WebSocket =
    MockWebSocket;
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
});

describe('useWebSocket', () => {
  it('enabled=false 时不创建 WebSocket', () => {
    const onMessage = vi.fn();
    renderHook(() => useWebSocket({ onMessage, enabled: false }));
    expect(MockWebSocket.instances).toHaveLength(0);
    expect(onMessage).not.toHaveBeenCalled();
  });

  it('enabled=true 时创建 WebSocket 并在收到 message 时调用 onMessage', () => {
    const onMessage = vi.fn();
    renderHook(() => useWebSocket({ onMessage, enabled: true }));
    expect(MockWebSocket.instances).toHaveLength(1);

    const ws = MockWebSocket.instances[0]!;
    expect(ws.url).toContain('/admin/ws');

    // 模拟后端推送一条 nested envelope metrics 消息（与 Epic 1 修复后契约一致）
    const payload = {
      type: 'metrics',
      data: {
        total_requests: 42,
        active_tenants: 3,
        total_cost_records: 50,
        cache_hit_rate: 0.5,
      },
    };
    ws.onmessage?.call(ws, { data: JSON.stringify(payload) });

    expect(onMessage).toHaveBeenCalledTimes(1);
    expect(onMessage).toHaveBeenCalledWith(payload);
  });

  it('JSON 解析失败时不调用 onMessage（容错）', () => {
    const onMessage = vi.fn();
    renderHook(() => useWebSocket({ onMessage, enabled: true }));
    const ws = MockWebSocket.instances[0]!;
    ws.onmessage?.call(ws, { data: 'not-json-at-all' });
    expect(onMessage).not.toHaveBeenCalled();
  });

  it('连接关闭后 3 秒内重连', () => {
    const onMessage = vi.fn();
    renderHook(() => useWebSocket({ onMessage, enabled: true }));
    expect(MockWebSocket.instances).toHaveLength(1);

    // 模拟 server 关闭
    const ws = MockWebSocket.instances[0]!;
    ws.onclose?.call(ws);

    // 推进 3s timer → 应触发重连（第二个实例）
    vi.advanceTimersByTime(3000);
    expect(MockWebSocket.instances.length).toBeGreaterThanOrEqual(2);
  });
});

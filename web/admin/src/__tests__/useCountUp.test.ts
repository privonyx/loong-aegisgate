// TASK-20260528-01 Epic 2 — useCountUp hook tests
//
// 覆盖：
//   1. 收敛到目标值（ease-out-cubic / RAF）
//   2. NaN  → 0 兜底（SR3）
//   3. Infinity → 0 兜底（SR3）
//   4. 负值 → 0 兜底（SR3）
//   5. unmount 时 cancelAnimationFrame（SR3 / 不残留 RAF callback）

import { renderHook, act } from '@testing-library/react';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { useCountUp } from '../hooks/useCountUp';

beforeEach(() => {
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
});

describe('useCountUp (TASK-20260528-01 SR3)', () => {
  it('在 durationMs 内收敛到目标值', () => {
    const { result } = renderHook(() => useCountUp({ to: 1.44, durationMs: 800 }));
    act(() => {
      vi.advanceTimersByTime(1000);
    });
    expect(parseFloat(result.current)).toBeCloseTo(1.44, 2);
  });

  it('NaN 输入兜底为 0', () => {
    const { result } = renderHook(() => useCountUp({ to: NaN, durationMs: 100 }));
    act(() => {
      vi.advanceTimersByTime(500);
    });
    expect(parseFloat(result.current)).toBe(0);
  });

  it('Infinity 输入兜底为 0', () => {
    const { result } = renderHook(() => useCountUp({ to: Infinity, durationMs: 100 }));
    act(() => {
      vi.advanceTimersByTime(500);
    });
    expect(parseFloat(result.current)).toBe(0);
  });

  it('负值兜底为 0', () => {
    const { result } = renderHook(() => useCountUp({ to: -42, durationMs: 100 }));
    act(() => {
      vi.advanceTimersByTime(500);
    });
    expect(parseFloat(result.current)).toBe(0);
  });

  it('unmount 时取消 RAF 不抛错', () => {
    const { unmount } = renderHook(() => useCountUp({ to: 100, durationMs: 1000 }));
    expect(() => unmount()).not.toThrow();
    act(() => {
      vi.advanceTimersByTime(2000);
    });
  });

  it('decimals 选项控制小数位数', () => {
    const { result } = renderHook(() => useCountUp({ to: 1.234567, durationMs: 100, decimals: 3 }));
    act(() => {
      vi.advanceTimersByTime(500);
    });
    expect(result.current).toBe('1.235');
  });
});

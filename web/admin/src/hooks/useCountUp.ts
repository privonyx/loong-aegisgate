// TASK-20260528-01 — useCountUp hook (RAF-based number tween, 0 new deps)
//
// 用于 HeroCaseStudy 区的 counting-up 动效。0 第三方动效库依赖
// （连续 6 任务守住 0 新依赖原则）；happy-dom 20 已验证支持 requestAnimationFrame
// （参见 Toast.tsx 既有用法）。
//
// SR3 收敛性保证：
//   - NaN / Infinity / 负值 → 兜底为 0
//   - useEffect cleanup 时 cancelAnimationFrame，不残留 RAF 回调
//   - ease-out-cubic 缓动 → durationMs 内收敛到目标值

import { useEffect, useState, useRef } from 'react';

export interface UseCountUpOptions {
  to: number;
  durationMs?: number;
  decimals?: number;
}

function sanitize(n: number): number {
  return Number.isFinite(n) && n >= 0 ? n : 0;
}

export function useCountUp({ to, durationMs = 800, decimals = 2 }: UseCountUpOptions): string {
  const safeTo = sanitize(to);
  const [value, setValue] = useState<number>(safeTo);
  const rafRef = useRef<number | undefined>(undefined);
  const startValRef = useRef<number>(safeTo);

  useEffect(() => {
    const startVal = startValRef.current;
    if (startVal === safeTo) {
      setValue(safeTo);
      return;
    }
    const startTime = performance.now();
    const step = (now: number) => {
      const t = Math.min(1, (now - startTime) / durationMs);
      const eased = 1 - Math.pow(1 - t, 3);
      const current = startVal + (safeTo - startVal) * eased;
      setValue(current);
      if (t < 1) {
        rafRef.current = requestAnimationFrame(step);
      } else {
        startValRef.current = safeTo;
      }
    };
    rafRef.current = requestAnimationFrame(step);
    return () => {
      if (rafRef.current !== undefined) {
        cancelAnimationFrame(rafRef.current);
        rafRef.current = undefined;
      }
    };
  }, [safeTo, durationMs]);

  return value.toFixed(decimals);
}

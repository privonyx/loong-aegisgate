// TASK-20260602-01 Epic 4 — Shared admin-API fetch wrapper.
//
// 之前 api/client.ts 与 api/autonomy.ts 各持有一份完全相同的 request<T>()
// 实现 (~25 行 × 2)，违反 DRY 且任何 401 / 错误处理变更需要双份维护。
// 本模块抽取共享 wrapper，两个 client 仅保留 BASE 常量 + 1 行薄封装。
//
// 行为契约（保持 1:1 与原实现一致）:
//   - credentials: same-origin（依赖 aegis_session cookie 透传）
//   - Content-Type: application/json（caller 可覆盖）
//   - 401 → 跳转 /admin/login（但避开两种回路：path 本身是 /auth/login，
//     或当前页面已经在 /login）
//   - 非 2xx → 解析 { error: { code, message } } 包络抛 Error；按稳定 error.code
//     映射 i18n errors namespace（未知 code / 无 code → 回退后端 message）；
//     解析失败则用 resp.statusText
//   - 成功 → 返回 resp.json() as T
import i18n from '../i18n';

// TASK-20260703-02 — 携带 HTTP status + 后端稳定 code 的错误类型，
// 供调用方按状态分级处理（如 Guard 页 D5：404 空态 / 503 banner / 429 提示）。
// message 仍为本地化后的文案（与旧 `throw new Error(msg)` 行为一致），
// 故既有 `.rejects.toThrow(msg)` 断言不受影响。
export class ApiError extends Error {
    readonly status: number;
    readonly code?: string;
    constructor(message: string, status: number, code?: string) {
        super(message);
        this.name = 'ApiError';
        this.status = status;
        this.code = code;
    }
}

export async function adminFetch<T>(
    base: string,
    path: string,
    options?: RequestInit,
): Promise<T> {
    const resp = await fetch(`${base}${path}`, {
        credentials: 'same-origin',
        headers: { 'Content-Type': 'application/json', ...options?.headers },
        ...options,
    });
    if (resp.status === 401) {
        if (
            !path.includes('/auth/login') &&
            !window.location.pathname.endsWith('/login')
        ) {
            window.location.href = '/admin/login';
        }
        throw new Error('Unauthorized');
    }
    if (!resp.ok) {
        const err = await resp
            .json()
            .catch(() => ({ error: { message: resp.statusText } }));
        const code: string | undefined = err.error?.code;
        const backendMsg: string | undefined = err.error?.message;
        // 本地化策略：仅当后端 message 缺失或等于该 code 的通用英文默认值
        // （即未携带具体细节）时，按 error.code 本地化；否则后端 message 含
        // 动态细节（如 "max 365 days"），原样透传以免丢失信息。
        let msg: string;
        if (code) {
            const genericDefault = i18n.getResource('en-US', 'errors', code) as
                | string
                | undefined;
            if (!backendMsg || backendMsg === genericDefault) {
                msg = i18n.t(`errors:${code}`, {
                    defaultValue: backendMsg || resp.statusText,
                });
            } else {
                msg = backendMsg;
            }
        } else {
            msg = backendMsg || resp.statusText;
        }
        throw new ApiError(msg, resp.status, code);
    }
    return resp.json();
}

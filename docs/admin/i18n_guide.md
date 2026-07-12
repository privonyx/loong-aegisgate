# Web 管理后台国际化（i18n）指南

> 关联任务：TASK-20260614-03 · 技术栈：`i18next` + `react-i18next` · 支持语言：`zh-CN`（默认）、`en-US`

本后台已全量国际化。本文档说明架构、约定，以及**新增/修改文案时的操作流程**。

## 1. 架构概览

```
src/i18n/
  index.ts        # i18n 实例：支持语言、默认语言、检测顺序、初始化
  resources.ts    # 按 namespace 聚合所有 locale JSON
src/locales/
  zh-CN/*.json    # 中文文案（每个 namespace 一个文件）
  en-US/*.json    # 英文文案（结构与 zh-CN 完全对齐）
src/hooks/useLanguage.ts   # 当前语言 + 切换 + localStorage 持久化
src/lib/format.ts          # 按 locale 的数字/货币/日期格式化（Intl）
```

- **语言检测顺序**（`detectLang`）：`localStorage(STORAGE_KEY)` → `navigator.language` → 默认 `zh-CN`。
- **初始化**：`main.tsx` 通过 `import './i18n'` 触发副作用初始化；`App.tsx` 用 `I18nextProvider` 包裹。
- **持久化**：`useLanguage().setLang(next)` 写入 `localStorage` 并同步 `document.documentElement.lang`。
- **顶栏切换器**：`Layout.tsx` 的 `data-testid="lang-switcher"` `<select>`。语言名按各自语言显示（`中文` / `English`），不翻译。

## 2. Namespace 清单

| 类别 | namespace |
|---|---|
| 通用 | `common`（actions/status/table/layout/roleGuard/export 等）、`errors`（AEGIS-xxxx 错误码）、`nav` |
| 认证 | `auth`（Login/MfaChallenge）、`sso`、`account` |
| 业务页 | `dashboard` `tenants` `users` `apikeys` `audits` `costs` `savings` `finops` `forecast` `templates` `rules` |
| 营销 | `marketing`（hero/quality/analogy/cache，**A2 纯净分离**：随语言切换、无对端语言残留）|

## 3. 文案约定

- **插值**：用 `{{var}}`。例：`"确定删除租户 {{name}}？"` → `t('...', { name })`。
- **复用通用键**：通用动作/状态优先复用 `common`（如 `t('common:actions.cancel')`），避免在业务 namespace 重复。
- **行内强调**：需要在句中保留加粗等标签时，用 `<Trans>` + 占位组件（如 `marketing` 的 `analogy.equivalent` / `cache.intercepted` 用 `<b>` 占位包裹数字）。
- **后端错误码本地化**：`api/request.ts` 的 `adminFetch` 按 `AEGIS-xxxx` 映射到 `errors` namespace。**仅当后端返回的 message 为通用默认值（或缺失）时**才用本地化文案覆盖，否则保留后端的具体 message（如「最长 365 天」这类带参数的细节）。
- **格式化**：数字/货币/日期统一走 `lib/format.ts`，勿手写 `toLocaleString` 散落各处。
- **K2 用词红线**：`marketing` 区严禁出现 `certified` / `production-grade` / `enterprise-ready` / `guaranteed`，由 `savingsStorytelling.test.ts` 反向遍历 `marketing.json`（两语言）守护。

## 4. 新增 / 修改文案流程

1. 在 `src/locales/zh-CN/<ns>.json` 与 `src/locales/en-US/<ns>.json` **同步**增删键（保持结构一致）。
2. 若新增 namespace：在 `src/i18n/resources.ts` 同时为 `zh-CN` 和 `en-US` 注册。
3. 组件内：`const { t } = useTranslation('<ns>')`，用 `t('key', { 插值 })` 取词。
4. 跑测试：`npm test`（测试默认锁定 `zh-CN`，见 `src/test/setup.ts`）。
5. 验证英文：必要时在测试中 `i18n.changeLanguage('en-US')` 后断言（参考 `__tests__/i18n_e2e.test.tsx`）。

## 5. 测试策略

- 全局 `setup.ts` 在每个用例前将 i18n 复位为 `zh-CN`，因此既有中文断言无需改动。
- 语言切换链路由 `__tests__/i18n_e2e.test.tsx` 与 `__tests__/Layout.test.tsx` 覆盖（顶栏 + 导航 + 营销区 follow-locale + localStorage 持久化）。
- 关键 UI 文案使用 `data-testid` 选择器，避免文案变更打断结构性断言。

// TASK-20260614-03 — i18n 资源装配。
//
// 每个 namespace 一个 json 文件，按语言分目录。新增 namespace 时在此显式
// import 并登记到 resources（显式 import 比 import.meta.glob 对 tsc + vitest
// 更友好，且 namespace 增长可控）。

import zhCommon from '../locales/zh-CN/common.json';
import enCommon from '../locales/en-US/common.json';
import zhErrors from '../locales/zh-CN/errors.json';
import enErrors from '../locales/en-US/errors.json';
import zhNav from '../locales/zh-CN/nav.json';
import enNav from '../locales/en-US/nav.json';
import zhAuth from '../locales/zh-CN/auth.json';
import enAuth from '../locales/en-US/auth.json';
import zhSso from '../locales/zh-CN/sso.json';
import enSso from '../locales/en-US/sso.json';
import zhAccount from '../locales/zh-CN/account.json';
import enAccount from '../locales/en-US/account.json';
import zhDashboard from '../locales/zh-CN/dashboard.json';
import enDashboard from '../locales/en-US/dashboard.json';
import zhTenants from '../locales/zh-CN/tenants.json';
import enTenants from '../locales/en-US/tenants.json';
import zhUsers from '../locales/zh-CN/users.json';
import enUsers from '../locales/en-US/users.json';
import zhApikeys from '../locales/zh-CN/apikeys.json';
import enApikeys from '../locales/en-US/apikeys.json';
import zhAudits from '../locales/zh-CN/audits.json';
import enAudits from '../locales/en-US/audits.json';
import zhCosts from '../locales/zh-CN/costs.json';
import enCosts from '../locales/en-US/costs.json';
import zhSavings from '../locales/zh-CN/savings.json';
import enSavings from '../locales/en-US/savings.json';
import zhFinops from '../locales/zh-CN/finops.json';
import enFinops from '../locales/en-US/finops.json';
import zhForecast from '../locales/zh-CN/forecast.json';
import enForecast from '../locales/en-US/forecast.json';
import zhTemplates from '../locales/zh-CN/templates.json';
import enTemplates from '../locales/en-US/templates.json';
import zhRules from '../locales/zh-CN/rules.json';
import enRules from '../locales/en-US/rules.json';
import zhMarketing from '../locales/zh-CN/marketing.json';
import enMarketing from '../locales/en-US/marketing.json';
import zhGuard from '../locales/zh-CN/guard.json';
import enGuard from '../locales/en-US/guard.json';

export const resources = {
  'zh-CN': {
    common: zhCommon,
    errors: zhErrors,
    nav: zhNav,
    auth: zhAuth,
    sso: zhSso,
    account: zhAccount,
    dashboard: zhDashboard,
    tenants: zhTenants,
    users: zhUsers,
    apikeys: zhApikeys,
    audits: zhAudits,
    costs: zhCosts,
    savings: zhSavings,
    finops: zhFinops,
    forecast: zhForecast,
    templates: zhTemplates,
    rules: zhRules,
    marketing: zhMarketing,
    guard: zhGuard,
  },
  'en-US': {
    common: enCommon,
    errors: enErrors,
    nav: enNav,
    auth: enAuth,
    sso: enSso,
    account: enAccount,
    dashboard: enDashboard,
    tenants: enTenants,
    users: enUsers,
    apikeys: enApikeys,
    audits: enAudits,
    costs: enCosts,
    savings: enSavings,
    finops: enFinops,
    forecast: enForecast,
    templates: enTemplates,
    rules: enRules,
    marketing: enMarketing,
    guard: enGuard,
  },
} as const;

export type Namespace = keyof (typeof resources)['zh-CN'];

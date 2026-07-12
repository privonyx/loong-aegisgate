// TASK-20260703-02 — Adaptive Guard 管理页。
// 闭环三端点：GET /admin/api/guard/explanation/{id}（决策解释）、POST /admin/api/guard/feedback
// （反馈标注）、POST /admin/api/guard/model/promote（模型晋升提案）。
//
// 设计：memory-bank/creative/creative-adaptive-guard-admin-ui.md（严格方案 A）。
//   - 反馈面板仅在解释成功加载（200）时渲染（严格 A）。
//   - SR-1：reviewer 身份由登录态派生（user_id + mapReviewerRole(role)），页面无可编辑控件。
//   - SR-3：解释文本/匹配模式已后端 PII-mask，前端原样展示，不落盘、不打印。
//   - SR-4：模型晋升为自治写操作 → ConfirmDialog 二次确认。
//   - D5：404 友好空态 / 503 未启用 banner / 429·409 分级 toast（不伪装成功）。
import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';
import {
  Search, SearchX, ShieldCheck, ShieldAlert, AlertTriangle, ChevronRight,
  ChevronDown, ArrowUpCircle, Loader2,
} from 'lucide-react';
import { useToast } from '../components/Toast';
import { useAuth } from '../hooks/useAuth';
import ConfirmDialog from '../components/ConfirmDialog';
import { guardApi, mapReviewerRole } from '../api/guard';
import { ApiError } from '../api/request';
import type { GuardExplanation, GuardFeedbackLabel } from '../types';

const ERROR_LABELS: GuardFeedbackLabel[] = ['false_positive', 'false_negative'];
const CONFIRM_LABELS: GuardFeedbackLabel[] = ['confirmed_block', 'confirmed_allow'];

const LABEL_STYLE: Record<GuardFeedbackLabel, string> = {
  false_positive: 'bg-warning/15 text-warning border-warning/30',
  false_negative: 'bg-danger/15 text-danger border-danger/30',
  confirmed_block: 'bg-success/15 text-success border-success/30',
  confirmed_allow: 'bg-accent/15 text-accent border-accent/30',
};

export default function Guard() {
  const { t } = useTranslation('guard');
  const { toast } = useToast();
  const { user } = useAuth();

  const [query, setQuery] = useState('');
  const [queriedId, setQueriedId] = useState('');
  const [loading, setLoading] = useState(false);
  const [explanation, setExplanation] = useState<GuardExplanation | null>(null);
  const [notFound, setNotFound] = useState(false);
  const [notWired, setNotWired] = useState(false);

  const [selectedLabel, setSelectedLabel] = useState<GuardFeedbackLabel | null>(null);
  const [comment, setComment] = useState('');
  const [submitting, setSubmitting] = useState(false);

  const [promoteOpen, setPromoteOpen] = useState(false);
  const [promoteAction, setPromoteAction] = useState(t('promote.actionDefault'));
  const [promoteModelId, setPromoteModelId] = useState('');
  const [promoteVersion, setPromoteVersion] = useState('');
  const [promoteConfirm, setPromoteConfirm] = useState(false);
  const [promoteSubmitting, setPromoteSubmitting] = useState(false);
  const [promoteDone, setPromoteDone] = useState(false);

  async function handleSearch() {
    const id = query.trim();
    if (!id) return;
    setLoading(true);
    setExplanation(null);
    setNotFound(false);
    setSelectedLabel(null);
    setComment('');
    try {
      const exp = await guardApi.getExplanation(id);
      setExplanation(exp);
      setQueriedId(id);
    } catch (e) {
      if (e instanceof ApiError && e.status === 404) {
        setNotFound(true);
      } else if (e instanceof ApiError && e.status === 503) {
        setNotWired(true);
      } else {
        toast('error', e instanceof Error ? e.message : String(e));
      }
    } finally {
      setLoading(false);
    }
  }

  async function handleSubmitFeedback() {
    if (!selectedLabel || !explanation) return;
    setSubmitting(true);
    try {
      await guardApi.submitFeedback({
        request_id: queriedId,
        label: selectedLabel,
        reviewer_user_id: user?.user_id ?? '',
        reviewer_role: mapReviewerRole(user?.role),
        comment: comment.trim() || undefined,
      });
      toast('success', t('feedback.success'));
      setSelectedLabel(null);
      setComment('');
    } catch (e) {
      if (e instanceof ApiError && e.status === 429) {
        toast('error', t('feedback.errors.rateLimited'));
      } else if (e instanceof ApiError && (e.status === 409 || e.status === 400)) {
        toast('error', t('feedback.errors.anomalyBlocked'));
      } else if (e instanceof ApiError && e.status === 503) {
        setNotWired(true);
      } else {
        toast('error', e instanceof Error ? e.message : String(e));
      }
    } finally {
      setSubmitting(false);
    }
  }

  function handlePromoteSubmit() {
    if (!promoteAction.trim() || !promoteModelId.trim() || !promoteVersion.trim()) {
      toast('error', t('promote.missingFields'));
      return;
    }
    setPromoteConfirm(true);
  }

  async function handlePromoteConfirm() {
    setPromoteConfirm(false);
    setPromoteSubmitting(true);
    try {
      await guardApi.promoteModel({
        action: promoteAction.trim(),
        model_id: promoteModelId.trim(),
        version: promoteVersion.trim(),
      });
      setPromoteDone(true);
      toast('success', t('promote.success'));
    } catch (e) {
      if (e instanceof ApiError && e.status === 503) {
        setNotWired(true);
      } else {
        toast('error', e instanceof Error ? e.message : String(e));
      }
    } finally {
      setPromoteSubmitting(false);
    }
  }

  const confidencePct = explanation ? Math.round(explanation.confidence * 100) : 0;
  const confidenceMet = explanation ? explanation.confidence >= explanation.threshold : false;

  return (
    <div className="space-y-6">
      <div>
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <p className="text-sm text-muted mt-1">{t('description')}</p>
      </div>

      {notWired && (
        <div
          data-testid="guard-not-wired-banner"
          className="flex items-center gap-2 rounded-lg border border-warning/30 bg-warning/10 px-4 py-3 text-sm text-warning"
        >
          <AlertTriangle size={16} />
          {t('states.notWired')}
        </div>
      )}

      {/* 解释查询 */}
      <div className="bg-card border border-border rounded-lg p-5 space-y-4">
        <label className="block text-sm font-medium text-fg" htmlFor="guard-search-input">
          {t('search.label')}
        </label>
        <div className="flex gap-2">
          <input
            id="guard-search-input"
            data-testid="guard-search-input"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={(e) => { if (e.key === 'Enter') handleSearch(); }}
            placeholder={t('search.placeholder')}
            className="flex-1 px-3 py-2 text-sm rounded-md bg-bg border border-border focus:outline-none focus:ring-1 focus:ring-primary"
          />
          <button
            data-testid="guard-search-button"
            onClick={handleSearch}
            disabled={loading || !query.trim()}
            className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors disabled:opacity-50"
          >
            {loading ? <Loader2 size={16} className="animate-spin" /> : <Search size={16} />}
            {t('search.button')}
          </button>
        </div>

        {!explanation && !notFound && !loading && (
          <p className="text-sm text-muted">{t('states.initial')}</p>
        )}

        {notFound && (
          <div
            data-testid="guard-explanation-empty"
            className="flex flex-col items-center justify-center gap-2 py-8 text-center text-muted"
          >
            <SearchX size={32} />
            <p className="text-sm max-w-md">{t('states.empty')}</p>
          </div>
        )}

        {explanation && (
          <div data-testid="guard-explanation-card" className="space-y-5">
            {/* 触发来源区 */}
            <section className="space-y-2">
              <h3 className="text-xs font-semibold uppercase tracking-wide text-muted">
                {t('explanation.triggerSection')}
              </h3>
              <div className="flex flex-wrap items-center gap-3">
                <span className="px-2 py-0.5 rounded bg-primary/10 text-primary text-xs font-medium">
                  {explanation.trigger_layer}
                </span>
                <span className="text-xs text-muted">
                  {t('explanation.triggerRuleId')}:{' '}
                  <span className="font-mono text-fg">{explanation.trigger_rule_id || t('explanation.empty')}</span>
                </span>
                <span className="text-xs text-muted">
                  {t('explanation.modelVersion')}:{' '}
                  <span className="font-mono text-fg">{explanation.model_version || t('explanation.empty')}</span>
                </span>
              </div>
            </section>

            {/* 判定数值区 */}
            <section className="space-y-2">
              <h3 className="text-xs font-semibold uppercase tracking-wide text-muted">
                {t('explanation.decisionSection')}
              </h3>
              <div className="flex items-center gap-3">
                <span className="text-xs text-muted w-20">{t('explanation.confidence')}</span>
                <div className="flex-1 h-2 rounded-full bg-bg overflow-hidden">
                  <div
                    className={`h-full ${confidenceMet ? 'bg-success' : 'bg-muted'}`}
                    style={{ width: `${confidencePct}%` }}
                  />
                </div>
                <span className="text-xs font-mono text-fg w-24 text-right">
                  {explanation.confidence.toFixed(2)} / {explanation.threshold.toFixed(2)}
                </span>
              </div>
            </section>

            {/* 匹配与解释区 */}
            <section className="space-y-2">
              <h3 className="text-xs font-semibold uppercase tracking-wide text-muted">
                {t('explanation.matchSection')}
              </h3>
              {explanation.matched_pattern && (
                <pre className="text-xs font-mono bg-bg/50 border border-border rounded p-3 overflow-x-auto text-fg">
                  {explanation.matched_pattern}
                </pre>
              )}
              <p className="text-sm text-fg">{explanation.explanation_text}</p>
            </section>

            {/* 反馈标注（严格 A：仅解释成功时出现） */}
            <div
              data-testid="guard-feedback-panel"
              className="border-t border-border pt-4 space-y-3"
            >
              <h3 className="text-sm font-medium text-fg">{t('feedback.title')}</h3>

              <div className="space-y-2">
                <p className="text-xs text-muted">{t('feedback.groupError')}</p>
                <div className="flex flex-wrap gap-2">
                  {ERROR_LABELS.map((label) => (
                    <LabelButton
                      key={label}
                      label={label}
                      text={t(`feedback.labels.${label}`)}
                      selected={selectedLabel === label}
                      onClick={() => setSelectedLabel(label)}
                    />
                  ))}
                </div>
                <p className="text-xs text-muted pt-1">{t('feedback.groupConfirm')}</p>
                <div className="flex flex-wrap gap-2">
                  {CONFIRM_LABELS.map((label) => (
                    <LabelButton
                      key={label}
                      label={label}
                      text={t(`feedback.labels.${label}`)}
                      selected={selectedLabel === label}
                      onClick={() => setSelectedLabel(label)}
                    />
                  ))}
                </div>
              </div>

              <textarea
                value={comment}
                onChange={(e) => setComment(e.target.value)}
                placeholder={t('feedback.commentPlaceholder')}
                rows={2}
                className="w-full px-3 py-2 text-sm rounded-md bg-bg border border-border focus:outline-none focus:ring-1 focus:ring-primary"
              />

              <button
                data-testid="guard-feedback-submit"
                onClick={handleSubmitFeedback}
                disabled={!selectedLabel || submitting}
                className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors disabled:opacity-50"
              >
                {submitting && <Loader2 size={16} className="animate-spin" />}
                {t('feedback.submit')}
              </button>
            </div>
          </div>
        )}
      </div>

      {/* 模型晋升折叠区 */}
      <div className="border-t border-border pt-4">
        <button
          data-testid="guard-promote-toggle"
          onClick={() => setPromoteOpen((v) => !v)}
          className="flex items-center gap-2 text-sm font-medium text-muted hover:text-fg transition-colors"
        >
          {promoteOpen ? <ChevronDown size={16} /> : <ChevronRight size={16} />}
          <ArrowUpCircle size={16} />
          {t('promote.title')}
        </button>

        {promoteOpen && (
          <div className="bg-card border border-border rounded-lg p-5 mt-3 space-y-3 max-w-lg">
            <Field label={t('promote.action')}>
              <input
                data-testid="guard-promote-action"
                value={promoteAction}
                onChange={(e) => setPromoteAction(e.target.value)}
                className="w-full px-3 py-2 text-sm rounded-md bg-bg border border-border focus:outline-none focus:ring-1 focus:ring-primary"
              />
            </Field>
            <Field label={t('promote.modelId')}>
              <input
                data-testid="guard-promote-model-id"
                value={promoteModelId}
                onChange={(e) => setPromoteModelId(e.target.value)}
                className="w-full px-3 py-2 text-sm rounded-md bg-bg border border-border focus:outline-none focus:ring-1 focus:ring-primary"
              />
            </Field>
            <Field label={t('promote.version')}>
              <input
                data-testid="guard-promote-version"
                value={promoteVersion}
                onChange={(e) => setPromoteVersion(e.target.value)}
                className="w-full px-3 py-2 text-sm rounded-md bg-bg border border-border focus:outline-none focus:ring-1 focus:ring-primary"
              />
            </Field>
            <button
              data-testid="guard-promote-submit"
              onClick={handlePromoteSubmit}
              disabled={promoteSubmitting}
              className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors disabled:opacity-50"
            >
              {promoteSubmitting && <Loader2 size={16} className="animate-spin" />}
              {t('promote.submit')}
            </button>

            {promoteDone && (
              <Link
                data-testid="guard-goto-finops"
                to="/finops"
                className="inline-flex items-center gap-1.5 text-sm text-accent hover:underline"
              >
                <ShieldCheck size={16} />
                {t('promote.gotoFinops')}
              </Link>
            )}
          </div>
        )}
      </div>

      <ConfirmDialog
        open={promoteConfirm}
        danger
        title={t('promote.confirmTitle')}
        message={t('promote.confirmMessage')}
        confirmLabel={t('promote.confirmLabel')}
        confirmTestId="guard-promote-confirm"
        onConfirm={handlePromoteConfirm}
        onCancel={() => setPromoteConfirm(false)}
      />
    </div>
  );
}

function LabelButton({
  label, text, selected, onClick,
}: {
  label: GuardFeedbackLabel;
  text: string;
  selected: boolean;
  onClick: () => void;
}) {
  const Icon = label.startsWith('confirmed') ? ShieldCheck : ShieldAlert;
  return (
    <button
      data-testid={`guard-label-${label}`}
      onClick={onClick}
      className={`flex items-center gap-1.5 px-3 py-1.5 text-xs font-medium rounded-md border transition-all ${LABEL_STYLE[label]} ${
        selected ? 'ring-2 ring-offset-1 ring-offset-card opacity-100' : 'opacity-70 hover:opacity-100'
      }`}
    >
      <Icon size={14} />
      {text}
    </button>
  );
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="space-y-1">
      <label className="block text-xs font-medium text-muted">{label}</label>
      {children}
    </div>
  );
}

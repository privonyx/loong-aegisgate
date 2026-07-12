import { useEffect, useState, useCallback, useMemo } from 'react';
import {
  LineChart, Line, XAxis, YAxis, Tooltip, ResponsiveContainer, Legend, CartesianGrid,
} from 'recharts';
import { useTranslation } from 'react-i18next';
import { TrendingUp, Activity, CalendarClock } from 'lucide-react';
import StatCard from '../components/StatCard';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { UsagePrediction, BudgetPrediction } from '../types';

// D5：history/forecast 天数 UI 上限钳制（防 DoS：限制后端预测计算跨度）。
const HISTORY_OPTIONS = [7, 14, 30, 60, 90];
const FORECAST_OPTIONS = [7, 14, 30];

interface ChartPoint {
  date: string;
  historical?: number;
  predicted?: number;
}

export default function Forecast() {
  const [usage, setUsage] = useState<UsagePrediction | null>(null);
  const [budget, setBudget] = useState<BudgetPrediction | null>(null);
  const [historyDays, setHistoryDays] = useState(30);
  const [forecastDays, setForecastDays] = useState(14);
  const [budgetInput, setBudgetInput] = useState(1000);
  const [loading, setLoading] = useState(true);
  const { toast } = useToast();
  const { t } = useTranslation('forecast');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const [u, b] = await Promise.all([
        api.predictUsage('', historyDays, forecastDays),
        api.predictBudget('', budgetInput, historyDays),
      ]);
      setUsage(u);
      setBudget(b);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [historyDays, forecastDays, budgetInput, toast, t]);

  useEffect(() => { load(); }, [load]);

  const chartData = useMemo<ChartPoint[]>(() => {
    if (!usage) return [];
    const hist = usage.historical.map(h => ({ date: h.date, historical: h.total_cost }));
    const pred = usage.predicted.map(p => ({ date: p.date, predicted: p.total_cost }));
    return [...hist, ...pred];
  }, [usage]);

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between flex-wrap gap-3">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <div className="flex items-center gap-3">
          <label className="flex items-center gap-1.5 text-sm text-muted">
            {t('historyDays')}
            <select
              aria-label={t('historyDays')}
              value={historyDays}
              onChange={e => setHistoryDays(Number(e.target.value))}
              className="text-sm"
            >
              {HISTORY_OPTIONS.map(d => <option key={d} value={d}>{d}</option>)}
            </select>
          </label>
          <label className="flex items-center gap-1.5 text-sm text-muted">
            {t('forecastDays')}
            <select
              aria-label={t('forecastDays')}
              value={forecastDays}
              onChange={e => setForecastDays(Number(e.target.value))}
              className="text-sm"
            >
              {FORECAST_OPTIONS.map(d => <option key={d} value={d}>{d}</option>)}
            </select>
          </label>
          <label className="flex items-center gap-1.5 text-sm text-muted">
            {t('budget')}
            <input
              aria-label={t('budgetAria')}
              type="number"
              value={budgetInput}
              onChange={e => setBudgetInput(Number(e.target.value))}
              className="w-24 text-sm"
            />
          </label>
        </div>
      </div>

      <div className="grid grid-cols-1 sm:grid-cols-3 gap-4">
        <StatCard
          title={t('stat.dailyTrend')}
          value={usage ? usage.daily_trend.toFixed(2) : '—'}
          icon={TrendingUp}
          accent={usage && usage.daily_trend >= 0 ? 'text-danger' : 'text-success'}
        />
        <StatCard
          title={t('stat.rSquared')}
          value={usage ? usage.r_squared.toFixed(2) : '—'}
          icon={Activity}
        />
        <StatCard
          title={t('stat.exhaustionDate')}
          value={budget?.budget_exhaustion_date || t('stat.sufficient')}
          icon={CalendarClock}
          subtitle={budget ? t('stat.budgetSubtitle', { budget: budget.budget }) : undefined}
        />
      </div>

      <div className="bg-card border border-border rounded-lg p-5">
        <h2 className="text-sm font-medium text-muted mb-3">{t('chart.title')}</h2>
        {loading ? (
          <div className="h-64 animate-pulse bg-border/30 rounded" />
        ) : chartData.length === 0 ? (
          <div className="h-64 flex items-center justify-center text-muted text-sm">{t('common:table.noData')}</div>
        ) : (
          <ResponsiveContainer width="100%" height={300}>
            <LineChart data={chartData}>
              <CartesianGrid strokeDasharray="3 3" stroke="var(--border)" />
              <XAxis dataKey="date" tick={{ fontSize: 11 }} />
              <YAxis tick={{ fontSize: 11 }} />
              <Tooltip />
              <Legend />
              <Line type="monotone" dataKey="historical" name={t('chart.historical')} stroke="var(--primary)" strokeWidth={2} dot={false} connectNulls />
              <Line type="monotone" dataKey="predicted" name={t('chart.predicted')} stroke="var(--success)" strokeWidth={2} strokeDasharray="5 5" dot={false} connectNulls />
            </LineChart>
          </ResponsiveContainer>
        )}
      </div>
    </div>
  );
}

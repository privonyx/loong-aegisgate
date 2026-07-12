import type { LucideIcon } from 'lucide-react';

interface StatCardProps {
  title: string;
  value: string | number;
  icon: LucideIcon;
  trend?: { value: number; label: string };
  accent?: string;
  // TASK-20260527-02 — optional subtitle line for Case Study Numbers row
  // (3 头条数字下方的细分 reason / cache hit type 等说明文本)。
  subtitle?: string;
}

export default function StatCard({
  title, value, icon: Icon, trend, accent = 'text-primary', subtitle,
}: StatCardProps) {
  return (
    <div className="bg-card border border-border rounded-lg p-5 flex items-start gap-4">
      <div className={`p-2.5 rounded-lg bg-primary/10 ${accent}`}>
        <Icon size={20} />
      </div>
      <div className="flex-1 min-w-0">
        <p className="text-sm text-muted truncate">{title}</p>
        <p className="text-2xl font-semibold mt-1 tracking-tight">{value}</p>
        {trend && (
          <p className={`text-xs mt-1 ${trend.value >= 0 ? 'text-success' : 'text-danger'}`}>
            {trend.value >= 0 ? '↑' : '↓'} {Math.abs(trend.value)}% {trend.label}
          </p>
        )}
        {subtitle && (
          <p className="text-xs text-muted mt-1 truncate">{subtitle}</p>
        )}
      </div>
    </div>
  );
}

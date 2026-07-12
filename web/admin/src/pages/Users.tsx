import { useEffect, useState, useCallback } from 'react';
import { Plus, Pencil, Trash2, X, Loader2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import DataTable, { type Column } from '../components/DataTable';
import ConfirmDialog from '../components/ConfirmDialog';
import { useToast } from '../components/Toast';
import { api } from '../api/client';
import type { User } from '../types';

const PAGE_SIZE = 20;

export default function UsersPage() {
  const [data, setData] = useState<User[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(0);
  const [loading, setLoading] = useState(true);
  const [showForm, setShowForm] = useState(false);
  const [editing, setEditing] = useState<User | null>(null);
  const [deleting, setDeleting] = useState<User | null>(null);
  const [filterTenant, setFilterTenant] = useState('');
  const { toast } = useToast();
  const { t } = useTranslation('users');

  const load = useCallback(async () => {
    setLoading(true);
    try {
      const res = await api.listUsers(filterTenant, PAGE_SIZE, page * PAGE_SIZE);
      setData(res.data);
      setTotal(res.total ?? res.count);
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.loadFailed'));
    } finally {
      setLoading(false);
    }
  }, [page, filterTenant, toast, t]);

  useEffect(() => { load(); }, [load]);

  const handleDelete = async () => {
    if (!deleting) return;
    try {
      await api.deleteUser(deleting.id);
      toast('success', t('toast.deleted'));
      setDeleting(null);
      load();
    } catch (e) {
      toast('error', e instanceof Error ? e.message : t('toast.deleteFailed'));
    }
  };

  const roleColor = (role: string) => {
    switch (role) {
      case 'super_admin': return 'bg-primary/10 text-primary';
      case 'tenant_admin': return 'bg-warning/10 text-warning';
      default: return 'bg-muted/20 text-muted';
    }
  };

  const columns: Column<User>[] = [
    { key: 'username', header: t('col.username') },
    { key: 'display_name', header: t('col.displayName') },
    { key: 'tenant_id', header: t('col.tenant'), render: (r) => <span className="font-mono text-xs">{r.tenant_id.slice(0, 8)}...</span> },
    { key: 'role', header: t('col.role'), render: (r) => <span className={`px-2 py-0.5 rounded text-xs font-medium ${roleColor(r.role)}`}>{r.role}</span> },
    { key: 'status', header: t('col.status'), render: (r) => <span className={`px-2 py-0.5 rounded text-xs font-medium ${r.status === 'active' ? 'bg-success/10 text-success' : 'bg-muted/20 text-muted'}`}>{r.status}</span> },
    { key: 'created_at', header: t('col.createdAt'), render: (r) => new Date(r.created_at).toLocaleString() },
  ];

  return (
    <div className="space-y-4">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold">{t('title')}</h1>
        <div className="flex items-center gap-3">
          <input
            placeholder={t('filterPlaceholder')}
            value={filterTenant}
            onChange={e => { setFilterTenant(e.target.value); setPage(0); }}
            className="text-sm w-48"
          />
          <button onClick={() => { setEditing(null); setShowForm(true); }} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 transition-colors">
            <Plus size={16} /> {t('newUser')}
          </button>
        </div>
      </div>

      <DataTable
        columns={columns}
        data={data}
        total={total}
        page={page}
        pageSize={PAGE_SIZE}
        onPageChange={setPage}
        loading={loading}
        actions={(row) => (
          <div className="flex items-center gap-1 justify-end">
            <button onClick={() => { setEditing(row); setShowForm(true); }} className="p-1.5 rounded text-muted hover:text-primary hover:bg-primary/10"><Pencil size={14} /></button>
            <button onClick={() => setDeleting(row)} className="p-1.5 rounded text-muted hover:text-danger hover:bg-danger/10"><Trash2 size={14} /></button>
          </div>
        )}
      />

      {showForm && <UserForm user={editing} onClose={() => setShowForm(false)} onSaved={() => { setShowForm(false); load(); }} />}

      <ConfirmDialog
        open={!!deleting}
        title={t('deleteTitle')}
        message={t('deleteConfirm', { username: deleting?.username })}
        confirmLabel={t('common:actions.delete')}
        danger
        onConfirm={handleDelete}
        onCancel={() => setDeleting(null)}
      />
    </div>
  );
}

function UserForm({ user, onClose, onSaved }: { user: User | null; onClose: () => void; onSaved: () => void }) {
  const [username, setUsername] = useState(user?.username ?? '');
  const [displayName, setDisplayName] = useState(user?.display_name ?? '');
  const [tenantId, setTenantId] = useState(user?.tenant_id ?? '');
  const [role, setRole] = useState(user?.role ?? 'viewer');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');
  const { toast } = useToast();
  const { t } = useTranslation('users');

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    try {
      if (user) {
        await api.updateUser(user.id, { display_name: displayName, role });
        toast('success', t('toast.updated'));
      } else {
        await api.createUser({ username, display_name: displayName, tenant_id: tenantId, role });
        toast('success', t('toast.created'));
      }
      onSaved();
    } catch (e) {
      setError(e instanceof Error ? e.message : t('toast.saveFailed'));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/50" onClick={onClose}>
      <div className="bg-card border border-border rounded-lg w-full max-w-md p-6" onClick={e => e.stopPropagation()}>
        <div className="flex items-center justify-between mb-4">
          <h2 className="font-semibold">{user ? t('editUser') : t('newUser')}</h2>
          <button onClick={onClose} className="text-muted hover:text-fg"><X size={18} /></button>
        </div>
        <form onSubmit={handleSubmit} className="space-y-3">
          {!user && (
            <>
              <div>
                <label className="block text-sm text-muted mb-1">{t('form.username')}</label>
                <input value={username} onChange={e => setUsername(e.target.value)} className="w-full" required />
              </div>
              <div>
                <label className="block text-sm text-muted mb-1">{t('form.tenantId')}</label>
                <input value={tenantId} onChange={e => setTenantId(e.target.value)} className="w-full" placeholder={t('form.tenantIdPlaceholder')} />
              </div>
            </>
          )}
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.displayName')}</label>
            <input value={displayName} onChange={e => setDisplayName(e.target.value)} className="w-full" />
          </div>
          <div>
            <label className="block text-sm text-muted mb-1">{t('form.role')}</label>
            <select value={role} onChange={e => setRole(e.target.value)} className="w-full">
              <option value="viewer">viewer</option>
              <option value="tenant_admin">tenant_admin</option>
              <option value="super_admin">super_admin</option>
            </select>
          </div>
          {error && <p className="text-sm text-danger bg-danger/10 rounded-md px-3 py-2">{error}</p>}
          <div className="flex justify-end gap-3 pt-2">
            <button type="button" onClick={onClose} className="px-4 py-2 text-sm rounded-md border border-border text-muted hover:text-fg transition-colors">{t('common:actions.cancel')}</button>
            <button type="submit" disabled={saving} className="flex items-center gap-1.5 px-4 py-2 text-sm rounded-md bg-primary text-white hover:bg-primary/90 disabled:opacity-50 transition-colors">
              {saving && <Loader2 size={14} className="animate-spin" />} {user ? t('common:actions.save') : t('create')}
            </button>
          </div>
        </form>
      </div>
    </div>
  );
}

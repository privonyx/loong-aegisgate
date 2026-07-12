import { lazy, Suspense } from 'react';
import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { I18nextProvider } from 'react-i18next';
import i18n from './i18n';
import { AuthContext, useAuthProvider } from './hooks/useAuth';
import { ToastProvider } from './components/Toast';
import ProtectedRoute from './components/ProtectedRoute';
import RoleGuard from './components/RoleGuard';
import Layout from './components/Layout';
import Login from './pages/Login';
import MfaChallenge from './pages/MfaChallenge';

const Dashboard = lazy(() => import('./pages/Dashboard'));
const Tenants = lazy(() => import('./pages/Tenants'));
const Users = lazy(() => import('./pages/Users'));
const ApiKeys = lazy(() => import('./pages/ApiKeys'));
const Audits = lazy(() => import('./pages/Audits'));
const Costs = lazy(() => import('./pages/Costs'));
const Savings = lazy(() => import('./pages/Savings'));
const FinOps = lazy(() => import('./pages/FinOps'));
const Templates = lazy(() => import('./pages/Templates'));
const Rules = lazy(() => import('./pages/Rules'));
const Sso = lazy(() => import('./pages/Sso'));
const Forecast = lazy(() => import('./pages/Forecast'));
const Guard = lazy(() => import('./pages/Guard'));
const AccountSecurity = lazy(() => import('./pages/AccountSecurity'));

function PageLoader() {
  return (
    <div className="flex items-center justify-center h-64">
      <div className="animate-spin rounded-full h-8 w-8 border-2 border-primary border-t-transparent" />
    </div>
  );
}

export default function App() {
  const auth = useAuthProvider();

  return (
    <I18nextProvider i18n={i18n}>
    <AuthContext.Provider value={auth}>
      <ToastProvider>
        <BrowserRouter basename="/admin">
          <Routes>
            <Route path="/login" element={<Login />} />
            <Route path="/mfa-challenge" element={<MfaChallenge />} />
            <Route
              path="/*"
              element={
                <ProtectedRoute>
                  <Layout>
                    <Suspense fallback={<PageLoader />}>
                      <Routes>
                        <Route index element={<Dashboard />} />
                        <Route path="tenants" element={<Tenants />} />
                        <Route path="users" element={<Users />} />
                        <Route path="keys" element={<ApiKeys />} />
                        <Route path="audits" element={<Audits />} />
                        <Route path="costs" element={<Costs />} />
                        <Route path="savings" element={<Savings />} />
                        <Route path="finops" element={<RoleGuard role="tenant_admin"><FinOps /></RoleGuard>} />
                        <Route path="templates" element={<Templates />} />
                        <Route path="rules" element={<Rules />} />
                        <Route path="guard" element={<RoleGuard role="tenant_admin"><Guard /></RoleGuard>} />
                        <Route path="sso" element={<RoleGuard role="super_admin"><Sso /></RoleGuard>} />
                        <Route path="forecast" element={<Forecast />} />
                        <Route path="account/security" element={<AccountSecurity />} />
                      </Routes>
                    </Suspense>
                  </Layout>
                </ProtectedRoute>
              }
            />
          </Routes>
        </BrowserRouter>
      </ToastProvider>
    </AuthContext.Provider>
    </I18nextProvider>
  );
}

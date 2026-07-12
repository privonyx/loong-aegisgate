import { render, screen } from '@testing-library/react';
import { vi, beforeEach } from 'vitest';

vi.mock('../api/client', () => ({
  api: {
    me: vi.fn().mockRejectedValue(new Error('Not authenticated')),
    login: vi.fn(),
    logout: vi.fn(),
  },
}));

import App from '../App';

describe('App', () => {
  beforeEach(() => {
    // BrowserRouter basename="/admin"：测试环境必须把 URL 切到 /admin/
    // 子路径下，否则 React Router 会警告 "Browser router with basename
    // /admin not matching window.location.pathname /"
    window.history.pushState({}, '', '/admin/');
  });

  it('mounts without crashing and shows login', async () => {
    render(<App />);
    expect(await screen.findByText('AegisGate')).toBeInTheDocument();
  });
});

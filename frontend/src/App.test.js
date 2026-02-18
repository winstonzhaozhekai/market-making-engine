import { render, screen } from '@testing-library/react';
import App from './App';

class MockWebSocket {
  static OPEN = 1;

  constructor() {
    this.readyState = MockWebSocket.OPEN;
    this.onopen = null;
    this.onclose = null;
    this.onerror = null;
    this.onmessage = null;

    setTimeout(() => {
      if (this.onopen) {
        this.onopen();
      }
    }, 0);
  }

  send() {}

  close() {
    if (this.onclose) {
      this.onclose();
    }
  }
}

describe('App', () => {
  beforeEach(() => {
    global.WebSocket = MockWebSocket;
  });

  test('renders controls and analytics sections', () => {
    render(<App />);
    expect(screen.getByText(/Run Controls/i)).toBeInTheDocument();
    expect(screen.getByText(/Run Metrics/i)).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: 'Comparison' })).toBeInTheDocument();
  });
});

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import './App.css';
import MetricsPanel from './components/MetricsPanel';
import RunControls from './components/RunControls';
import ComparisonView from './components/ComparisonView';

const DEFAULT_RUN_CONFIG = {
  seed: '42',
  strategy: 'heuristic',
  iterations: '1000',
  latencyMs: '10',
  maxNetPosition: '1000',
  maxNotionalExposure: '500000',
  maxDrawdown: '10000',
};

const MAX_LOG_MESSAGES = 4000;
const MAX_POINTS_PER_RUN = 2000;
const MAX_TRADES_PER_RUN = 5000;

const LOG_ROW_HEIGHT = 20;
const LOG_VIEWPORT_HEIGHT = 360;
const LOG_OVERSCAN_ROWS = 10;

function toFiniteNumber(value, fallback) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function normalizeRunConfig(config) {
  return {
    seed: Math.max(0, Math.floor(toFiniteNumber(config.seed, 42))),
    strategy: config.strategy === 'avellaneda-stoikov' ? 'avellaneda-stoikov' : 'heuristic',
    iterations: Math.max(1, Math.floor(toFiniteNumber(config.iterations, 1000))),
    latencyMs: Math.max(0, Math.floor(toFiniteNumber(config.latencyMs, 10))),
    maxNetPosition: Math.max(1, Math.floor(toFiniteNumber(config.maxNetPosition, 1000))),
    maxNotionalExposure: Math.max(1, toFiniteNumber(config.maxNotionalExposure, 500000)),
    maxDrawdown: Math.max(1, toFiniteNumber(config.maxDrawdown, 10000)),
  };
}

function pushCapped(existing, incoming, cap) {
  const merged = existing.concat(incoming);
  if (merged.length <= cap) {
    return merged;
  }
  return merged.slice(merged.length - cap);
}

function appendPointDecimated(existing, point, cap) {
  const appended = existing.concat(point);
  if (appended.length <= cap) {
    return appended;
  }

  const decimated = [];
  for (let i = 0; i < appended.length; i += 2) {
    decimated.push(appended[i]);
  }

  const lastPoint = appended[appended.length - 1];
  if (decimated[decimated.length - 1] !== lastPoint) {
    decimated.push(lastPoint);
  }
  return decimated;
}

function App() {
  const wsRef = useRef(null);
  const pendingRunMetaRef = useRef([]);
  const runMetaByIdRef = useRef(new Map());
  const queuedRunsRef = useRef([]);
  const isRunningRef = useRef(false);

  const [connectionStatus, setConnectionStatus] = useState('connecting');
  const [messages, setMessages] = useState([]);
  const [runsById, setRunsById] = useState({});
  const [activeRunId, setActiveRunId] = useState(null);
  const [selectedRunId, setSelectedRunId] = useState(null);
  const [isRunning, setIsRunning] = useState(false);
  const [runMode, setRunMode] = useState('single');
  const [singleConfig, setSingleConfig] = useState(DEFAULT_RUN_CONFIG);
  const [compareConfigA, setCompareConfigA] = useState(DEFAULT_RUN_CONFIG);
  const [compareConfigB, setCompareConfigB] = useState({
    ...DEFAULT_RUN_CONFIG,
    strategy: 'avellaneda-stoikov',
  });
  const [comparisonRunIds, setComparisonRunIds] = useState({ a: null, b: null });
  const [logScrollTop, setLogScrollTop] = useState(0);

  const updateRunning = useCallback((next) => {
    isRunningRef.current = next;
    setIsRunning(next);
  }, []);

  const appendMessage = useCallback((message) => {
    setMessages((prev) => {
      const next = prev.concat(message);
      if (next.length <= MAX_LOG_MESSAGES) {
        return next;
      }
      return next.slice(next.length - MAX_LOG_MESSAGES);
    });
  }, []);

  const sendRunConfigCommands = useCallback((config) => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) {
      return false;
    }

    wsRef.current.send(`set_seed:${config.seed}`);
    wsRef.current.send(`set_strategy:${config.strategy}`);
    wsRef.current.send(`set_iterations:${config.iterations}`);
    wsRef.current.send(`set_latency_ms:${config.latencyMs}`);
    wsRef.current.send(`set_max_net_position:${config.maxNetPosition}`);
    wsRef.current.send(`set_max_notional_exposure:${config.maxNotionalExposure}`);
    wsRef.current.send(`set_max_drawdown:${config.maxDrawdown}`);
    return true;
  }, []);

  const launchRun = useCallback((rawConfig, label, comparisonSlot = null) => {
    if (!wsRef.current || wsRef.current.readyState !== WebSocket.OPEN) {
      appendMessage('[local] websocket_not_connected');
      return false;
    }

    const config = normalizeRunConfig(rawConfig);
    pendingRunMetaRef.current.push({ config, label, comparisonSlot });

    if (!sendRunConfigCommands(config)) {
      return false;
    }

    wsRef.current.send('run_simulation');
    updateRunning(true);
    return true;
  }, [appendMessage, sendRunConfigCommands, updateRunning]);

  const maybeStartNextQueuedRun = useCallback(() => {
    if (isRunningRef.current) {
      return;
    }

    const next = queuedRunsRef.current.shift();
    if (!next) {
      return;
    }

    launchRun(next.config, next.label, next.slot);
  }, [launchRun]);

  useEffect(() => {
    const websocket = new WebSocket('ws://localhost:8080');
    const runMetaById = runMetaByIdRef.current;
    wsRef.current = websocket;

    websocket.onopen = () => {
      setConnectionStatus('connected');
    };

    websocket.onclose = () => {
      setConnectionStatus('closed');
      updateRunning(false);
    };

    websocket.onerror = () => {
      appendMessage('[local] websocket_error');
    };

    websocket.onmessage = (event) => {
      appendMessage(event.data);

      let data;
      try {
        data = JSON.parse(event.data);
      } catch (error) {
        return;
      }

      if (data.type === 'status') {
        if (data.status === 'connected') {
          setConnectionStatus('connected');
        }

        if (data.status === 'started' && typeof data.run_id === 'number') {
          const runId = data.run_id;
          const meta = pendingRunMetaRef.current.shift() || {
            config: normalizeRunConfig(DEFAULT_RUN_CONFIG),
            label: `Run ${runId}`,
            comparisonSlot: null,
          };

          runMetaByIdRef.current.set(runId, meta);
          updateRunning(true);
          setActiveRunId(runId);
          setSelectedRunId(runId);

          setRunsById((prev) => {
            const existing = prev[runId] || {};
            return {
              ...prev,
              [runId]: {
                runId,
                label: meta.label,
                config: meta.config,
                status: 'running',
                points: existing.points || [],
                trades: existing.trades || [],
                summary: existing.summary || null,
              },
            };
          });
        }

        if (data.status === 'stopped') {
          updateRunning(false);
          maybeStartNextQueuedRun();
        }

        return;
      }

      if (data.type === 'error') {
        if (typeof data.message === 'string' && (data.message.includes('simulation_') || data.message.includes('invalid_'))) {
          updateRunning(false);
          maybeStartNextQueuedRun();
        }
        return;
      }

      if (data.type === 'simulation_update' && typeof data.run_id === 'number') {
        const runId = data.run_id;
        const meta = runMetaByIdRef.current.get(runId) || {
          config: normalizeRunConfig(DEFAULT_RUN_CONFIG),
          label: `Run ${runId}`,
          comparisonSlot: null,
        };

        const trades = Array.isArray(data.trades) ? data.trades : [];
        const metrics = data.metrics || null;

        setRunsById((prev) => {
          const existing = prev[runId] || {
            runId,
            label: meta.label,
            config: meta.config,
            status: 'running',
            points: [],
            trades: [],
            summary: null,
          };

          const incomingTrades = trades.map((trade, idx) => ({
            iteration: data.iteration,
            price: trade.price,
            size: trade.size,
            side: trade.side,
            key: `${runId}-${data.iteration}-${idx}`,
          }));

          let points = existing.points;
          if (metrics) {
            points = appendPointDecimated(points, {
              iteration: data.iteration,
              inventory: toFiniteNumber(metrics.inventory, 0),
              spread: toFiniteNumber(data.spread, 0),
              realizedPnl: toFiniteNumber(metrics.realized_pnl, 0),
              unrealizedPnl: toFiniteNumber(metrics.unrealized_pnl, 0),
              totalPnl: toFiniteNumber(metrics.total_pnl, 0),
              drawdown: toFiniteNumber(metrics.drawdown, 0),
              markPrice: toFiniteNumber(metrics.mark_price, 0),
            }, MAX_POINTS_PER_RUN);
          }

          const next = {
            ...existing,
            runId,
            label: meta.label,
            config: meta.config,
            status: data.is_final ? 'completed' : 'running',
            points,
            trades: pushCapped(existing.trades, incomingTrades, MAX_TRADES_PER_RUN),
            summary: metrics || existing.summary,
            finishedAt: data.is_final ? Date.now() : existing.finishedAt,
          };

          return {
            ...prev,
            [runId]: next,
          };
        });

        setActiveRunId(runId);
        setSelectedRunId(runId);

        if (data.is_final) {
          updateRunning(false);
          if (meta.comparisonSlot === 'a' || meta.comparisonSlot === 'b') {
            setComparisonRunIds((prev) => ({
              ...prev,
              [meta.comparisonSlot]: runId,
            }));
          }
          maybeStartNextQueuedRun();
        }
      }
    };

    return () => {
      queuedRunsRef.current = [];
      pendingRunMetaRef.current = [];
      runMetaById.clear();
      websocket.close();
    };
  }, [appendMessage, maybeStartNextQueuedRun, updateRunning]);

  const runs = useMemo(() => {
    return Object.values(runsById).sort((a, b) => a.runId - b.runId);
  }, [runsById]);

  const selectedRun = useMemo(() => {
    if (selectedRunId !== null && runsById[selectedRunId]) {
      return runsById[selectedRunId];
    }
    if (activeRunId !== null && runsById[activeRunId]) {
      return runsById[activeRunId];
    }
    return runs.length > 0 ? runs[runs.length - 1] : null;
  }, [activeRunId, runs, runsById, selectedRunId]);

  const completedRuns = useMemo(() => {
    return runs.filter((run) => run.status === 'completed');
  }, [runs]);

  const fallbackRunA = completedRuns.length >= 2 ? completedRuns[completedRuns.length - 2] : null;
  const fallbackRunB = completedRuns.length >= 1 ? completedRuns[completedRuns.length - 1] : null;

  const comparisonRunA = comparisonRunIds.a !== null ? runsById[comparisonRunIds.a] : fallbackRunA;
  const comparisonRunB = comparisonRunIds.b !== null ? runsById[comparisonRunIds.b] : fallbackRunB;

  const handleStartSingle = useCallback(() => {
    queuedRunsRef.current = [];
    setComparisonRunIds({ a: null, b: null });
    launchRun(singleConfig, 'Single');
  }, [launchRun, singleConfig]);

  const handleStartComparison = useCallback(() => {
    setComparisonRunIds({ a: null, b: null });
    queuedRunsRef.current = [
      { config: compareConfigA, label: 'A', slot: 'a' },
      { config: compareConfigB, label: 'B', slot: 'b' },
    ];
    maybeStartNextQueuedRun();
  }, [compareConfigA, compareConfigB, maybeStartNextQueuedRun]);

  const handleStop = useCallback(() => {
    queuedRunsRef.current = [];
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      wsRef.current.send('stop_simulation');
    }
    updateRunning(false);
  }, [updateRunning]);

  const visibleRowCount = Math.ceil(LOG_VIEWPORT_HEIGHT / LOG_ROW_HEIGHT);
  const logStartIndex = Math.max(0, Math.floor(logScrollTop / LOG_ROW_HEIGHT) - LOG_OVERSCAN_ROWS);
  const logEndIndex = Math.min(
    messages.length,
    logStartIndex + visibleRowCount + LOG_OVERSCAN_ROWS * 2
  );
  const visibleMessages = messages.slice(logStartIndex, logEndIndex);

  return (
    <div className="appShell">
      <header className="topBar">
        <div>
          <h1>HFT Run Lab</h1>
          <p>Post-trade analysis dashboard with reproducible runs and A/B comparison.</p>
        </div>
        <div className={`connectionBadge connection-${connectionStatus}`}>
          {connectionStatus === 'connected' ? 'WS Connected' : 'WS Offline'}
        </div>
      </header>

      <main className="dashboardGrid">
        <section className="panel controlsPanel">
          <RunControls
            connectionStatus={connectionStatus}
            isRunning={isRunning}
            runMode={runMode}
            setRunMode={setRunMode}
            singleConfig={singleConfig}
            setSingleConfig={setSingleConfig}
            compareConfigA={compareConfigA}
            setCompareConfigA={setCompareConfigA}
            compareConfigB={compareConfigB}
            setCompareConfigB={setCompareConfigB}
            onRunSingle={handleStartSingle}
            onRunComparison={handleStartComparison}
            onStop={handleStop}
          />
        </section>

        <section className="panel metricsPanel">
          <div className="panelHeaderRow">
            <h2>Run Metrics</h2>
            <div className="runSelectorWrap">
              <label htmlFor="run-select">Inspect run</label>
              <select
                id="run-select"
                value={selectedRun ? selectedRun.runId : ''}
                onChange={(event) => setSelectedRunId(Number(event.target.value))}
                disabled={runs.length === 0}
              >
                {runs.length === 0 && <option value="">No runs yet</option>}
                {runs.map((run) => (
                  <option key={run.runId} value={run.runId}>
                    #{run.runId} {run.label} ({run.status})
                  </option>
                ))}
              </select>
            </div>
          </div>

          <MetricsPanel run={selectedRun} />
        </section>

        <section className="panel comparisonPanel">
          <ComparisonView runA={comparisonRunA} runB={comparisonRunB} />
        </section>

        <section className="panel loggerPanel">
          <div className="panelHeaderRow">
            <h2>WebSocket Log</h2>
            <span>{messages.length} messages (capped at {MAX_LOG_MESSAGES})</span>
          </div>
          <div
            className="logViewport"
            style={{ height: `${LOG_VIEWPORT_HEIGHT}px` }}
            onScroll={(event) => setLogScrollTop(event.currentTarget.scrollTop)}
          >
            <div
              className="logInner"
              style={{ height: `${messages.length * LOG_ROW_HEIGHT}px` }}
            >
              {visibleMessages.map((message, idx) => {
                const absoluteIndex = logStartIndex + idx;
                return (
                  <div
                    className="logLine"
                    key={absoluteIndex}
                    style={{
                      top: `${absoluteIndex * LOG_ROW_HEIGHT}px`,
                      height: `${LOG_ROW_HEIGHT}px`,
                      lineHeight: `${LOG_ROW_HEIGHT}px`,
                    }}
                  >
                    {message}
                  </div>
                );
              })}
            </div>
          </div>
        </section>
      </main>
    </div>
  );
}

export default App;

import React from 'react';

const STRATEGY_OPTIONS = [
  { value: 'heuristic', label: 'Heuristic' },
  { value: 'avellaneda-stoikov', label: 'Avellaneda-Stoikov' },
];

const FIELDS = [
  { key: 'seed', label: 'Seed', type: 'number', step: 1 },
  { key: 'iterations', label: 'Run Length', type: 'number', step: 1 },
  { key: 'latencyMs', label: 'Latency (ms)', type: 'number', step: 1 },
  { key: 'maxNetPosition', label: 'Max Net Position', type: 'number', step: 1 },
  { key: 'maxNotionalExposure', label: 'Max Notional', type: 'number', step: '0.01' },
  { key: 'maxDrawdown', label: 'Max Drawdown', type: 'number', step: '0.01' },
];

function ConfigForm({ title, config, onChange }) {
  return (
    <div className="configCard">
      <h4>{title}</h4>
      <div className="formGrid">
        <label>
          <span>Strategy</span>
          <select
            value={config.strategy}
            onChange={(event) => onChange((prev) => ({ ...prev, strategy: event.target.value }))}
          >
            {STRATEGY_OPTIONS.map((option) => (
              <option key={option.value} value={option.value}>
                {option.label}
              </option>
            ))}
          </select>
        </label>

        {FIELDS.map((field) => (
          <label key={field.key}>
            <span>{field.label}</span>
            <input
              type={field.type}
              step={field.step}
              value={config[field.key]}
              onChange={(event) => {
                const nextValue = event.target.value;
                onChange((prev) => ({
                  ...prev,
                  [field.key]: nextValue,
                }));
              }}
            />
          </label>
        ))}
      </div>
    </div>
  );
}

function RunControls({
  connectionStatus,
  isRunning,
  runMode,
  setRunMode,
  singleConfig,
  setSingleConfig,
  compareConfigA,
  setCompareConfigA,
  compareConfigB,
  setCompareConfigB,
  onRunSingle,
  onRunComparison,
  onStop,
}) {
  const connected = connectionStatus === 'connected';

  return (
    <div className="runControlsBody">
      <div className="panelHeaderRow">
        <h2>Run Controls</h2>
        <span>{connected ? 'Ready' : 'Disconnected'}</span>
      </div>

      <div className="modeSwitch" role="radiogroup" aria-label="run mode">
        <button
          type="button"
          className={runMode === 'single' ? 'active' : ''}
          onClick={() => setRunMode('single')}
        >
          Single Run
        </button>
        <button
          type="button"
          className={runMode === 'compare' ? 'active' : ''}
          onClick={() => setRunMode('compare')}
        >
          Compare A/B
        </button>
      </div>

      {runMode === 'single' ? (
        <ConfigForm title="Run Config" config={singleConfig} onChange={setSingleConfig} />
      ) : (
        <div className="compareForms">
          <ConfigForm title="Config A" config={compareConfigA} onChange={setCompareConfigA} />
          <ConfigForm title="Config B" config={compareConfigB} onChange={setCompareConfigB} />
        </div>
      )}

      <div className="controlsActions">
        {runMode === 'single' ? (
          <button
            type="button"
            className="primaryAction"
            onClick={onRunSingle}
            disabled={!connected || isRunning}
          >
            Start Run
          </button>
        ) : (
          <button
            type="button"
            className="primaryAction"
            onClick={onRunComparison}
            disabled={!connected || isRunning}
          >
            Start A/B
          </button>
        )}

        <button
          type="button"
          className="stopAction"
          onClick={onStop}
          disabled={!connected || !isRunning}
        >
          Stop
        </button>
      </div>
    </div>
  );
}

export default RunControls;

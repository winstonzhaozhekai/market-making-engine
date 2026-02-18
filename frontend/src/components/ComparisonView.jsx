import React from 'react';

const METRIC_ROWS = [
  { key: 'total_pnl', label: 'Total PnL', style: 'currency' },
  { key: 'realized_pnl', label: 'Realized PnL', style: 'currency' },
  { key: 'unrealized_pnl', label: 'Unrealized PnL', style: 'currency' },
  { key: 'drawdown', label: 'Drawdown', style: 'currency' },
  { key: 'throughput_eps', label: 'Throughput', style: 'number' },
  { key: 'total_fills', label: 'Total Fills', style: 'int' },
];

function formatMetric(value, style = 'number') {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) {
    return 'N/A';
  }

  if (style === 'currency') {
    return `$${numeric.toFixed(2)}`;
  }
  if (style === 'int') {
    return `${Math.round(numeric)}`;
  }
  return numeric.toFixed(2);
}

function deltaClass(delta) {
  if (!Number.isFinite(delta) || delta === 0) {
    return 'deltaNeutral';
  }
  return delta > 0 ? 'deltaPositive' : 'deltaNegative';
}

function ComparisonView({ runA, runB }) {
  const complete = runA && runB && runA.summary && runB.summary;

  return (
    <div className="comparisonBody">
      <div className="panelHeaderRow">
        <h2>Comparison</h2>
        <span>A/B parameter deltas</span>
      </div>

      {!complete ? (
        <div className="emptyState">
          <p>Run comparison by launching two queued configs in Compare A/B mode.</p>
        </div>
      ) : (
        <>
          <div className="comparisonRunMeta">
            <div>
              <h3>Run A</h3>
              <p>#{runA.runId} | {runA.summary.strategy} | seed {runA.config.seed}</p>
            </div>
            <div>
              <h3>Run B</h3>
              <p>#{runB.runId} | {runB.summary.strategy} | seed {runB.config.seed}</p>
            </div>
          </div>

          <div className="comparisonTable">
            <div className="comparisonHeader">Metric</div>
            <div className="comparisonHeader">Run A</div>
            <div className="comparisonHeader">Run B</div>
            <div className="comparisonHeader">Delta (B - A)</div>

            {METRIC_ROWS.map((row) => {
              const aValue = Number(runA.summary[row.key]);
              const bValue = Number(runB.summary[row.key]);
              const delta = bValue - aValue;

              return (
                <React.Fragment key={row.key}>
                  <div>{row.label}</div>
                  <div>{formatMetric(aValue, row.style)}</div>
                  <div>{formatMetric(bValue, row.style)}</div>
                  <div className={deltaClass(delta)}>{formatMetric(delta, row.style)}</div>
                </React.Fragment>
              );
            })}
          </div>
        </>
      )}
    </div>
  );
}

export default ComparisonView;

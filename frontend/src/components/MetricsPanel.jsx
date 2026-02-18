import React, { useEffect, useMemo, useRef } from 'react';
import Chart from 'chart.js/auto';

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

  return numeric.toFixed(4);
}

function SeriesChart({ title, points, series, valueStyle = 'number' }) {
  const canvasRef = useRef(null);
  const chartRef = useRef(null);

  const labels = useMemo(() => points.map((point) => point.iteration), [points]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return;
    }

    if (!chartRef.current) {
      chartRef.current = new Chart(canvas.getContext('2d'), {
        type: 'line',
        data: {
          labels,
          datasets: series.map((item) => ({
            label: item.label,
            data: points.map((point) => point[item.key]),
            borderColor: item.color,
            backgroundColor: item.color,
            pointRadius: 0,
            tension: 0.18,
            borderWidth: 2,
          })),
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: false,
          interaction: {
            mode: 'index',
            intersect: false,
          },
          plugins: {
            legend: {
              labels: {
                color: '#f4f4ef',
                boxWidth: 12,
              },
            },
            title: {
              display: true,
              text: title,
              color: '#f4f4ef',
              font: {
                family: 'IBM Plex Sans',
                size: 13,
                weight: '600',
              },
            },
            tooltip: {
              callbacks: {
                label: (context) => `${context.dataset.label}: ${formatMetric(context.parsed.y, valueStyle)}`,
              },
            },
          },
          scales: {
            x: {
              ticks: {
                color: '#bcbcab',
                maxTicksLimit: 8,
              },
              grid: {
                color: 'rgba(244, 244, 239, 0.08)',
              },
            },
            y: {
              ticks: {
                color: '#bcbcab',
                callback: (value) => formatMetric(value, valueStyle),
              },
              grid: {
                color: 'rgba(244, 244, 239, 0.08)',
              },
            },
          },
        },
      });
    }

    chartRef.current.data.labels = labels;
    chartRef.current.data.datasets = series.map((item) => ({
      label: item.label,
      data: points.map((point) => point[item.key]),
      borderColor: item.color,
      backgroundColor: item.color,
      pointRadius: 0,
      tension: 0.18,
      borderWidth: 2,
    }));
    chartRef.current.options.plugins.title.text = title;
    chartRef.current.update('none');
  }, [labels, points, series, title, valueStyle]);

  useEffect(() => {
    return () => {
      if (chartRef.current) {
        chartRef.current.destroy();
        chartRef.current = null;
      }
    };
  }, []);

  return (
    <div className="chartCard">
      <canvas ref={canvasRef} />
    </div>
  );
}

function MetricsPanel({ run }) {
  if (!run) {
    return (
      <div className="emptyState">
        <p>No run selected. Start a simulation to populate analytics charts.</p>
      </div>
    );
  }

  const summary = run.summary || {};

  return (
    <div className="metricsPanelBody">
      <div className="runSummary">
        <h3>Run #{run.runId} - {run.label}</h3>
        <span className={`statusPill status-${run.status}`}>{run.status}</span>
        <div className="summaryGrid">
          <div><strong>Strategy</strong><span>{summary.strategy || run.config.strategy}</span></div>
          <div><strong>Seed</strong><span>{run.config.seed}</span></div>
          <div><strong>Iterations</strong><span>{formatMetric(summary.total_iterations || run.config.iterations, 'int')}</span></div>
          <div><strong>Runtime</strong><span>{formatMetric(summary.total_runtime, 'number')} ms</span></div>
          <div><strong>Total PnL</strong><span>{formatMetric(summary.total_pnl, 'currency')}</span></div>
          <div><strong>Realized PnL</strong><span>{formatMetric(summary.realized_pnl, 'currency')}</span></div>
          <div><strong>Unrealized PnL</strong><span>{formatMetric(summary.unrealized_pnl, 'currency')}</span></div>
          <div><strong>Drawdown</strong><span>{formatMetric(summary.drawdown, 'currency')}</span></div>
          <div><strong>Throughput</strong><span>{formatMetric(summary.throughput_eps, 'number')} evt/s</span></div>
          <div><strong>Total Fills</strong><span>{formatMetric(summary.total_fills, 'int')}</span></div>
        </div>
      </div>

      <div className="chartsGrid">
        <SeriesChart
          title="Inventory Over Time"
          points={run.points}
          series={[{ key: 'inventory', label: 'Inventory', color: '#7dd3fc' }]}
          valueStyle="int"
        />
        <SeriesChart
          title="Spread Over Time"
          points={run.points}
          series={[{ key: 'spread', label: 'Spread', color: '#f59e0b' }]}
          valueStyle="number"
        />
        <SeriesChart
          title="PnL Over Time"
          points={run.points}
          series={[
            { key: 'realizedPnl', label: 'Realized', color: '#34d399' },
            { key: 'unrealizedPnl', label: 'Unrealized', color: '#a78bfa' },
            { key: 'totalPnl', label: 'Total', color: '#f97316' },
          ]}
          valueStyle="currency"
        />
        <SeriesChart
          title="Drawdown Over Time"
          points={run.points}
          series={[{ key: 'drawdown', label: 'Drawdown', color: '#f87171' }]}
          valueStyle="currency"
        />
      </div>
    </div>
  );
}

export default MetricsPanel;

import React, { useState, useEffect, useRef } from 'react';
import Chart from 'chart.js/auto';
import './App.css';

function App() {
  const [messages, setMessages] = useState([]);
  const [trades, setTrades] = useState([]);
  const [metrics, setMetrics] = useState(null);
  const chartRef = useRef(null);
  const chartInstance = useRef(null);
  const [ws, setWs] = useState(null);

  useEffect(() => {
    const websocket = new WebSocket('ws://localhost:8080');
    setWs(websocket);

    websocket.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        setMessages((prevMessages) => [...prevMessages, event.data]);

        if (data.trades && data.trades.length > 0) {
          setTrades((prevTrades) => [...prevTrades, ...data.trades.map((trade) => ({
            iteration: data.iteration,
            price: trade.price,
            size: trade.size,
            side: trade.side,
          }))]);
        }

        if (data.metrics) {
          setMetrics(data.metrics);
        }
      } catch (error) {
        console.error('Error parsing WebSocket message:', error, event.data);
      }
    };

    websocket.onerror = (error) => {
      console.error('WebSocket error:', error);
    };

    websocket.onclose = () => {
      console.log('WebSocket connection closed');
    };

    return () => websocket.close();
  }, []);

  useEffect(() => {
    if (chartRef.current && trades.length > 0) {
      const ctx = chartRef.current.getContext('2d');
      
      // Destroy existing chart if it exists
      if (chartInstance.current) {
        chartInstance.current.destroy();
      }

      // Separate buy and sell trades
      const buyTrades = trades.filter(trade => trade.side === 'BUY');
      const sellTrades = trades.filter(trade => trade.side === 'SELL');

      // Create datasets for both buy and sell trades
      const datasets = [];
      
      if (buyTrades.length > 0) {
        datasets.push({
          label: 'Buy Trades',
          data: buyTrades.map(trade => ({ x: trade.iteration, y: trade.price })),
          backgroundColor: 'rgba(34, 197, 94, 0.8)',
          borderColor: 'rgba(34, 197, 94, 1)',
          borderWidth: 2,
          pointRadius: 6,
          pointHoverRadius: 8,
          showLine: false,
        });
      }

      if (sellTrades.length > 0) {
        datasets.push({
          label: 'Sell Trades',
          data: sellTrades.map(trade => ({ x: trade.iteration, y: trade.price })),
          backgroundColor: 'rgba(239, 68, 68, 0.8)',
          borderColor: 'rgba(239, 68, 68, 1)',
          borderWidth: 2,
          pointRadius: 6,
          pointHoverRadius: 8,
          showLine: false,
        });
      }

      chartInstance.current = new Chart(ctx, {
        type: 'scatter',
        data: {
          datasets: datasets,
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: {
            title: {
              display: true,
              text: 'Market Making Trades',
              font: {
                size: 16,
                weight: 'bold',
              },
            },
            legend: {
              display: true,
              position: 'top',
            },
            tooltip: {
              callbacks: {
                label: function(context) {
                  const trade = trades.find(t => t.iteration === context.parsed.x && t.price === context.parsed.y);
                  return `${context.dataset.label}: $${context.parsed.y.toFixed(2)} (Size: ${trade?.size || 'N/A'})`;
                },
              },
            },
          },
          scales: {
            x: {
              type: 'linear',
              position: 'bottom',
              title: {
                display: true,
                text: 'Iteration',
                font: {
                  size: 14,
                  weight: 'bold',
                },
              },
              grid: {
                display: true,
                color: 'rgba(0, 0, 0, 0.1)',
              },
            },
            y: {
              title: {
                display: true,
                text: 'Price ($)',
                font: {
                  size: 14,
                  weight: 'bold',
                },
              },
              grid: {
                display: true,
                color: 'rgba(0, 0, 0, 0.1)',
              },
              ticks: {
                callback: function(value) {
                  return '$' + value.toFixed(2);
                },
              },
            },
          },
          animation: {
            duration: 200,
          },
        },
      });
    }
  }, [trades]);

  const startSimulation = () => {
    if (ws) {
      // Clear existing data
      setMessages([]);
      setTrades([]);
      setMetrics(null);
      ws.send('run_simulation');
    }
  };

  return (
    <div className="App">
      <header className="App-header">
        <div style={{ display: 'flex', flexDirection: 'row', width: '100%' }}>
          <div className="WebSocketLogger">
            <h2>WebSocket Logger</h2>
            <button onClick={startSimulation}>Start Simulation</button>
            <div className="messages">
              {messages.map((message, index) => (
                <p key={index}>{message}</p>
              ))}
            </div>
            <div className="TradeStatistics">
              <h3>Trade Statistics</h3>
              <p>Total Trades: {trades.length}</p>
              <p>Buy Trades: {trades.filter(t => t.side === 'BUY').length}</p>
              <p>Sell Trades: {trades.filter(t => t.side === 'SELL').length}</p>
              {trades.length > 0 && (
                <>
                  <p>Price Range: ${Math.min(...trades.map(t => t.price)).toFixed(2)} - ${Math.max(...trades.map(t => t.price)).toFixed(2)}</p>
                  <p>Average Price: ${(trades.reduce((sum, t) => sum + t.price, 0) / trades.length).toFixed(2)}</p>
                </>
              )}
            </div>
          </div>
          <div className="ChartContainer">
            <div style={{ height: '500px', position: 'relative' }}>
              <canvas ref={chartRef}></canvas>
            </div>
            {metrics && (
              <div className="SimulationMetrics">
                <h3>Simulation Metrics</h3>
                <div>
                  <p>Total Iterations: {metrics.total_iterations}</p>
                  <p>Total Runtime: {metrics.total_runtime}ms</p>
                  <p>Average Iteration Time: {metrics.average_iteration_time}ms</p>
                  <p>Inventory: {metrics.inventory} shares</p>
                  <p>Cash: ${metrics.cash.toFixed(2)}</p>
                  <p>Mark Price: ${metrics.mark_price.toFixed(2)}</p>
                  <p>Unrealized PnL: ${metrics.unrealized_pnl.toFixed(2)}</p>
                  <p>Total PnL: ${metrics.total_pnl.toFixed(2)}</p>
                  <p>Total Slippage: ${metrics.total_slippage.toFixed(2)}</p>
                  <p>Missed Opportunities: {metrics.missed_opportunities}</p>
                  <p>Inventory Skew: {metrics.inventory_skew.toFixed(2)}</p>
                </div>
              </div>
            )}
          </div>
        </div>
      </header>
    </div>
  );
}

export default App;

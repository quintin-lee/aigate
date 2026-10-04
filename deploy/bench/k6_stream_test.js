import http from 'k6/http';
import { check, sleep } from 'k6';
import { Trend, Counter, Rate } from 'k6/metrics';

// Custom metrics for LLM Streaming
const ttftTrend = new Trend('llm_ttft_ms');
const totalDurationTrend = new Trend('llm_total_duration_ms');
const streamChunkCounter = new Counter('llm_chunks_received');
const streamSuccessRate = new Rate('llm_stream_success_rate');
const rateLimitCounter = new Counter('llm_rate_limited_429');

const SCALE = parseFloat(__ENV.SCALE || '1');      // time scale: 0.2 => 5x shorter
const PEAK = parseInt(__ENV.PEAK_VUS || '100', 10); // peak virtual users
const d = (s) => `${Math.max(1, Math.round(s * SCALE))}s`;

export const options = {
  scenarios: {
    chat_stream_load: {
      executor: 'ramping-vus',
      startVUs: 1,
      stages: [
        { duration: d(30), target: Math.round(PEAK * 0.2) }, // Ramp up
        { duration: d(60), target: Math.round(PEAK * 0.5) }, // Steady state
        { duration: d(30), target: PEAK },                   // Peak burst
        { duration: d(30), target: 0 },                      // Ramp down
      ],
      gracefulRampDown: '10s',
    },
  },
  thresholds: {
    'llm_ttft_ms': ['p(95)<800'],           // 95% of TTFT should be under 800ms
    'llm_total_duration_ms': ['p(95)<8000'], // 95% total stream duration < 8s
    'llm_stream_success_rate': ['rate>0.98'], // 98%+ success rate
    'http_req_failed': ['rate<0.02'],       // <2% HTTP errors
  },
};

const BASE_URL = __ENV.TARGET_URL || 'http://localhost:8080';
const API_KEY = __ENV.API_KEY || 'sk-test-client-key';
const MODEL = __ENV.MODEL || 'gpt-4o';

export default function () {
  const payload = JSON.stringify({
    model: MODEL,
    messages: [
      { role: 'system', content: 'You are a concise, helpful assistant.' },
      { role: 'user', content: `Explain the benefits of streaming Server-Sent Events in 3 short bullet points. [req ${__VU}-${__ITER}-${Date.now()}]` }
    ],
    temperature: 0.7,
    max_tokens: 150,
    stream: true,
  });

  const params = {
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${API_KEY}`,
      'Accept': 'text/event-stream',
    },
    timeout: '60s',
  };

  const startTime = Date.now();
  const res = http.post(`${BASE_URL}/v1/chat/completions`, payload, params);

  if (res.status === 429) {
    rateLimitCounter.add(1);
    streamSuccessRate.add(0);
    sleep(1);
    return;
  }

  const success = check(res, {
    'status is 200': (r) => r.status === 200,
    'content-type is event-stream': (r) => r.headers['Content-Type'] && r.headers['Content-Type'].includes('text/event-stream'),
    'contains data chunks': (r) => r.body && r.body.includes('data:'),
    'contains [DONE] marker': (r) => r.body && r.body.includes('data: [DONE]'),
  });

  streamSuccessRate.add(success ? 1 : 0);

  if (res.status === 200 && res.body) {
    const totalDuration = Date.now() - startTime;
    totalDurationTrend.add(totalDuration);

    // Parse SSE lines to approximate TTFT from server timestamps or chunk structure
    const lines = res.body.split('\n');
    let chunkCount = 0;
    for (let i = 0; i < lines.length; i++) {
      if (lines[i].startsWith('data:') && !lines[i].includes('[DONE]')) {
        chunkCount++;
      }
    }
    streamChunkCounter.add(chunkCount);

    // If client timing is available:
    if (res.timings && res.timings.waiting) {
      ttftTrend.add(res.timings.waiting);
    }
  }

  // Realistic user pacing between queries
  sleep(Math.random() * 2 + 1);
}

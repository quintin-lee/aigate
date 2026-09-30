import http from 'k6/http';
import { check } from 'k6';
import { Trend, Rate, Counter } from 'k6/metrics';
import { parseSSELines } from './lib/sse_parser.js';

// Custom metrics
export const ttftTrend = new Trend('aigate_ttft_ms');
export const cacheHitRate = new Rate('aigate_cache_hit_rate');
export const failoverSuccessRate = new Rate('aigate_failover_success_rate');
export const reqThroughput = new Counter('aigate_completed_requests');

const TARGET_URL = __ENV.TARGET_URL || 'http://127.0.0.1:18080/v1/chat/completions';
const API_KEY = __ENV.API_KEY || 'aig_benchmark_test_key';
const SCENARIO = __ENV.SCENARIO || 'sync';
const VUS = parseInt(__ENV.VUS || '20', 10);
const DURATION = __ENV.DURATION || '10s';

// Load fixture payloads
const syncPayload = open('../config/fixtures/chat_sync.json');
const streamPayload = open('../config/fixtures/chat_stream.json');
const cachePayload = open('../config/fixtures/chat_cache.json');

export const options = {
  scenarios: {
    benchmark_scenario: {
      executor: 'constant-vus',
      vus: VUS,
      duration: DURATION,
    },
  },
  thresholds: {
    http_req_failed: ['rate<0.01'],
  },
};

const headers = {
  'Content-Type': 'application/json',
  'Authorization': `Bearer ${API_KEY}`,
};

export default function () {
  if (SCENARIO === 'sync') {
    const res = http.post(TARGET_URL, syncPayload, { headers });
    check(res, {
      'status is 200': (r) => r.status === 200,
    });
    reqThroughput.add(1);

  } else if (SCENARIO === 'stream') {
    const startTime = Date.now();
    const res = http.post(TARGET_URL, streamPayload, {
      headers: Object.assign({}, headers, { 'Accept': 'text/event-stream' }),
      responseType: 'text',
    });

    const is200 = check(res, { 'status is 200': (r) => r.status === 200 });
    if (is200 && res.body) {
      const events = parseSSELines(res.body);
      for (const ev of events) {
        if (ev.data && ev.data !== '[DONE]') {
          try {
            const parsed = JSON.parse(ev.data);
            if (parsed.choices && parsed.choices[0] && parsed.choices[0].delta) {
              const ttft = Date.now() - startTime;
              ttftTrend.add(ttft);
              break;
            }
          } catch (e) {
            // ignore malformed JSON chunk
          }
        }
      }
    }
    reqThroughput.add(1);

  } else if (SCENARIO === 'cache') {
    const res = http.post(TARGET_URL, cachePayload, { headers });
    const is200 = check(res, { 'status is 200': (r) => r.status === 200 });
    if (is200) {
      const isHit = res.headers['X-Cache'] === 'HIT';
      cacheHitRate.add(isHit);
    }
    reqThroughput.add(1);

  } else if (SCENARIO === 'failover') {
    // Model bench-failover maps to primary (503) -> secondary (200)
    const payload = JSON.stringify({
      model: 'bench-failover',
      messages: [{ role: 'user', content: 'failover test' }]
    });
    const res = http.post(TARGET_URL, payload, { headers });
    const is200 = check(res, {
      'status is 200': (r) => r.status === 200,
      'switched to secondary': (r) => r.headers['X-Upstream-Provider'] === 'mock-secondary',
    });
    failoverSuccessRate.add(is200);
    reqThroughput.add(1);
  }
}

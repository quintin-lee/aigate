-- wrk script for testing response cache HIT QPS
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Authorization"] = "Bearer " .. (os.getenv("API_KEY") or "aig_benchmark_test_key")

local file = io.open("benchmarks/config/fixtures/chat_cache.json", "r")
if file then
    wrk.body = file:read("*all")
    file:close()
else
    wrk.body = '{"model":"bench-cache","messages":[{"role":"user","content":"Fixed prompt for deterministic cache hit"}]}'
end

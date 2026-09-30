-- wrk script for streaming SSE chat completions
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Accept"] = "text/event-stream"
wrk.headers["Authorization"] = "Bearer " .. (os.getenv("API_KEY") or "aig_benchmark_test_key")

local file = io.open("benchmarks/config/fixtures/chat_stream.json", "r")
if file then
    wrk.body = file:read("*all")
    file:close()
else
    wrk.body = '{"model":"bench-stream","stream":true,"messages":[{"role":"user","content":"ping"}]}'
end

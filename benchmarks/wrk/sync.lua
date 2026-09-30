-- wrk script for non-streaming chat completions
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Authorization"] = "Bearer " .. (os.getenv("API_KEY") or "aig_benchmark_test_key")

local file = io.open("benchmarks/config/fixtures/chat_sync.json", "r")
if file then
    wrk.body = file:read("*all")
    file:close()
else
    wrk.body = '{"model":"bench-sync","messages":[{"role":"user","content":"ping"}]}'
end

response = function(status, headers, body)
    if status ~= 200 then
        -- Print unexpected status for diagnostic
    end
end

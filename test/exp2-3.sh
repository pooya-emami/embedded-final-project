#!/bin/bash

echo "========================================="
echo "Experiment 2-3: 50 Concurrent API Requests"
echo "Student: Pooya Emami (404300409)"
echo "========================================="

URL="https://localhost:8443/api/v1/telemetry"
DURATION=30
CONCURRENT=50
INTERVAL=5

echo ""
echo "Sending $CONCURRENT concurrent requests for $DURATION seconds..."
echo ""

START=$(date +%s.%N)
END_TIME=$((SECONDS + DURATION))

while [ $SECONDS -lt $END_TIME ]; do
    for i in $(seq 1 $CONCURRENT); do
        (
            curl -s -k $URL > /dev/null 2>&1
        ) &
    done
    
    if [ $((SECONDS % INTERVAL)) -eq 0 ]; then
        curl -s -k -w "Request at ${SECONDS}s: %{time_total} seconds\n" $URL > /dev/null
    fi
    
    sleep 0.01
done

wait

ELAPSED=$(echo "$(date +%s.%N) - $START" | bc)

echo ""
echo "All $CONCURRENT concurrent requests completed!"
echo "Total time: ${ELAPSED} seconds"
echo ""

# Sample response
echo "Sample response:"
curl -s -k $URL | python3 -m json.tool

echo ""
echo "========================================="
echo "Experiment 2-3 Complete!"
echo "========================================="
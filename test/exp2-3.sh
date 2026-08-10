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
echo "Each process sends one request every $INTERVAL seconds"
echo ""

START=$(date +%s.%N)
END_TIME=$((SECONDS + DURATION))

for i in $(seq 1 $CONCURRENT); do
    (
        while [ $SECONDS -lt $END_TIME ]; do
            curl -s -k -w "Process $i: %{time_total} seconds\n" $URL > /dev/null
            sleep $INTERVAL
        done
    ) &
done

wait

ELAPSED=$(echo "$(date +%s.%N) - $START" | bc)

echo ""
echo "All requests completed!"
echo "Total time: ${ELAPSED} seconds"
echo ""

# Sample response
echo "Sample response:"
curl -s -k $URL | python3 -m json.tool

echo ""
echo "========================================="
echo "Experiment 2-3 Complete!"
echo "========================================="
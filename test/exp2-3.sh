#!/bin/bash

echo "========================================="
echo "Experiment 2-3: 50 Concurrent API Requests"
echo "Student: Pooya Emami (404300409)"
echo "========================================="

URL="https://localhost:8443/api/v1/telemetry"
DURATION=30
CONCURRENT=50

echo ""
echo "Sending $CONCURRENT concurrent requests for $DURATION seconds..."
echo ""

START=$(date +%s.%N)
END_TIME=$((SECONDS + DURATION))

for i in $(seq 1 $CONCURRENT); do
    (
        while [ $SECONDS -lt $END_TIME ]; do
            curl -s -k $URL > /dev/null 2>&1
            sleep 0.01
        done
    ) &
done

# Wait for all to complete
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
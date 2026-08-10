#!/bin/bash

echo "========================================="
echo "Experiment 2-3: API Response Time"
echo "Student: Pooya Emami (404300409)"
echo "========================================="

URL="https://localhost:8443/api/v1/telemetry"
DURATION=30
INTERVAL=5

echo ""
echo "Sending 1 request every $INTERVAL seconds for $DURATION seconds..."
echo ""

echo "Time     | Response Time (seconds)"
echo "---------|----------------------"

for i in $(seq 1 $((DURATION / INTERVAL))); do
    START=$(date +%s.%N)
    curl -s -k $URL > /dev/null 2>&1
    END=$(date +%s.%N)
    ELAPSED=$(echo "$END - $START" | bc)
    echo "$(date +%H:%M:%S) | $(printf "%.4f" $ELAPSED) s"
    sleep $INTERVAL
done

echo ""
echo "========================================="
echo "Experiment 2-3 Complete!"
echo "========================================="
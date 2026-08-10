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

# Start 50 concurrent curl processes
for i in $(seq 1 $CONCURRENT); do
    (
        while [ $SECONDS -lt $END_TIME ]; do
            curl -s -k $URL > /dev/null 2>&1
        done
    ) &
done

# Sample curl every 5 seconds
echo ""
echo "Sample Response Times:"
echo "Time     | Response Time"
echo "---------|--------------"
for i in $(seq 1 $((DURATION / INTERVAL))); do
    sleep $INTERVAL
    
    START_SAMPLE=$(date +%s.%N)
    curl -s -k $URL > /dev/null 2>&1
    END_SAMPLE=$(date +%s.%N)
    ELAPSED=$(echo "$END_SAMPLE - $START_SAMPLE" | bc)
    
    echo "$(date +%H:%M:%S) | $(printf "%.4f" $ELAPSED) s"
done

wait

ELAPSED=$(echo "$(date +%s.%N) - $START" | bc)

echo ""
echo "All $CONCURRENT concurrent requests completed!"
echo "Total time: ${ELAPSED} seconds"
echo ""
echo "========================================="
echo "Experiment 2-3 Complete!"
echo "========================================="
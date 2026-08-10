#!/bin/bash

echo "========================================="
echo "Experiment 2-3: 50 Concurrent API Requests"
echo "Student: Pooya Emami (404300409)"
echo "========================================="

URL="https://localhost:8443/api/v1/telemetry"

echo ""
echo "Sending 50 concurrent requests to $URL"
echo "This may take a few seconds..."
echo ""

# Measure time
START=$(date +%s.%N)

# Send 50 concurrent requests
for i in {1..50}; do
    curl -s -k $URL > /dev/null &
done

# Wait for all to complete
wait

END=$(date +%s.%N)
ELAPSED=$(echo "$END - $START" | bc)

echo "All 50 requests completed!"
echo "Total time: ${ELAPSED} seconds"
echo "Average time per request: $(echo "$ELAPSED / 50" | bc -l) seconds"
echo ""

# Check response quality
echo "Sample response:"
curl -s -k $URL | python3 -m json.tool

echo ""
echo "========================================="
echo "Experiment 2-3 Complete!"
echo "=========================================" 

#!/bin/bash

echo "========================================="
echo "Experiment 2-1: Temperature Monitoring"
echo "Student: Pooya Emami (404300409)"
echo "========================================="

MODES=("idle" "raw" "processed")
MODE_NAMES=("Idle (No Stream)" "Raw Stream Only" "Stream + Detection")

for i in "${!MODES[@]}"; do
    echo ""
    echo "========================================="
    echo "Mode: ${MODE_NAMES[$i]}"
    echo "========================================="
    
    # Set mode
    curl -s -X POST https://localhost:8443/api/v1/stream_mode -k \
        -H "Content-Type: application/json" \
        -d "{\"mode\":\"${MODES[$i]}\"}" > /dev/null
    
    # Record temperature every 30 seconds for 5 minutes (10 samples)
    echo "Recording temperature every 30 seconds for 5 minutes..."
    for j in {1..10}; do
        TEMP=$(curl -s -k https://localhost:8443/api/v1/telemetry | python3 -c "import sys, json; print(json.load(sys.stdin).get('temp', 'N/A'))")
        echo "$(date +%H:%M:%S) - Temp: ${TEMP}°C"
        sleep 30
    done
done

echo ""
echo "========================================="
echo "Experiment 2-1 Complete!"
echo "=========================================" 

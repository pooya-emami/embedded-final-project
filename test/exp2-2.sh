 #!/bin/bash

echo "========================================="
echo "Experiment 2-2: Memory Usage Monitoring"
echo "Student: Pooya Emami (404300409)"
echo "========================================="

# Get server PID
PID=$(pgrep -f "security_server|server" | head -1)

if [ -z "$PID" ]; then
    echo "Error: Server not running!"
    exit 1
fi

echo "Server PID: $PID"
echo "Recording memory usage every 30 seconds for 5 minutes..."
echo ""

echo "Time      | RSS (MB) | VM (MB) | %MEM"
echo "----------|----------|---------|-----"

for i in {1..10}; do
    MEM=$(ps -o rss,vsz,pmem -p $PID 2>/dev/null | tail -1)
    RSS=$(echo $MEM | awk '{print $1/1024}')
    VSZ=$(echo $MEM | awk '{print $2/1024}')
    PMEM=$(echo $MEM | awk '{print $3}')
    echo "$(date +%H:%M:%S) | $(printf "%8.2f" $RSS) | $(printf "%7.2f" $VSZ) | $(printf "%4.1f" $PMEM)%"
    sleep 30
done

echo ""
echo "========================================="
echo "Experiment 2-2 Complete!"
echo "========================================="

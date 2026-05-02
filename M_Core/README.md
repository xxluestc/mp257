## <b>OpenAMP_TTY_echo Application Description</b>

-  It demonstrates How to use OpenAMP MW + Virtual UART to create an Inter-Processor Communication channel seen as TTY device in Linux OS.
-  This project deals with CPU2 (Cortex-M33) firmware and requires Linux OS running on CPU1 (Cortex-A35).

### <b>Keywords</b>

OpenAMP, RPMsg, TFM, IPCC, Inter-Processor Communication

### <b>How to use it ?</b>

- Use the following script:

######################### shell start ########################################################

#!/bin/bash

# Start the M33&A35 virtual channel, hiding terminal messages
echo "Opening M33&A35 Virtual channel..."
./fw_cortex_m33.sh start

# Configure Virtual UART0
echo "Configuring Virtual UART0..."
stty -onlcr -echo -F /dev/ttyRPMSG0

# Start reading UART0 data in the background
echo "Starting to read from Virtual UART0..."
cat /dev/ttyRPMSG0 &
CAT_PID=$!  # Save the background process ID

# Define the number of iterations and interval
NUM_ITERATIONS=5
INTERVAL=2

echo ""

sleep $INTERVAL

# Loop to send data
for ((i=1; i<=NUM_ITERATIONS; i++)); do
    echo "Sending 'Hello alientek!' to CPU M33 (Iteration $i)..."
    echo "Hello alientek!" >/dev/ttyRPMSG0
    sleep $INTERVAL
done

# Clean up the background process
echo "Stopping background process..."
kill $CAT_PID

# Stop the virtual channel, hiding terminal messages
echo ""
echo "Stopping M33&A35 Virtual channel..."
./fw_cortex_m33.sh stop

echo "Script completed."

######################### shell end ##########################################################
 
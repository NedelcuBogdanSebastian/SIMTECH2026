import serial
import time

# Debug mode: 0 = only decoded data with response time
debug = 0  

ser = serial.Serial("COM10", 19200, timeout=0.1)

# List of TX strings to send
TX_STRINGS = ['/DA2\\']

# Counter
counter = 0

while True:
    # Select TX string
    TX_STRING = TX_STRINGS[counter % len(TX_STRINGS)]

    # Record send time
    send_time = int(time.time() * 1000)

    # Send request
    ser.write(bytearray(TX_STRING, 'ascii'))

    # Read response
    response = ser.readline().strip()

    if response:
        try:
            # Record receive time
            receive_time = int(time.time() * 1000)
            elapsed = receive_time - send_time  # response time in ms

            # Decode response
            decoded_response = response.decode('utf-8')
            module_type = decoded_response[1:3]   # DA, IA, TA
            module_number = decoded_response[3]   # e.g., "2"
            pay_load = decoded_response[5:-4]     # payload
            CRC16 = decoded_response[-4:]         # CRC

            if module_type == "TA":
                T1 = int(pay_load[:2], 16)
                T2 = int(pay_load[2:4], 16)
                T3 = int(pay_load[4:6], 16)
                T4 = int(pay_load[6:], 16)
                print(f"Module {module_type}{module_number}, Decoded: {T1-30}, {T2-30}, {T3-30}, {T4-30}, Time: {elapsed} ms")

            elif module_type == "IA":
                I1 = int(pay_load[:3], 16)
                I2 = int(pay_load[3:6], 16)
                I3 = int(pay_load[6:9], 16)
                I4 = int(pay_load[9:12], 16)
                I5 = int(pay_load[12:15], 16)
                I6 = int(pay_load[15:18], 16)
                print(f"Module {module_type}{module_number}, Decoded: {I1}, {I2}, {I3}, {I4}, {I5}, {I6}, Time: {elapsed} ms")

            elif module_type == "DA":
                A = int(pay_load[0], 16)
                B = int(pay_load[1], 16)
                C = int(pay_load[2], 16)
                PULSE_COUNTER = pay_load[3:7]
                print(f"Module {module_type}{module_number}, Decoded: {bin(A)[2:].zfill(4)} {bin(B)[2:].zfill(4)} {bin(C)[2:].zfill(4)} {int(PULSE_COUNTER, 16)}, Time: {elapsed} ms")

        except Exception as e:
            print(f"Decode --- Error: {e}")

    else:
        print("Response --- ERROR")

    # Increment counter
    counter += 1

    #time.sleep(0.001)

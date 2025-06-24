import serial
import time
import threading

def sender(ser): 
    try:
        while True:
            msg = input("Send to ESP32: ")
            ser.write((msg + '\n').encode('ascii'))
    except KeyboardInterrupt:
        pass

def receiver(ser):
    try:
        while True:
            if ser.in_waiting:
                response = ser.readline().decode('ascii').strip()
                print("Received from ESP32: ", response)
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass

def main():
    ser = serial.Serial("/dev/ttyAMA0", 115200)
    print("Connected!")

    send_thread = threading.Thread(target=sender, args=(ser,), daemon=True)
    recv_thread = threading.Thread(target=receiver, args=(ser,), daemon=True)

    send_thread.start()
    recv_thread.start()


    try:
        while send_thread.is_alive() and recv_thread.is_alive():
            time.sleep(0.1)
    
    except KeyboardInterrupt:
        print("\nExiting.")

    finally:
        ser.close()

if __name__ == "__main__":
    main()
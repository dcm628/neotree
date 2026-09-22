import serial
import time
import struct

# Global variable to store the serial connection
ser = None

def open_neotree_serial(port='/dev/ttyUSB0', baudrate=115200, timeout=1):
    """
    Opens the serial port to communicate with the microcontroller.
    
    :param port: The serial port (e.g., '/dev/ttyUSB0' or '/dev/ttyAMA0')
    :param baudrate: The baud rate for the serial communication (default: 9600)
    :param timeout: The timeout for read operations (default: 1 second)
    :return: None if successful, or an error message if failed.
    """
    global ser
    
    try:
        # Open the serial port with the given parameters
        ser = serial.Serial(port, baudrate, timeout=timeout)
        
        # Wait for the connection to establish (if necessary)
        time.sleep(.25)  # Optional: Give time for the connection to stabilize
        print(f"Successfully connected to {port} at {baudrate} baud.")
        return None  # Success
    
    except serial.SerialException as e:
        return f"Error: Unable to open serial port {port}. {str(e)}"

def write_neotree_serial(data):
    """
    Sends data to the microcontroller over the serial port.
    
    :param data: The data to be written to the serial port (string or bytes).
    :return: None if successful, or an error message if failed.
    """
    global ser
    
    if ser is None or not ser.is_open:
        return "Error: Serial port is not open. Please open the serial port first."

    try:
        # Write data to the serial port (ensure it's in bytes)
        if isinstance(data, str):
            data = data.encode('utf-8')  # Convert string to bytes if necessary
        
        ser.write(data)  # Write the data
        print(f"Data sent: {data}")
        return None  # Success

    except serial.SerialException as e:
        return f"Error: Failed to write data to serial port. {str(e)}"

def write_tree_single_led(ser, msg_type, led_position, r, g, b):
    """
    Packs the provided data into a single byte string and writes it to the serial port.
    
    :param ser: The serial object (opened with pyserial) for sending data.
    :param msg_type: The message type (uint8_t).
    :param led_position: The LED position (uint16_t).
    :param r: The red value (uint8_t).
    :param g: The green value (uint8_t).
    :param b: The blue value (uint8_t).
    :return: None
    """
    # Pack the data into a single byte string in little-endian format
    packed_data = struct.pack('<BHBBB', msg_type, led_position, r, g, b)
    # Write the packed data to the serial port
    ser.write(packed_data)
    print(f"Sent data: {packed_data}")
MAX_GROUP_UPDATE_ENTRIES = 50  # must match max_group_update_entries in firmware/include/dcm_rgb.hpp

def write_tree_group_leds(ser, msg_type, led_updates):
    """
    Update several LEDs (each its own position and color) in a single serial
    message (COLOR_GROUP_RGB_UPDATE) instead of one round-trip per LED.

    :param ser: The serial object (opened with pyserial) for sending data.
    :param msg_type: The message type (uint8_t) - COLOR_GROUP_RGB_UPDATE is 2.
    :param led_updates: List of (led_position, r, g, b) tuples, at most
        MAX_GROUP_UPDATE_ENTRIES long (the firmware's serial receive buffer
        is fixed-size and has to hold the whole message in one read).
    :return: None
    """
    count = len(led_updates)
    if count > MAX_GROUP_UPDATE_ENTRIES:
        raise ValueError(
            f"write_tree_group_leds: {count} entries exceeds the firmware's "
            f"MAX_GROUP_UPDATE_ENTRIES ({MAX_GROUP_UPDATE_ENTRIES}) - split into "
            f"multiple calls."
        )
    packed_data = struct.pack('<BB', msg_type, count)
    for led_position, r, g, b in led_updates:
        packed_data += struct.pack('<HBBB', led_position, r, g, b)
    ser.write(packed_data)
    print(f"Sent group update: {count} LEDs ({len(packed_data)} bytes)")

def write_tree_all_led(ser, msg_type, r, g, b):
    """
    Packs the provided data into a single byte string and writes it to the serial port.
    
    :param ser: The serial object (opened with pyserial) for sending data.
    :param msg_type: The message type (uint8_t).
    :param led_position: The LED position (uint16_t).
    :param r: The red value (uint8_t).
    :param g: The green value (uint8_t).
    :param b: The blue value (uint8_t).
    :return: None
    """
    # Pack the data into a single byte string in little-endian format
    packed_data = struct.pack('<BBBB', msg_type, r, g, b)
    # Write the packed data to the serial port
    ser.write(packed_data)
    print(f"Sent data: {packed_data}")
    
def cleanup_serial():
    """
    Closes the serial port connection if it is open.
    This should be called at the end of the script to release resources.
    """
    global ser
    if ser and ser.is_open:
        try:
            ser.close()  # Close the serial port
            print("Serial port closed successfully.")
        except serial.SerialException as e:
            print(f"Error while closing serial port: {str(e)}")
    else:
        print("No open serial port to close.")
        
        
def generate_color(wheel_position):
    """Generate an RGB color based on the position in the color wheel (0-255)."""
    wheel_position %= 255  # Ensure the position wraps around between 0 and 255
    
    if wheel_position < 85:
        # Transition from Red to Green
        r = 255 - wheel_position * 3
        g = wheel_position * 3
        b = 0
    elif wheel_position < 170:
        # Transition from Green to Blue
        wheel_position -= 85
        r = 0
        g = 255 - wheel_position * 3
        b = wheel_position * 3
    else:
        # Transition from Blue to Red
        wheel_position -= 170
        r = wheel_position * 3
        g = 0
        b = 255 - wheel_position * 3

    return r, g, b

def invert_rgb(r, g, b):
    r = 255 - r
    g = 255 - g
    b = 255 - b
    
    return r, g, b
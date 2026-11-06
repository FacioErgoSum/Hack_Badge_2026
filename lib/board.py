"""Miscellaneous Billboard Badge hardware"""

import machine

STATUS_LED_PIN = 1
BATT_V_SNS_PIN = 14

# Test points
TP8_GPIO9  = 9
TP9_GPIO41 = 41

# The LED is wired up to be active when the pin is pulled low, so
# this signal is inverted so setting it to True turns the LED on.
status_led = machine.Signal(machine.Pin(STATUS_LED_PIN, machine.Pin.OUT, value=True), invert=True)

# This is connected to the output of a 1/2 voltage divider from
# the input to the 3.3V regulator. That is either driven from
# USB or the battery.
# batt_voltage = board.batt_v_sns.
reg_in_v_sns = machine.ADC(machine.Pin(BATT_V_SNS_PIN), 
                           atten=machine.ADC.ATTN_11DB)

# UART exposed to test pads on back of badge
# TODO: UART(0) is invalid. UART(1) interferes with the E-Paper Display
# uart = machine.UART()


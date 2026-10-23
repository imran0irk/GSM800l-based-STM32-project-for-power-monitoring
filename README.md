# STM32F103 GSM Pump Controller with AC Power Detection

An automated, GSM-based agricultural pump controller built on the STM32F103C8T6 (Blue Pill / custom board) and SIM800L module. The system detects mains AC line presence, controls a pump relay via authorized missed calls and SMS commands, and sends immediate alert notifications during power outages and restorations. It features an onboard hardware button to dynamically register an administrator phone number through an incoming call.

---<img width="2799" height="2421" alt="924be7ec361d73a4487841c082da2465" src="https://github.com/user-attachments/assets/087fb167-40a6-429c-8edc-c1661bf2a345" />
<img width="660" height="713" alt="f209e19e-16fe-4845-ba2e-c18a34bb8bfb" src="https://github.com/user-attachments/assets/63d80cc8-55a8-462d-8b05-8328c53bb646" />



## Features

* **Missed Call Pump Toggle:** Toggles the pump relay on or off when receiving a missed call from the registered admin number (only allowed if AC power is present).


* **Mains AC Power Monitoring:** Samples AC power state through an isolated optocoupler circuit with debounce filtering.


* **Automated Power Alerts:** Dispatches SMS alerts and places a missed call to the admin upon power failure (load shedding) and power restoration.


* **Safety Lockout:** Automatically disables and locks out the pump relay when AC power failure is detected.


* **One-Touch Admin Learn Mode:** Enters phone-number programming mode via a hardware push button, saving the next incoming caller's number directly into non-volatile memory.


* **SMS Remote Control & Status:** Supports direct SMS commands (`ON`, `OFF`, `STATUS`, `SET`) with authorized number checks.


* **Non-Volatile Storage:** Persists the administrator's contact number in emulated EEPROM.



---

## Hardware Pinout & Wiring

### Microcontroller Pin Mapping

| Peripheral / Signal | STM32 Pin | Mode | Description |
| --- | --- | --- | --- |
| **AC Power Sense** | `PA0`<br> | Analog Input (`ADC`)

 | Optocoupler bridge rectifier output for 220V sensing

 |
| **Pump Relay Out** | `PB12`<br> | Digital Output | Drives NPN transistor / relay coil

 |
| **Admin Set Button** | `PB11`<br> | `INPUT_PULLUP`<br> | Momentary tactile push button to Ground

 |
| **Admin Set LED** | `PB13`<br> | Digital Output | Active HIGH when admin learn mode is active

 |
| **AC Power LED** | `PB0`<br> | Digital Output | Active HIGH indicator for active mains AC power

 |
| **Pump Active LED** | `PB1`<br> | Digital Output | Active HIGH indicator for pump relay state

 |
| **SIM800L TX** | `PA3` (`USART2_RX`)

 | Serial Input | Connected directly to SIM800L TX

 |
| **SIM800L RX** | `PA2` (`USART2_TX`)

 | Serial Output | Connected to SIM800L RX via 10k/10k voltage divider

 |
| **Programming Port** | `PA9` / `PA10` (`USART1`)

 | Serial TTL | Connected to external USB-to-TTL programmer

 |

### Hardware Power Requirements

* **SIM800L Supply Voltage:** Requires a dedicated supply between **3.7V and 4.4V** capable of providing up to **2A peak burst current** during cellular network handshakes.


* **Logic Level Translation:** SIM800L RX requires a voltage divider (e.g., two 10k resistors) between the STM32 3.3V TX (`PA2`) and GSM module RX to protect input stages.


* **AC Sensing Circuit:** Mains AC 220V is stepped down via current limiting resistors (e.g., 47k), rectified via a bridge rectifier with filter capacitor, and isolated using an optocoupler before connecting to `PA0`.



---

## Operation & Usage

### 1. Registering an Admin Phone Number

#### Method A: Hardware Button (Learn Mode)

1. Press the tactile button connected to **PB11**.


2. The indicator LED on **PB13** lights up, confirming the system is in Admin Learn Mode.


3. Place a phone call to the SIM card inside the controller.


4. The system reads the Caller Line Identification (CLIP/CLCC), saves the number into EEPROM, turns off the **PB13** LED, hangs up the call, and sends an SMS confirmation to the new administrator.


5. Note: If no call is detected within 60 seconds, the mode times out automatically.



#### Method B: SMS Command

Send the following SMS text from the existing admin phone:

```text
SET 017XXXXXXXX

```

(If `REQUIRE_CONFIRM_BUTTON_FOR_SET` is enabled in firmware, the button on PB11 must be held when sending the command).

---

### 2. Controlling the Pump

* **Missed Call Control:**
* Call the system from the registered admin number.


* If AC power is present, the system will toggle the pump relay state (`ON` $\leftrightarrow$ `OFF`), disconnect the call, and send an SMS notification with the new state.


* If AC power is missing, the system will disconnect the call and reject the operation via SMS.


* Calls from unauthorized numbers are rejected automatically.




* **SMS Control Commands (Admin Only):**
* `ON` – Energizes the pump relay (fails if AC power is not present).


* `OFF` – De-energizes the pump relay.


* `STATUS` – Queries the live status of the system (Power, Pump state, Admin number).





---

## Firmware Setup & Installation

1. **Prerequisites:**
* Arduino IDE with either the **STM32duino (official STMicroelectronics core)** or **Arduino_STM32 (Roger Clark)** core installed.
* `EEPROM` emulation library supported by your STM32 core.




2. **Flashing via USB-to-TTL Adapter:**
* Set **BOOT0** jumper to **1** and **BOOT1** jumper to **0**.
* Connect USB-to-TTL `TX` to `PA10` (`RX1`), `RX` to `PA9` (`TX1`), and connect shared `GND`.


* Press the physical **RESET** button on the STM32 board.
* Select your board type and upload using baud rate **115200** or **57600**.
* Once upload completes, move **BOOT0** back to **0** and press **RESET**.

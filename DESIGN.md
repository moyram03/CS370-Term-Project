# Design Document
## Architecture
The system we are creating will use a Raspberry Pi4 Model B connected to two sensors: an Adafruit Si7021 humidity sensor and a waterproof DS18B20 temperature sensor. The sensors will collect environmental readings near washing machines/humid areas to help identify certain conditions that can encourage or cause mold or mildew growth which can cause impacts to health or damage property.

The system will use separate processes to collect data and send the readings to a more centralized process for analysis and keeping the storage. The data that is collected will be saved locally so users can review conditions that might be changing from current and previous conditions.
## Mechanism Mapping
### Mechanism D
Our system will store temperature and humidity readings in an append only log file. Each reading will include the time it was recorded. This will allow users to look at past readings and see when conditions might have been good for mold growth. We will also make sure the system can recover if it loses power while saving data.


### Mechanism E
Our system will use separate processes to read data from each sensor. These processes will send their readings to a main process that handles the data. We will also have a supervisor that checks if the processes are running and restarts them if they stop working. This will help the device continue running without someone having to watch it.

## Failure Modes
### Humidity Sensor Failure
- Detection: The sensor stops sending readings or returns an error.
- - Response: The system tries to reconnect and continues collecting temperature data.
- Log: Records the sensor error and when it recovers.

### Temperature Sensor Failure
- Detection: The sensor stops sending readings or returns an error.
- Response: The system tries to reconnect and continues collecting humidity data.
- Log: Records the sensor error and when it recovers.

### Process Crash
- Detection: The supervisor notices that a process has stopped running.
- Response: The supervisor restarts the process.
- Log: Records when the process crashed and restarted.

### Power Outage
- Detection: When the Raspberry Pi starts again, the system checks for incomplete records.
- Response: The system recovers saved data and starts collecting readings again.
- Log: Records the restart time and recovery status.

### Storage Gets Full
- Detection: The system receives an error when trying to save data.
- Response: The system reports the problem and stops saving new readings until space is available.
- Log: Records the storage error when possible.


## 4. Storage and Data
## 5. Constraints and Substitutions
## 6. Evaluation Plan
## 7. Ownership Map
## 8. AI-Use Plan

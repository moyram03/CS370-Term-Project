# Design Document
## Architecture
The system we are creating will use a Raspberry Pi4 Model B connected to two sensors: an Adafruit Si7021 humidity sensor and a waterproof DS18B20 temperature sensor. The sensors will collect environmental readings near washing machines/humid areas to help identify certain conditions that can encourage or cause mold or mildew growth which can cause impacts to health or damage property.

The system will use separate processes to collect data and send the readings to a more centralized process for analysis and keeping the storage. The data that is collected will be saved locally so users can review conditions that might be changing from current and previous conditions.
## Mechanism Mapping
### Mechanism D
Our system will store temperature and humidity readings in an append only log file. Each reading will include the time it was recorded. This will allow users to look at past readings and see when conditions might have been good for mold growth. We will also make sure the system can recover if it loses power while saving data.


### Mechanism E
Our system will use separate processes to read data from each sensor. These processes will send their readings to a main process that handles the data. We will also have a supervisor that checks if the processes are running and restarts them if they stop working. This will help the device continue running without someone having to watch it. Additionally, it will allow the sensors to run asynchronously and not wait for one another.

## Failure Modes
### Humidity Sensor Failure
- Detection: The sensor stops sending readings or returns an error.
- Response: The system tries to reconnect and continues collecting temperature data.
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


## Storage and Data
Our system will collect temperature and humidity readings every 30 seconds. Each reading will include a timestamp, temperature, and humidity level. The readings will be stored locally on the Raspberry Pi using an append-only log file.

New readings will be added to the end of the file so previous data is not lost. We plan to keep the data for 30 days before removing older readings to save space.

The system will use fsync() every 5 minutes to save the readings to storage. If the Raspberry Pi loses power, some recent readings may be lost. When it restarts, the system will check for incomplete records and recover the valid data.

## Constraints and Substitutions
Our system will use a Raspberry Pi 4 Model B with an Adafruit Si7021 humidity sensor and a waterproof DS18B20 temperature sensor. Both sensors are available, and we are currently working on setting them up and testing them.

One limitation we noticed is that the DS18B20 temperature sensor takes longer to provide readings. We will need to account for this when collecting data every 30 seconds.

The Raspberry Pi will also need a constant power source to operate. If the power goes out, the system will stop collecting readings until power is restored.

## Evaluation Plan
We will test our system to make sure both sensors collect temperature and humidity readings every 30 seconds. We will also check that the readings are being saved correctly and that the slower temperature sensor does not prevent the system from collecting data.

To test our storage system, we will simulate a power outage while data is being saved. When the Raspberry Pi restarts, we will check that the system recovers valid readings and continues collecting new data.

We will also test our supervisor by stopping one of the sensor processes to see if it automatically restarts. Finally, we plan to leave the system running for 48 hours to make sure it can collect and store data without needing someone to monitor it.

## 7. Ownership Map
## 8. AI-Use Plan

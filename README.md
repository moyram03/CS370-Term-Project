# Problem memo -- M&R (Moises Ramirez, Rishab Karki)
## The user
A person or business that regularly uses washing machines, such as someone living in an apartment or a commercial laundromat managing multiple machines. The device would sit next to or on a washing machine to monitor the conditions around wet laundry.
## The problem
Washing machine areas can remain warm and humid, creating conditions where mold and mildew are more likely to develop. This can lead to odors, cleaning costs, and possible damage if the conditions go unnoticed.
## Why a device
The device needs to continuously monitor the washing machine area, including when no one is present. A phone cannot directly measure the temperature and humidity of the area throughout the day.
## The sensors
Adafruit Si7021 Humidity Sensor
Waterproof DS18B20 Digital Temperature Sensor
They will cooperate with one another by relaying the information to the device, ie whether the relative humidity level is adequate for mold to grow, while the other device will verify by seeing if the temperature is within mold range.
## The mechanisms
First guess at two items from the Section 3.2 menu, one sentence
of justification each. Allowed to change by the design doc.

F - High rate sampling pipeline
D - Custom storage layer
## The risk
The thing most likely to sink our project would be unreliable temperature and humidity conditions. Might give false positives.
// general libraries
#include <SPIFFS.h>
#include <Wire.h>

// System Configuration
#define POWER_MONITOR_MODE   0  // SET TO 1 TO ENABLE INA226 ON THIS ESP32, 0 TO DISABLE (make sure this matches sensorcontrol ESP32)

#if POWER_MONITOR_MODE
#include <INA226_WE.h>
#endif

// defs
#define Serial_RX_PIN       20
#define Serial_TX_PIN       21

#if POWER_MONITOR_MODE
#define SDA_PIN              21
#define SCL_PIN              22
#define POWER_I2C_ADDRESS    0x41

// Smart power monitoring: 2 readings per sensor cycle (when sensors active + when in deep sleep)
unsigned long lastSensorPacketTime = 0;
bool powerReadingScheduled = false;
bool activeReadingTaken = false;  // Flag to prevent multiple active readings per transmission
#endif

// file management
File dataFile;
#if POWER_MONITOR_MODE
File powerFile;
#endif

// control flags
bool printReceivedPackets = true;  // Toggle for printing received packets
bool loggingEnabled = true;        // Whether logging is enabled (disabled when file is full)
#if POWER_MONITOR_MODE
bool powerLoggingEnabled = true;   // Whether power logging is enabled
#endif

// file management settings
const size_t MAX_FILE_SIZE = 300000;  // 300KB max file size for sensor data (remainder after power allocation)
#if POWER_MONITOR_MODE
const size_t MAX_POWER_FILE_SIZE = 950000;  // 950KB max file size for power data (prioritized for full battery life)
unsigned long lastPowerSample = 0;
INA226_WE ina226 = INA226_WE(POWER_I2C_ADDRESS);
#endif

// Validate if received packet is a valid sensor data packet
bool isValidDataPacket(String packet) {
  // Must have minimum length
  if (packet.length() < 10) return false;
  
  // Must start with a number (temperature value) or negative sign
  char firstChar = packet.charAt(0);
  if (!isDigit(firstChar) && firstChar != '-') return false;
  
  // Must contain commas (CSV format)
  if (packet.indexOf(',') == -1) return false;
  
  // Count commas based on power monitor mode
#if POWER_MONITOR_MODE
  // Power monitoring disabled on sensor ESP32 - expect 7 fields (6 commas)
  int expectedCommas = 6;
#else
  // Power monitoring enabled on sensor ESP32 - expect 9 fields (8 commas) 
  int expectedCommas = 8;
#endif
  
  int commaCount = 0;
  for (int i = 0; i < packet.length(); i++) {
    if (packet.charAt(i) == ',') commaCount++;
  }
  if (commaCount != expectedCommas) return false;
  
  // Filter out known boot message patterns
  if (packet.indexOf("ets") != -1) return false;
  if (packet.indexOf("rst:") != -1) return false;
  if (packet.indexOf("boot:") != -1) return false;
  if (packet.indexOf("configsip:") != -1) return false;
  if (packet.indexOf("load:") != -1) return false;
  if (packet.indexOf("entry") != -1) return false;
  if (packet.indexOf("mode:") != -1) return false;
  
  return true;
}

void setup() {
  // uart connection to wireless tracker (receive data from sensorcontrol ESP32)
  Serial.begin(9600, SERIAL_8N1, Serial_RX_PIN, Serial_TX_PIN);

#if POWER_MONITOR_MODE
  // Initialize I2C for power monitoring
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize INA226 power sensor
  if(!ina226.init()){
    Serial.println("Failed to init INA226. Check your wiring.");
    while(1){}
  } else {
    Serial.println("Power sensor started on battery_life_test ESP32.");
  }
  ina226.waitUntilConversionCompleted();
#endif

  // Initialize SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  
  Serial.println("Battery Life Test - Data Logger Started");
#if POWER_MONITOR_MODE
  Serial.println("Power monitoring: ENABLED on this ESP32");
#else
  Serial.println("Power monitoring: DISABLED (handled by sensor ESP32)");
#endif
  Serial.println("Waiting for sensor data packets...");
  Serial.println("Type 'help' for available commands");
  Serial.println();
  
  // Create or open data file
  dataFile = SPIFFS.open("/sensor_data.txt", FILE_APPEND);
  if (!dataFile) {
    Serial.println("Failed to open data file");
  } else {
    Serial.println("Data file opened successfully");
    // Write header if file is new
    if (dataFile.size() == 0) {
#if POWER_MONITOR_MODE
      dataFile.println("timestamp_ms,temp_C,hum_pct,pres_hPa,gas_Ohm,pm1_std,pm2_5_std,pm10_std");
#else
      dataFile.println("timestamp_ms,temp_C,hum_pct,pres_hPa,gas_Ohm,pm1_std,pm2_5_std,pm10_std,busVoltage_V,current_mA");
#endif
    }
    // Check if file is already full
    if (dataFile.size() >= MAX_FILE_SIZE) {
      loggingEnabled = false;
      Serial.println("WARNING: Data file is full! Use 'clear' command to delete data and resume logging.");
    }
    dataFile.close();
  }

#if POWER_MONITOR_MODE
  // Create or open power data file
  powerFile = SPIFFS.open("/power_data.txt", FILE_APPEND);
  if (!powerFile) {
    Serial.println("Failed to open power data file");
  } else {
    Serial.println("Power data file opened successfully");
    // Write header if file is new
    if (powerFile.size() == 0) {
      powerFile.println("timestamp_ms,busVoltage_V,current_mA,phase");
    }
    // Check if file is already full
    if (powerFile.size() >= MAX_POWER_FILE_SIZE) {
      powerLoggingEnabled = false;
      Serial.println("WARNING: Power data file is full! Use 'clearpower' command to delete data and resume logging.");
    }
    powerFile.close();
  }
#endif
}

void loop() {
#if POWER_MONITOR_MODE
  // Synchronized power monitoring: Take reading 30 seconds after each sensor packet (during deep sleep)
  if (powerReadingScheduled && (millis() - lastSensorPacketTime >= 30000)) {
    if (powerLoggingEnabled) {
      float busVoltage = ina226.getBusVoltage_V();
      float current = ina226.getCurrent_mA();
      
      // Log power data with sleep phase indicator
      File checkPowerFile = SPIFFS.open("/power_data.txt", FILE_READ);
      if (checkPowerFile && checkPowerFile.size() >= MAX_POWER_FILE_SIZE) {
        checkPowerFile.close();
        powerLoggingEnabled = false;
        Serial.println("POWER FILE FULL! Use 'clearpower' command to delete data and resume.");
      } else {
        if (checkPowerFile) checkPowerFile.close();
        
        powerFile = SPIFFS.open("/power_data.txt", FILE_APPEND);
        if (powerFile) {
          String powerData = String(millis()) + "," + String(busVoltage) + "," + String(current) + ",s";
          powerFile.println(powerData);
          powerFile.close();
          
          if (printReceivedPackets) {
            Serial.print("Power (sleep): ");
            Serial.println(powerData);
          }
        }
      }
    }
    powerReadingScheduled = false;  // Reset the flag
    activeReadingTaken = false;     // Allow next active reading for next transmission cycle
  }
#endif
  
  // Check if data is available from Serial (sensorcontrol ESP32)
  if (Serial.available()) {
#if POWER_MONITOR_MODE
    // Take immediate power reading as soon as data starts arriving (capture active phase)
    // Only take one active reading per transmission cycle
    if (powerLoggingEnabled && !activeReadingTaken) {
      unsigned long immediateTimestamp = millis();
      float busVoltage = ina226.getBusVoltage_V();
      float current = ina226.getCurrent_mA();
      
      File checkPowerFile = SPIFFS.open("/power_data.txt", FILE_READ);
      if (checkPowerFile && checkPowerFile.size() < MAX_POWER_FILE_SIZE) {
        checkPowerFile.close();
        
        powerFile = SPIFFS.open("/power_data.txt", FILE_APPEND);
        if (powerFile) {
          String powerData = String(immediateTimestamp) + "," + String(busVoltage) + "," + String(current) + ",a";
          powerFile.println(powerData);
          powerFile.close();
          
          if (printReceivedPackets) {
            Serial.print("Power (active): ");
            Serial.println(powerData);
          }
        }
        
        // Mark that we've taken an active reading for this transmission cycle
        activeReadingTaken = true;
        
        // Schedule sleep power reading for 30 seconds from now
        lastSensorPacketTime = immediateTimestamp;
        powerReadingScheduled = true;
      } else if (checkPowerFile) {
        checkPowerFile.close();
      }
    }
#endif

    String receivedPacket = Serial.readStringUntil('\n');
    receivedPacket.trim(); // Remove any trailing whitespace/newlines
    
    // Filter out invalid packets (boot messages, empty lines, etc.)
    if (isValidDataPacket(receivedPacket)) {
      // Add timestamp from battery_life_test (more reliable than sensor millis() in deep sleep)
      unsigned long timestamp = millis();
      String timestampedPacket = String(timestamp) + "," + receivedPacket;
      
      // Print to USB Serial for monitoring (only if enabled)
      if (printReceivedPackets) {
        Serial.print("Received: ");
        Serial.println(timestampedPacket);
      }
      
      // Save to file (only if logging is enabled and file isn't full)
      if (loggingEnabled) {
        // Check file size before writing
        File checkFile = SPIFFS.open("/sensor_data.txt", FILE_READ);
        if (checkFile && checkFile.size() >= MAX_FILE_SIZE) {
          checkFile.close();
          loggingEnabled = false;
          Serial.println("FILE FULL! Logging disabled. Use 'clear' command to delete data and resume.");
        } else {
          if (checkFile) checkFile.close();
          
          dataFile = SPIFFS.open("/sensor_data.txt", FILE_APPEND);
          if (dataFile) {
            dataFile.println(timestampedPacket);
            dataFile.close();
            if (printReceivedPackets) {
              Serial.println("Data saved to file");
            }
          } else {
            Serial.println("Error opening file for writing");
          }
        }
      } else {
        if (printReceivedPackets) {
          Serial.println("Logging disabled - file is full");
        }
      }
    } else if (receivedPacket == "print" || receivedPacket == "show" || receivedPacket == "data") {
      Serial.println("\n=== STORED DATA ===");
      printStoredData();
      Serial.println("=== END OF DATA ===\n");
#if POWER_MONITOR_MODE
    } else if (receivedPacket == "printpower" || receivedPacket == "showpower" || receivedPacket == "power") {
      Serial.println("\n=== POWER DATA ===");
      printPowerData();
      Serial.println("=== END OF POWER DATA ===\n");
#endif
    } else if (receivedPacket == "clear" || receivedPacket == "delete") {
      clearStoredData();
#if POWER_MONITOR_MODE
    } else if (receivedPacket == "clearpower" || receivedPacket == "deletepower") {
      clearPowerData();
#endif
    } else if (receivedPacket == "toggle" || receivedPacket == "quiet" || receivedPacket == "verbose") {
      togglePrintMode();
    } else if (receivedPacket == "status") {
      printStatus();
    } else if (receivedPacket == "wipe" || receivedPacket == "format") {
      wipeAllFiles();
    } else if (receivedPacket == "help" || receivedPacket == "?") {
      printHelp();
    } else {
      // Optionally log filtered packets for debugging
      if (printReceivedPackets && receivedPacket.length() > 0) {
        Serial.print("Filtered: ");
        Serial.println(receivedPacket);
      }
    }
  }
  
  // Small delay to prevent overwhelming the serial buffer
  delay(10);
}

void printStoredData() {
  File file = SPIFFS.open("/sensor_data.txt", FILE_READ);
  if (file) {
    while (file.available()) {
      Serial.write(file.read());
    }
    file.close();
  } else {
    Serial.println("Error: Could not open data file for reading");
  }
}

void clearStoredData() {
  if (SPIFFS.remove("/sensor_data.txt")) {
    Serial.println("Data file cleared successfully!");
    // Recreate the file with header
    dataFile = SPIFFS.open("/sensor_data.txt", FILE_WRITE);
    if (dataFile) {
#if POWER_MONITOR_MODE
      dataFile.println("timestamp_ms,temp_C,hum_pct,pres_hPa,gas_Ohm,pm1_std,pm2_5_std,pm10_std");
#else
      dataFile.println("timestamp_ms,temp_C,hum_pct,pres_hPa,gas_Ohm,pm1_std,pm2_5_std,pm10_std,busVoltage_V,current_mA");
#endif
      dataFile.close();
      Serial.println("New data file created with header");
      loggingEnabled = true;  // Re-enable logging
      Serial.println("Logging resumed");
    } else {
      Serial.println("Error: Could not create new data file after clearing");
    }
  } else {
    Serial.println("Error: Could not clear data file");
  }
}

#if POWER_MONITOR_MODE
void printPowerData() {
  File file = SPIFFS.open("/power_data.txt", FILE_READ);
  if (file) {
    while (file.available()) {
      Serial.write(file.read());
    }
    file.close();
  } else {
    Serial.println("Error: Could not open power data file for reading");
  }
}

void clearPowerData() {
  if (SPIFFS.remove("/power_data.txt")) {
    Serial.println("Power data file cleared successfully!");
    // Recreate the file with header
    powerFile = SPIFFS.open("/power_data.txt", FILE_WRITE);
    if (powerFile) {
      powerFile.println("timestamp_ms,busVoltage_V,current_mA,phase");
      powerFile.close();
      Serial.println("New power data file created with header");
      powerLoggingEnabled = true;  // Re-enable logging
      Serial.println("Power logging resumed");
    } else {
      Serial.println("Error: Could not create new power data file after clearing");
    }
  } else {
    Serial.println("Error: Could not clear power data file");
  }
}
#endif

void printHelp() {
  Serial.println("\n=== AVAILABLE COMMANDS ===");
  Serial.println("print/show/data - Display stored sensor data");
#if POWER_MONITOR_MODE
  Serial.println("printpower/showpower/power - Display stored power data");
#endif
  Serial.println("clear/delete    - Clear sensor data file and resume logging");
#if POWER_MONITOR_MODE
  Serial.println("clearpower/deletepower - Clear power data file and resume logging");
#endif
  Serial.println("toggle/quiet/verbose - Toggle packet printing on/off");
  Serial.println("status          - Show current logging status");
  Serial.println("wipe/format     - Delete ALL files from memory");
  Serial.println("help/?          - Show this help message");
  Serial.println("==========================\n");
}

void togglePrintMode() {
  printReceivedPackets = !printReceivedPackets;
  Serial.print("Packet printing is now: ");
  Serial.println(printReceivedPackets ? "ON (verbose mode)" : "OFF (quiet mode)");
  Serial.println("Data is still being saved to file regardless of this setting.");
}

void printStatus() {
  Serial.println("\n=== LOGGER STATUS ===");
  Serial.print("Packet printing: ");
  Serial.println(printReceivedPackets ? "ON (verbose)" : "OFF (quiet)");
  Serial.print("Sensor logging status: ");
  Serial.println(loggingEnabled ? "ENABLED" : "DISABLED (file full)");
#if POWER_MONITOR_MODE
  Serial.print("Power logging status: ");
  Serial.println(powerLoggingEnabled ? "ENABLED" : "DISABLED (file full)");
#endif
  
  // Check sensor data file size
  File file = SPIFFS.open("/sensor_data.txt", FILE_READ);
  if (file) {
    size_t fileSize = file.size();
    Serial.print("Sensor data file size: ");
    Serial.print(fileSize);
    Serial.print(" / ");
    Serial.print(MAX_FILE_SIZE);
    Serial.print(" bytes (");
    Serial.print((fileSize * 100) / MAX_FILE_SIZE);
    Serial.println("% full)");
    file.close();
  } else {
    Serial.println("Sensor data file: Not found");
  }

#if POWER_MONITOR_MODE
  // Check power data file size
  File powerFile = SPIFFS.open("/power_data.txt", FILE_READ);
  if (powerFile) {
    size_t fileSize = powerFile.size();
    Serial.print("Power data file size: ");
    Serial.print(fileSize);
    Serial.print(" / ");
    Serial.print(MAX_POWER_FILE_SIZE);
    Serial.print(" bytes (");
    Serial.print((fileSize * 100) / MAX_POWER_FILE_SIZE);
    Serial.println("% full)");
    powerFile.close();
  } else {
    Serial.println("Power data file: Not found");
  }
#endif
  
  // Check SPIFFS usage
  Serial.print("SPIFFS used: ");
  Serial.print(SPIFFS.usedBytes());
  Serial.print(" / ");
  Serial.print(SPIFFS.totalBytes());
  Serial.print(" bytes (");
  Serial.print(SPIFFS.totalBytes() / 1024.0 / 1024.0, 2);
  Serial.println(" MB total)");
  Serial.println("===================\n");
}

void wipeAllFiles() {
  Serial.println("WIPING ALL FILES FROM MEMORY...");
  if (SPIFFS.format()) {
    Serial.println("All files deleted successfully!");
    Serial.println("Creating new data files...");
    
    // Recreate the sensor data file
    dataFile = SPIFFS.open("/sensor_data.txt", FILE_WRITE);
    if (dataFile) {
#if POWER_MONITOR_MODE
      dataFile.println("timestamp_ms,temp_C,hum_pct,pres_hPa,gas_Ohm,pm1_std,pm2_5_std,pm10_std");
#else
      dataFile.println("timestamp_ms,temp_C,hum_pct,pres_hPa,gas_Ohm,pm1_std,pm2_5_std,pm10_std,busVoltage_V,current_mA");
#endif
      dataFile.close();
      Serial.println("New sensor data file created with header");
      loggingEnabled = true;
    } else {
      Serial.println("Error: Could not create new sensor data file after wipe");
    }

#if POWER_MONITOR_MODE
    // Recreate the power data file
    powerFile = SPIFFS.open("/power_data.txt", FILE_WRITE);
    if (powerFile) {
      powerFile.println("timestamp_ms,busVoltage_V,current_mA,phase");
      powerFile.close();
      Serial.println("New power data file created with header");
      powerLoggingEnabled = true;
    } else {
      Serial.println("Error: Could not create new power data file after wipe");
    }
#endif
    
    Serial.println("Logging enabled - memory wiped clean!");
  } else {
    Serial.println("Error: Could not format SPIFFS");
  }
}

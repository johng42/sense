/* 
   web_server.ino

   Example code for using the Sense board to serve a web page displaying
   environment data over a WiFi network.
   
   This example is designed for the Arduino Nano 33 IoT only.
   All environmental data values are measured and displayed on a text 
   web page generated by the Arduino, which acts as a simple web server. 
   The page auto-refreshes every three seconds. The Arduino can either 
   connect to an existing WiFi network, or generate its own for other 
   devices to connect to.

   Copyright 2020 Metriful Ltd. 
   Licensed under the MIT License - for further details see LICENSE.txt

   For code examples, datasheet and user guide, visit https://github.com/metriful/sense
*/

#include <Metriful_Sense.h>
#include <stdint.h>
#include <SPI.h>
#include <WiFiNINA.h>

//////////////////////////////////////////////////////////
// USER-EDITABLE SETTINGS

// Choose how often to read and display data (every 3, 100, 300 seconds)
uint8_t cycle_period = CYCLE_PERIOD_3_S;

// The I2C address of the Sense board
uint8_t i2c_7bit_address = I2C_ADDR_7BIT_SB_OPEN;

// Choose whether to print data over the serial port
// as well as displaying on the web page.
bool printSerialData = true;

// Whether to read the particle data (set false if no PPD42 particle 
// sensor is connected, to avoid seeing spurious data).
bool getParticleData = true;

// Choose whether to create a new WiFi network (Arduino as Access Point),
// or connect to an existing WiFi network.
bool createWifiNetwork = true;
// If creating a WiFi network, a static (fixed) IP address ("theIP") is 
// specified by the user.  Otherwise, if connecting to an existing 
// network, an IP address is automatically allocated and the serial 
// output must be viewed at startup to see this allocated IP address.

// Provide the SSID (name) and password for the WiFi network. Depending
// on the choice of createWifiNetwork, this is either created by the 
// Arduino (Access Point mode) or already exists.
// To avoid problems, do not create a new network with the same ssid 
// as an already existing network.
char ssid[] = "PUT WIFI NETWORK NAME HERE IN QUOTES"; // network SSID (name)
char pass[] = "PUT WIFI PASSWORD HERE IN QUOTES";     // network password

// The static IP address of the Arduino, only used when generating a new 
// WiFi network (createWifiNetwork = true). The served web 
// page will be available at  http://<IP address here>
IPAddress theIP(192, 168, 12, 20); 
// e.g. theIP(192, 168, 12, 20) means an IP of 192.168.12.20
//      and the web page will be at http://192.168.12.20

// END OF USER-EDITABLE SETTINGS
//////////////////////////////////////////////////////////

#ifndef ARDUINO_SAMD_NANO_33_IOT
#error ("This example program has been created specifically for the Arduino Nano 33 IoT.")
#endif

WiFiServer server(80);
uint16_t refreshPeriodSeconds;

// Structs for data
AirData_t airData = {0};
AirQualityData_t airQualityData = {0};
LightData_t lightData = {0}; 
ParticleData_t particleData = {0};
SoundData_t soundData = {0};

// Buffer for commands (big enough to fit the largest send transaction):
uint8_t transmit_buffer[LIGHT_INTERRUPT_THRESHOLD_BYTES] = {0};

// Storage for the web page text
char lineBuffer[128] = {0};
char pageBuffer[2100] = {0};

void setup() {
  // Initialize the Arduino pins, set up the serial port and reset:
  SenseHardwareSetup(i2c_7bit_address); 
  
  if (createWifiNetwork) {
    // The Arduino generates its own WiFi network 
    
    // Set the chosen static IP address:
    WiFi.config(theIP);
    
    Serial.print("Creating access point named: ");
    Serial.println(ssid);
    
    if (WiFi.beginAP(ssid, pass) != WL_AP_LISTENING) {
      Serial.println("Creating access point failed.");
      while (true) {}
    }
  }
  else {
    // The Arduino connects to an existing Wifi network
    
    // Wait for the serial port to start because the user must be able
    // to see the printed IP address
    while (!Serial) {}
    
    int status = WL_IDLE_STATUS;
    while (status != WL_CONNECTED) {
      Serial.print("Attempting to connect to network named: ");
      Serial.println(ssid);

      // Connect to WPA/WPA2 network, using DHCP to obtain an IP
      // address. Because the address is not known before this point,
      // a serial monitor must be used to display it to the user.
      status = WiFi.begin(ssid, pass);
      // Wait 10 seconds for connection to complete:
      delay(10000);
    }
    
    // Print the board's IP address: the user must view this output
    IPAddress ip = WiFi.localIP();
    Serial.print("IP Address: ");
    Serial.println(ip);
  }

  // Start the web server
  server.begin();
  
  ////////////////////////////////////////////////////////////////////
  
  // Select how often to refresh the web page. This should be done at
  // least as often as new data are obtained. For the longer time 
  // interval cycles, choose to refresh more often than once per cycle
  // so that the displayed data do not become too old.
  if (cycle_period == CYCLE_PERIOD_3_S) {
    refreshPeriodSeconds = 3;
  }
  else if (cycle_period == CYCLE_PERIOD_100_S) {
    refreshPeriodSeconds = 30;
  }
  else if (cycle_period == CYCLE_PERIOD_300_S) {
    refreshPeriodSeconds = 30;
  }
  else {
    Serial.println("Invalid choice of cycle_period.");
    while (true) {}
  }
  
  // Apply the chosen settings to the Sense board
  if (getParticleData) {
    transmit_buffer[0] = ENABLED;
    TransmitI2C(i2c_7bit_address, PARTICLE_SENSOR_ENABLE_REG, transmit_buffer, 1);
  }
  transmit_buffer[0] = cycle_period;
  TransmitI2C(i2c_7bit_address, CYCLE_TIME_PERIOD_REG, transmit_buffer, 1);

  Serial.println("Entering cycle mode and waiting for data.");
  b_ready_assertion_event = false;
  TransmitI2C(i2c_7bit_address, CYCLE_MODE_CMD, transmit_buffer, 0);
}

void loop() {
  // While waiting for the next data release, 
  // respond to client requests by serving the web page with the last
  // available data. Initially the data will be all zero (until the 
  // first data readout has completed).
  while (!b_ready_assertion_event) {
    handleClientRequests();
  }
  b_ready_assertion_event = false;
  
  // new data are now ready

  /* Read data from Sense into the data structs. 
  For each category of data (air, sound, etc.) a pointer to the data struct is 
  passed to the ReceiveI2C() function. The received byte sequence fills the data 
  struct in the correct order so that each field within the struct receives
  the value of an environmental quantity (temperature, sound level, etc.)
  */ 
  
  // Air data
  ReceiveI2C(i2c_7bit_address, AIR_DATA_READ, (uint8_t *) &airData, AIR_DATA_BYTES);
  
  /* Air quality data
  Note that the initial self-calibration of the air quality data 
  takes a few minutes to complete. During this time the accuracy 
  parameter is zero and the data values do not change.
  */ 
  ReceiveI2C(i2c_7bit_address, AIR_QUALITY_DATA_READ, (uint8_t *) &airQualityData, AIR_QUALITY_DATA_BYTES);
  
  // Light data
  ReceiveI2C(i2c_7bit_address, LIGHT_DATA_READ, (uint8_t *) &lightData, LIGHT_DATA_BYTES);
  
  // Sound data
  ReceiveI2C(i2c_7bit_address, SOUND_DATA_READ, (uint8_t *) &soundData, SOUND_DATA_BYTES);
  
  /* Particle data
  Note that this requires the connection of a PPD42 particle 
  sensor (invalid values will be obtained if this sensor is not present).
  Also note that, due to the low pass filtering used, the 
  particle data become valid after an initial stabilization 
  period of approximately two minutes.
  */ 
  if (getParticleData) {
    ReceiveI2C(i2c_7bit_address, PARTICLE_DATA_READ, (uint8_t *) &particleData, PARTICLE_DATA_BYTES);
  }

  if (printSerialData) {
    // Print all data to the serial port as labeled text
    printAirData(&airData, false);
    printAirQualityData(&airQualityData, false);
    printLightData(&lightData, false);
    printSoundData(&soundData, false);
    if (getParticleData) {
      printParticleData(&particleData, false);
    }
    Serial.println();
  }
  
  // Create the web page ready for client requests
  assembleWebPage();
}


void handleClientRequests(void) {
  // Check for incoming client requests
  WiFiClient client = server.available();   
  if (client) { 
    bool blankLine = true;
    while (client.connected()) {
      if (client.available()) {
        char c = client.read();
        if (c == '\n') {
          // Two consecutive newline characters indicates the end of the client HTTP request
          if (blankLine) {
            // Send the page as a response
            client.println(pageBuffer);
            break; 
          }
          else {
            blankLine = true;
          }
        }
        else if (c != '\r') { 
          // Carriage return (\r) is disregarded for blank line detection
          blankLine = false;
        }
      }
    }
    delay(10);
    // Close the connection:
    client.stop();
  }
}

void assembleWebPage(void) {
  strcpy(pageBuffer,"HTTP/1.1 200 OK\n" "Content-type:text/html\n" "Connection: close\n");
  sprintf(lineBuffer,"Refresh: %i\n\n<!DOCTYPE HTML><html>\
            <head><title>Metriful Sense Demo</title>",refreshPeriodSeconds);
  strcat(pageBuffer,lineBuffer);
  strcat(pageBuffer,"</head><body><h1>Sense Environment Data</h1>");
  
  //////////////////////////////////////
  
  strcat(pageBuffer,"<p><h2>Air Data</h2><table style='width:50%'>");
  
  uint8_t T_positive_integer = airData.T_C_int_with_sign & TEMPERATURE_VALUE_MASK;
  // If the most-significant bit is set, the temperature is negative (below 0 C)
  if ((airData.T_C_int_with_sign & TEMPERATURE_SIGN_MASK) != 0) {
    // The bit is set: celsius temperature is negative
    sprintf(lineBuffer,"<tr><td>Temperature</td><td>-%u.%u</td><td>C</td></tr>",
        T_positive_integer, airData.T_C_fr_1dp);
  }
  else {
    // The bit is not set: celsius temperature is positive
    sprintf(lineBuffer,"<tr><td>Temperature</td><td>%u.%u</td><td>C</td></tr>",
        T_positive_integer, airData.T_C_fr_1dp);
  }
  strcat(pageBuffer,lineBuffer);
  
  sprintf(lineBuffer,"<tr><td>Pressure</td><td>%lu</td><td>Pa</td></tr>", airData.P_Pa);
  strcat(pageBuffer,lineBuffer);
  
  sprintf(lineBuffer,"<tr><td>Humidity</td><td>%u.%u</td><td>%%</td></tr>", 
      airData.H_pc_int, airData.H_pc_fr_1dp);
  strcat(pageBuffer,lineBuffer);
  
  sprintf(lineBuffer,"<tr><td>Gas Sensor Resistance</td><td>%lu</td><td>ohm</td></tr></table></p>", 
      airData.G_ohm);
  strcat(pageBuffer,lineBuffer);

  //////////////////////////////////////
  
  strcat(pageBuffer,"<p><h2>Air Quality Data</h2><table style='width:50%'>");
  
  sprintf(lineBuffer,"<tr><td>Air Quality Index</td><td>%u.%u (%s)</td><td></td></tr>",
      airQualityData.AQI_int, airQualityData.AQI_fr_1dp, interpret_AQI_value(airQualityData.AQI_int));
  strcat(pageBuffer,lineBuffer);
  
  sprintf(lineBuffer,"<tr><td>Estimated CO2</td><td>%u.%u</td><td>ppm</td></tr>", 
      airQualityData.CO2e_int, airQualityData.CO2e_fr_1dp);
  strcat(pageBuffer,lineBuffer);
  
  sprintf(lineBuffer,"<tr><td>Equivalent Breath VOC</td><td>%u.%02u</td><td>ppm</td></tr>", 
      airQualityData.bVOC_int, airQualityData.bVOC_fr_2dp);
  strcat(pageBuffer,lineBuffer);
  
  sprintf(lineBuffer,"<tr><td>Measurement Accuracy</td><td>%s</td><td></td></tr></table></p>", 
      interpret_AQI_accuracy(airQualityData.AQI_accuracy));
  strcat(pageBuffer,lineBuffer);
  
  //////////////////////////////////////
  
  strcat(pageBuffer,"<p><h2>Sound Data</h2><table style='width:50%'>");
  
  sprintf(lineBuffer,"<tr><td>A-weighted Sound Pressure Level</td>"
                     "<td>%u.%u</td><td>dBA</td></tr>",
                     soundData.SPL_dBA_int, soundData.SPL_dBA_fr_1dp);
  strcat(pageBuffer,lineBuffer);
  
  for (uint8_t i=0; i<SOUND_FREQ_BANDS; i++) {
    sprintf(lineBuffer,"<tr><td>Frequency Band %i (%i Hz) SPL</td><td>%u.%u</td><td>dB</td></tr>",
        i+1, sound_band_mids_Hz[i], soundData.SPL_bands_dB_int[i], soundData.SPL_bands_dB_fr_1dp[i]);
    strcat(pageBuffer,lineBuffer);
  }
  
  sprintf(lineBuffer,"<tr><td>Peak Sound Amplitude</td><td>%u.%02u</td><td>mPa</td></tr>", 
      soundData.peak_amp_mPa_int, soundData.peak_amp_mPa_fr_2dp);
  strcat(pageBuffer,lineBuffer);

  if (soundData.stable == 0) {
    strcat(pageBuffer,"<tr><td>Microphone Initialized</td><td>No</td><td></td></tr></table></p>");
  }
  else {
    strcat(pageBuffer,"<tr><td>Microphone Initialized</td><td>Yes</td><td></td></tr></table></p>");
  }
  
  //////////////////////////////////////
  
  strcat(pageBuffer,"<p><h2>Light Data</h2><table style='width:50%'>");
  
  sprintf(lineBuffer,"<tr><td>Illuminance</td><td>%u.%02u</td><td>lux</td></tr>",
      lightData.illum_lux_int, lightData.illum_lux_fr_2dp);
  strcat(pageBuffer,lineBuffer);

  sprintf(lineBuffer,"<tr><td>White Light Level</td><td>%u</td><td></td></tr></table></p>", lightData.white);
  strcat(pageBuffer,lineBuffer);
  
  //////////////////////////////////////
  
  if (getParticleData) {
    strcat(pageBuffer,"<p><h2>Air Particulate Data</h2><table style='width:50%'>");
    
    sprintf(lineBuffer,"<tr><td>Sensor Occupancy</td><td>%u.%02u</td><td>%%</td></tr>",
        particleData.occupancy_pc_int, particleData.occupancy_pc_fr_2dp);
    strcat(pageBuffer,lineBuffer);

    sprintf(lineBuffer,"<tr><td>Particle Concentration</td><td>%u</td><td>ppL</td></tr></table></p>", 
        particleData.concentration_ppL);
    strcat(pageBuffer,lineBuffer);
  }

  //////////////////////////////////////
  
  strcat(pageBuffer,"</body></html>");
}

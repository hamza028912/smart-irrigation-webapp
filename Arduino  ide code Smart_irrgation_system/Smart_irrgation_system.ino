/*
 * Arduino Smart Irrigation System
 * For Arduino Uno Rev4 WiFi
 * Includes:
 * - Pump control via relay
 * - Capacitive soil moisture sensor (V1.2)
 * - Water flow sensor (YF-s201)
 * - Simple watering schedule
 * - Weather-based smart watering mode
 * - send ip of the arduino to my email through IFTTT
 *- openweather added and smmart mode
 */
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <WiFiS3.h>

#define WEBHOOKkey "ibI6sQLrZQuEuMT9ywKGDzWneFLdTOT33wSahn6bTN1"
#define EVENT "send_ip" //webhook event name

// Weather API settings
#define WEATHER_API_KEY "f182e088896241bb87c3e81c5e85a640" // OpenWeatherMap API key
#define JEDDAH_LAT 21.588713 // Latitude for Jeddah
#define JEDDAH_LON 39.191943 // Longitude for Jeddah

// WiFi credentials
const char* ssid = "Abuelyas66";
const char* password = "fa2521zk";

// Pin definitions
const int RELAY_PIN = 8;        // Relay connected to pin 8
const int MOISTURE_PIN = A0;    // Soil moisture sensor on analog pin A0
const int FLOW_SENSOR_PIN = 2;  // Flow sensor on digital pin 2 (supports interrupts)

// Sensor calibration values
const int MOISTURE_AIR_VALUE = 850;    // Value in air (dry)
const int MOISTURE_WATER_VALUE = 300;  // Value in water (wet)

// Flow sensor variables
volatile int flowPulseCount = 0;       // Count of pulses from flow sensor
float flowRate = 0.0;                  // Flow rate in liters per minute
float totalWaterUsed = 0.0;            // Total water used in liters
unsigned long flowLastTime = 0;        // Last time flow was calculated
const float FLOW_CALIBRATION = 7.5;    // Pulses per liter (may need adjustment)

// Simple scheduling system
struct SimpleSchedule {
  bool enabled;             // Is this schedule active?
  unsigned long interval;   // Hours between watering (converted to milliseconds)
  unsigned long duration;   // Watering duration in milliseconds
  unsigned long lastRun;    // Last time this schedule was run
};

SimpleSchedule schedules[3];  // Using array to maintain compatibility with the web app
bool schedulingEnabled = false;
bool currentlyWatering = false;
unsigned long wateringStartTime = 0;
unsigned long wateringDuration = 0;

// Smart watering variables
bool smartModeEnabled = false;
unsigned long lastWeatherCheck = 0;
const unsigned long WEATHER_CHECK_INTERVAL = 30 * 60 * 1000; // 30 minutes

// Weather data structure
struct WeatherData {
  float temperature;       // Current temperature in Celsius
  float humidity;          // Current humidity percentage
  float rainProbability;   // Probability of precipitation (0-1)
  float windSpeed;         // Wind speed in m/s
  long sunrise;            // Sunrise time 
  long sunset;             // Sunset time 
  String weatherCondition; // Main weather condition (Clear, Clouds, Rain, etc.)
};

WeatherData currentWeather;

// Initialize the WiFi server on port 80
WiFiServer server(80);
WiFiClient wifiClient;

// Timing variables for smart watering
unsigned long smartWateringLastCheck = 0;
const unsigned long SMART_WATERING_CHECK_INTERVAL = 30 *  1000; // 15 minutes

// Flow sensor interrupt service routine
void flowPulseCounter() {
  flowPulseCount++;
}

void setup() {
  // Initialize serial communication
  Serial.begin(9600);
  
  // Set pin modes
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  
  // Ensure pump is OFF at startup
  digitalWrite(RELAY_PIN, LOW);
  
  // Attach interrupt for flow sensor
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulseCounter, RISING);
  
  // Connect to WiFi network
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("");
  Serial.println("WiFi connected");
  
  // Start the server
  server.begin();
  Serial.println("Server started");
  
  // Print the IP address
  Serial.print("Arduino IP address: ");
  Serial.println(WiFi.localIP());
  sendIPToIFTTT(); // email me ip address of the arduino
  
  // Initialize flow measurement
  flowLastTime = millis();
  
  // Initialize schedules with default values
  initSchedules();
}

void loop() {
  // Update sensor readings
  updateFlowMeasurements();
  
  // Check schedule if enabled
  if (schedulingEnabled && !smartModeEnabled) {
    checkSchedule();
  }
  
  // Check smart watering mode
  if (smartModeEnabled) {
    checkSmartWatering();
  }
  
  // Check if a client has connected
  WiFiClient client = server.available();
  if (!client) {
    return;
  }
  
  // Wait until the client sends some data
  Serial.println("New client");
  unsigned long timeout = millis() + 3000;  // 3 second timeout
  while (!client.available() && millis() < timeout) {
    delay(1);
  }
  
  if (!client.available()) {
    Serial.println("Client timeout");
    client.stop();
    return;
  }
  
  // Read the first line of the request
  String request = client.readStringUntil('\r');
  Serial.println("Received: " + request);
  
  // Check if this is a POST request with a body
  bool isPost = request.indexOf("POST") == 0;
  String body = "";
  
  // For POST requests, read the body
  if (isPost) {
    // Skip headers until we reach the empty line
    bool emptyLineFound = false;
    timeout = millis() + 3000;  // 3 second timeout
    
    while (client.available() && !emptyLineFound && millis() < timeout) {
      String line = client.readStringUntil('\r');
      client.read(); // consume the \n
      if (line.length() <= 1) { // Empty line is just \n
        emptyLineFound = true;
      }
    }
    
    // Now read the body
    timeout = millis() + 3000;  // 3 second timeout
    while (client.available() && millis() < timeout) {
      char c = client.read();
      body += c;
    }
    
    Serial.println("POST body received: " + body);
  } else {
    client.flush();
  }
  
  // Process the request
  if (request.indexOf("GET /on") != -1) {
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Pump turned ON");
    smartModeEnabled = false;  // Disable smart mode when manual control is used
  }
  else if (request.indexOf("GET /off") != -1) {
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Pump turned OFF");
    smartModeEnabled = false;  // Disable smart mode when manual control is used
  }
  else if (request.indexOf("GET /smart/enable") != -1) {
    smartModeEnabled = true;
    schedulingEnabled = false; // Disable regular scheduling
    currentlyWatering = false; // Stop any current watering
    digitalWrite(RELAY_PIN, LOW); // Turn off pump
    Serial.println("Smart watering mode enabled");
    
    // Get latest weather data
    fetchWeatherData();
  }
  else if (request.indexOf("GET /smart/disable") != -1) {
    smartModeEnabled = false;
    currentlyWatering = false;
    digitalWrite(RELAY_PIN, LOW); // Turn off pump
    Serial.println("Smart watering mode disabled");
  }
  else if (request.indexOf("GET /schedule/enable") != -1) {
    schedulingEnabled = true;
    smartModeEnabled = false;   // Disable smart mode when scheduling is enabled
    Serial.println("Scheduling enabled");
  }
  else if (request.indexOf("GET /schedule/disable") != -1) {
    schedulingEnabled = false;
    currentlyWatering = false; // Also stop any current watering
    digitalWrite(RELAY_PIN, LOW); // Turn off pump
    Serial.println("Scheduling disabled");
  }
  else if (request.indexOf("POST /schedule/update") != -1 ) {
    updateSchedulesFromJSON(body);
  }
  else if (request.indexOf("GET /weather/update") != -1) {


     fetchWeatherData();
  }
  
  // Send response header
  Serial.println("Sending HTTP response");
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain");
  client.println("Access-Control-Allow-Origin: *");  // Allow cross-origin requests
  client.println("");
  
  // Send appropriate response body based on request
  if (request.indexOf("GET /status") != -1) {
    // Return the current pump status
    client.println(digitalRead(RELAY_PIN) == HIGH ? "on" : "off");
  }
  else if (request.indexOf("GET /moisture") != -1) {
    // Return the soil moisture percentage
    client.println(String(getMoisturePercentage()));
  }
  else if (request.indexOf("GET /flow") != -1) {
    // Return the flow rate
    client.println(String(flowRate));
  }
  else if (request.indexOf("GET /water") != -1) {
    // Return the total water used
    client.println(String(totalWaterUsed));
  }
  else if (request.indexOf("GET /data") != -1) {
    // Return all sensor data as JSON
    String jsonData = "{";
    jsonData += "\"pump\":\"" + String(digitalRead(RELAY_PIN) == HIGH ? "on" : "off") + "\",";
    jsonData += "\"moisture\":" + String(getMoisturePercentage()) + ",";
    jsonData += "\"flowRate\":" + String(flowRate) + ",";
    jsonData += "\"totalWater\":" + String(totalWaterUsed) + ",";
    jsonData += "\"smartMode\":" + String(smartModeEnabled ? "true" : "false");
    jsonData += "}";
    client.println(jsonData);
  }
  else if (request.indexOf("GET /schedule") != -1) {
    // Return schedule data
    client.println(getSchedulesJSON());
  }
  else if (request.indexOf("GET /weather") != -1) {
    // Return weather data
    client.println(getWeatherJSON());
  }
  else {
    // Return acknowledgment for other requests
    client.println("OK");
  }
  
  delay(10);
  client.stop();
  Serial.println("Client disconnected");
}

// Get weather data as JSON
String getWeatherJSON() {
  String json = "{";
  json += "\"temperature\":" + String(currentWeather.temperature) + ",";
  json += "\"humidity\":" + String(currentWeather.humidity) + ",";
  json += "\"rainProbability\":" + String(currentWeather.rainProbability) + ",";
  json += "\"windSpeed\":" + String(currentWeather.windSpeed) + ",";
  json += "\"condition\":\"" + currentWeather.weatherCondition + "\",";
  json += "\"lastUpdated\":" + String(lastWeatherCheck);
  json += "}";
  return json;
}

// Fetch weather data from OpenWeatherMap API
void fetchWeatherData() {
  if (String(WEATHER_API_KEY).length() == 0) {
    Serial.println("Weather API key not set");
    return;
  }
  
  // Only fetch every 30 minutes to avoid API rate limits
  if (millis() - lastWeatherCheck < WEATHER_CHECK_INTERVAL && lastWeatherCheck > 0) {
    Serial.println("Weather check skipped (too soon)");
    return;
  }
  
  Serial.println("Fetching weather data...");
  
  // Create URL with specific coordinates for Jeddah
  String url = "/data/2.5/forecast?lat=21.588713&lon=39.191943&units=metric&appid=" + String(WEATHER_API_KEY);
  
  // Name the server
  const char server[] = "api.openweathermap.org";
  
  // Create a WiFi client and make a request
  if (wifiClient.connect(server, 80)) {
    Serial.println("Connected to weather server");
    Serial.println("Sending request to weather server...");
    
    // Make HTTP request
    wifiClient.print("GET " + url + " HTTP/1.1\r\n");
    wifiClient.print("Host: " + String(server) + "\r\n");
    wifiClient.print("Connection: close\r\n\r\n");
    Serial.println("Request sent, waiting for response...");
    
    // Wait for response
    unsigned long timeout = millis() + 5000;
    while (wifiClient.available() == 0 && millis() < timeout) {
      delay(10);
    }
    
    if (millis() >= timeout) {
      Serial.println("Weather API request timeout");
      wifiClient.stop();
      return;
    }
    Serial.println("Response received, processing...");
    
    // Skip HTTP headers
    char endOfHeaders[] = "\r\n\r\n";
    if (!wifiClient.find(endOfHeaders)) {
      Serial.println("Invalid response");
      wifiClient.stop();
      return;
    }
    
    // Just print the raw response instead:
    Serial.println("Raw weather response:");
    Serial.println("----------------------");
    int charCount = 0;
    while (wifiClient.available()) {
      char c = wifiClient.read();
      Serial.print(c);
      charCount++;
      // If response is too long, truncate it
      if (charCount > 1000) {
        Serial.println("\n[Response truncated - too long]");
        break;
      }
    }
    Serial.println("\n----------------------");
    Serial.println("End of raw response");
    
    // Set some dummy values so the rest of code works:
    currentWeather.temperature = 25.0;
    currentWeather.humidity = 50;
    currentWeather.windSpeed = 2.0;
    currentWeather.rainProbability = 0.0;
    currentWeather.weatherCondition = "Clear";
    currentWeather.sunrise = 0;
    currentWeather.sunset = 0;
    
    // Update last check time
    lastWeatherCheck = millis();
    
    Serial.println("Weather data set to dummy values");
    Serial.print("Temperature: ");
    Serial.println(currentWeather.temperature);
    Serial.print("Condition: ");
    Serial.println(currentWeather.weatherCondition);
    
    wifiClient.stop();
  } else {
    Serial.println("Failed to connect to weather server");
  }
}

// Decide whether to water in smart mode
bool shouldWater() {
  int moisture = getMoisturePercentage();
  
  // Don't water if soil is already moist (>50%)
  if (moisture > 50) {
    Serial.println("Smart mode: Soil already moist, no watering needed");
    return false;
  }
  
  // Don't water if it's raining or high chance of rain
  if (currentWeather.weatherCondition == "Rain" || 
      currentWeather.weatherCondition == "Drizzle" || 
      currentWeather.weatherCondition == "Thunderstorm" ||
      currentWeather.rainProbability > 0.4) {
    Serial.println("Smart mode: Rain detected or forecasted, skipping watering");
    return false;
  }
  
  // Water more aggressively in hot weather
  if (currentWeather.temperature > 30 && currentWeather.humidity < 40) {
    // In hot and dry conditions, water even if soil is moderately moist
    return moisture < 45;
  }
  
  // For high humidity, water less frequently
  if (currentWeather.humidity > 70) {
    return moisture < 30; // Only water if very dry
  }
  
  // Default case: water if moisture is below 40%
  return moisture < 40;
}

// Check if smart watering is needed  it checks every 15 minutes
void checkSmartWatering() {
  // Only check every 15 minutes 
  if (millis() - smartWateringLastCheck < SMART_WATERING_CHECK_INTERVAL) {
    return;
  }
  
  smartWateringLastCheck = millis();
  Serial.println("=== Smart Watering Check ===");
  
  // If we're currently watering, check if its time to stop
  if (currentlyWatering) {
    if (millis() - wateringStartTime >= wateringDuration) {
      // Stop watering
      digitalWrite(RELAY_PIN, LOW);
      currentlyWatering = false;
      Serial.println("Smart watering completed");
    }
    return;
  }
  
  // Check if we need to update weather data
  if (millis() - lastWeatherCheck > WEATHER_CHECK_INTERVAL || lastWeatherCheck == 0) {
    fetchWeatherData();
  }
  
  // Get current moisture level
  int moisture = getMoisturePercentage();
  Serial.print("Current moisture: ");
  Serial.print(moisture);
  Serial.println("%");

  // Debug: Show weather values being used
  Serial.print("Using weather values - Temp: ");
  Serial.print(currentWeather.temperature);
  Serial.print("°C, Humidity: ");
  Serial.print(currentWeather.humidity);
  Serial.println("%");
  // Decide if watering is needed
  if (shouldWater()) {
    Serial.println("Decision: SHOULD WATER");

    // Calculate dynamic watering duration based on conditions
    int baseDuration = 5; // Base duration in minutes
    
    // Adjust for very dry soil (more water for drier soil)
    if (moisture < 20) {
      baseDuration += 2; // Add 2 minutes for very dry soil
      Serial.println("Extending watering time: Very dry soil");
    } else if (moisture < 30) {
      baseDuration += 1; // Add 1 minute for moderately dry soil
      Serial.println("Extending watering time: Moderately dry soil");
    }
    
    // Adjust for temperature (more water for hot weather)
    if (currentWeather.temperature > 35) {
      baseDuration += 1; // Add 1 minute for very hot weather
      Serial.println("Extending watering time: Very hot weather");
    }
    
    // Adjust for wind (more water for windy conditions due to evaporation)
    if (currentWeather.windSpeed > 5) {
      baseDuration += 1; // Add 1 minute for windy conditions
      Serial.println("Extending watering time: Windy conditions");
    }
    
    // Cap maximum duration at 10 minutes for safety
    if (baseDuration > 10) {
      baseDuration = 10;
    }
    
    // Set the dynamic watering duration
    wateringDuration = baseDuration * 60 * 1000; // Convert to milliseconds
    
    // Start watering
    digitalWrite(RELAY_PIN, HIGH);
    currentlyWatering = true;
    wateringStartTime = millis();
    
    Serial.print("Smart watering started for ");
    Serial.print(baseDuration);
    Serial.println(" minutes");
  }
}

// Calculate flow rate and total water used
void updateFlowMeasurements() {
  // Calculate flow every second
  if (millis() - flowLastTime > 1000) {
    // Disable interrupt while calculating
    detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
    
    // Calculate flow rate (L/min) = (pulse count / calibration factor) * (60 sec/min)
    flowRate = (flowPulseCount / FLOW_CALIBRATION) * 60.0;
    
    // Calculate water used in this time period (L)
    float waterUsed = flowPulseCount / FLOW_CALIBRATION;
    
    // Add to total water used
    totalWaterUsed += waterUsed;
    
    // Reset pulse count and update time
    flowPulseCount = 0;
    flowLastTime = millis();
    
    // Re-enable interrupt
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulseCounter, RISING);
  }
}

// Get soil moisture as a percentage (0-100%)
int getMoisturePercentage() {
  int moistureValue = analogRead(MOISTURE_PIN);
  
  // Print raw value for debugging
  //Serial.print("Moisture raw value: ");
  //Serial.println(moistureValue);
  
  // Map the sensor values to a percentage (0-100%)
  // Note: For capacitive sensors, higher value = drier soil
  int moisturePercentage = map(moistureValue, MOISTURE_AIR_VALUE, MOISTURE_WATER_VALUE, 0, 100);
  
  // Constrain to ensure we don't go below 0% or above 100%
  moisturePercentage = constrain(moisturePercentage, 0, 100);
  
  return moisturePercentage;
}

// Initialize schedules with default values
void initSchedules() {
  // Schedule 1: We'll only use this one for our simplified interface
  schedules[0].enabled = true;
  schedules[0].interval = 1 * 60 * 60 * 1000; // 1 hour in milliseconds (changed from 12 hours)
  schedules[0].duration = 5 * 60 * 1000;      // 5 minutes in milliseconds
  schedules[0].lastRun = 0;                   // Never run
  
  // Schedule 2 and 3: Initialize but we won't use them
  schedules[1].enabled = false;
  schedules[1].interval = 24 * 60 * 60 * 1000;
  schedules[1].duration = 10 * 60 * 1000;
  schedules[1].lastRun = 0;
  
  schedules[2].enabled = false;
  schedules[2].interval = 48 * 60 * 60 * 1000;
  schedules[2].duration = 15 * 60 * 1000;
  schedules[2].lastRun = 0;
  
  // Scheduling is disabled by default
  schedulingEnabled = false;
}

// Check schedule and start/stop watering
void checkSchedule() {
  unsigned long currentTime = millis();
  
  // If we're currently watering, check if it's time to stop
  if (currentlyWatering) {
    if (currentTime - wateringStartTime >= wateringDuration) {
      // Stop watering
      digitalWrite(RELAY_PIN, LOW);
      currentlyWatering = false;
      Serial.println("Scheduled watering completed");
    }
    return; // Don't check schedule while watering
  }
  
  // For simplicity, we'll only check the first schedule
  if (schedules[0].enabled) {
    // Check if it's time to run the schedule
    if ((schedules[0].lastRun == 0) || 
        (currentTime - schedules[0].lastRun >= schedules[0].interval)) {
      
      // Start watering
      digitalWrite(RELAY_PIN, HIGH);
      currentlyWatering = true;
      wateringStartTime = currentTime;
      wateringDuration = schedules[0].duration;
      schedules[0].lastRun = currentTime;
      
      Serial.println("Starting scheduled watering");
    }
  }
}

// Get JSON string with all schedule data
String getSchedulesJSON() {
  String json = "{\"enabled\":" + String(schedulingEnabled ? "true" : "false") + ",\"schedules\":[";
  
  for (int i = 0; i < 3; i++) {
    if (i > 0) {
      json += ",";
    }
    
    // Convert interval from milliseconds to hours
    int intervalHours = schedules[i].interval / (60 * 60 * 1000);
    
    // Convert duration from milliseconds to minutes
    int durationMinutes = schedules[i].duration / (60 * 1000);
    
    json += "{\"enabled\":" + String(schedules[i].enabled ? "true" : "false") + ",";
    json += "\"interval\":" + String(intervalHours) + ",";
    json += "\"duration\":" + String(durationMinutes) + ",";
    json += "\"lastRun\":" + String(schedules[i].lastRun) + "}";
  }
  
  json += "]}";
  return json;
}

// Parse schedule data from JSON
bool updateSchedulesFromJSON(String json) {
  // Print received JSON for debugging
  Serial.println("Received JSON: " + json);
  
  // Check if input is valid
  if (json.length() < 10 || json.indexOf("{") == -1) {
    Serial.println("Invalid JSON format (too short or missing opening brace)");
    return false;
  }
  
  // Parse main enabled flag
  int mainEnabledPos = json.indexOf("\"enabled\":");
  if (mainEnabledPos > 0) {
    mainEnabledPos += 10; // Skip past "enabled":
    String enabledStr = json.substring(mainEnabledPos, json.indexOf(",", mainEnabledPos));
    enabledStr.trim();
    if (enabledStr == "true") {
      schedulingEnabled = true;
      smartModeEnabled = false; // Disable smart mode when scheduling is enabled
      Serial.println("Scheduling enabled from JSON");
    } else if (enabledStr == "false") {
      schedulingEnabled = false;
      Serial.println("Scheduling disabled from JSON");
    }
  }
  
  // Find "schedules" section
  int schedulesPos = json.indexOf("\"schedules\":");
  if (schedulesPos < 0) {
    Serial.println("No schedules section found in JSON");
    return false;
  }
  
  // Find the array start after "schedules":
  int arrayStart = json.indexOf("[", schedulesPos);
  if (arrayStart < 0) {
    Serial.println("No array start found in schedules section");
    return false;
  }
  
  // Process each schedule object
  int pos = arrayStart;
  for (int i = 0; i < 3; i++) {
    pos = json.indexOf("{", pos);
    if (pos == -1) break;
    
    int endPos = json.indexOf("}", pos);
    if (endPos == -1) {
      Serial.println("Invalid JSON structure - missing closing brace");
      return false;
    }
    
    String scheduleJson = json.substring(pos, endPos + 1);
    Serial.print("Schedule ");
    Serial.print(i);
    Serial.print(" JSON: ");
    Serial.println(scheduleJson);
    
    // Parse individual schedule
    int enabledPos = scheduleJson.indexOf("\"enabled\":");
    int intervalPos = scheduleJson.indexOf("\"interval\":");
    int durationPos = scheduleJson.indexOf("\"duration\":");
    
    if (enabledPos > 0) {
      enabledPos += 10; // Skip past "enabled":
      String enabledStr = scheduleJson.substring(enabledPos, scheduleJson.indexOf(",", enabledPos));
      enabledStr.trim();
      if (enabledStr == "true") {
        schedules[i].enabled = true;
        Serial.print("Schedule ");
        Serial.print(i);
        Serial.println(" enabled");
      } else if (enabledStr == "false") {
        schedules[i].enabled = false;
        Serial.print("Schedule ");
        Serial.print(i);
        Serial.println(" disabled");
      }
    }
    
    if (intervalPos > 0) {
      intervalPos += 11; // Skip past "interval":
      String intervalStr = scheduleJson.substring(intervalPos, scheduleJson.indexOf(",", intervalPos));
      intervalStr.trim();
      int intervalHours = intervalStr.toInt();
      if (intervalHours > 0) {
        schedules[i].interval = intervalHours * 60 * 60 * 1000; // Convert hours to milliseconds
        
        Serial.print("Schedule ");
        Serial.print(i);
        Serial.print(" interval set to ");
        Serial.print(intervalHours);
        Serial.println(" hours");
      }
    }
    
    if (durationPos > 0) {
      durationPos += 11; // Skip past "duration":
      int commaPos = scheduleJson.indexOf(",", durationPos);
      int bracePos = scheduleJson.indexOf("}", durationPos);
      int endOfValuePos = (commaPos > 0 && commaPos < bracePos) ? commaPos : bracePos;
      
      String durationStr = scheduleJson.substring(durationPos, endOfValuePos);
      durationStr.trim();
      int durationMinutes = durationStr.toInt();
      if (durationMinutes > 0) {
        schedules[i].duration = durationMinutes * 60 * 1000; // Convert minutes to milliseconds
        
        Serial.print("Schedule ");
        Serial.print(i);
        Serial.print(" duration set to ");
        Serial.print(durationMinutes);
        Serial.println(" minutes");
      }
    }
    
    pos = endPos + 1; // Move past this schedule object
  }
  
  Serial.println("Schedules updated from JSON");
  return true;
}

void sendIPToIFTTT() {
  // Get the IP address
  String ipAddress = WiFi.localIP().toString();
  // Simple WiFi client
  WiFiClient client;
  
  // Connect directly to IFTTT
  if (client.connect("maker.ifttt.com", 80)) {
    // Build HTTP request
    client.print("POST /trigger/");
    client.print(EVENT);
    client.print("/with/key/");
    client.print(WEBHOOKkey);
    client.println(" HTTP/1.1");
    client.println("Host: maker.ifttt.com");
    client.println("Content-Type: application/json");
  
   // Just send the IP address
    String jsonData = "{\"value1\":\"" + ipAddress + "\"}";
    
    client.print("Content-Length: ");
    client.println(jsonData.length());
    client.println();
    client.println(jsonData);
    
    Serial.println("IP sent to IFTTT");
  } 
  else {
    Serial.println("Failed to connect to IFTTT");
  }
  
  // Close the connection
  client.stop();
}
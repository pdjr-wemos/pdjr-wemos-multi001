/*********************************************************************
 * NAME
 *   multiHTT-0.90.cpp - multi-sensor wireless MQTT node.
 * PLATFORM
 *   Wemos MINI-D1
 * SENSORS
 *   AM2320 (humidity and temperature)
 *   HXD-2801 (ball tilt switch)
 * DESCRIPTION
 *   This firmware implements an IoT MQTT client which monitors ambient
 *   temperature and humidity and the orientation of an attached tilt
 *   sensor.
 * 
 *   Readings are published to a user defined topic on a user configured
 *   MQTT server (see CONFIGURATION below) as a JSON MQTT message of the
 *   form:
 * 
 *   '{ "humidity": humidity, "temperature": temperature, "tilt": tilt  }'
 * 
 *   where:
 * 
 *     humidity is an integer percentage value in the range 0..100;
 *     temperature is an integer Celsius value in the range -40..80;
 *     tilt is integer boolean (1 or 0) indicating 'on' or 'off'.
 * 
 *   The value 999 (undefined) is published to indicate that reading
 *   the associated sensor failed for whatever reason.
 * 
 *   The defined MQTT topic is updated whenever a sensor value changes
 *   or once every 30 seconds. The maximum update rate is once every
 *   three seconds.
 * 
 * CONFIGURATION
 * 
 * The module automatically enters configuration mode when it is unable
 * to connect to an already configured WiFi host network or on first use
 * when no host network has been configured.
 * 
 * In configuration mode the module operates as a wireless access pointOn first use the
 * module must be configured by the user who is required to specify
 * the SSID and password of the hsensed data is
 * published to a user specified MQTT topic on a user defined MQTT
 * server accessed over a user-specfifiea JSON styled message of the form:
 * 
 *   '{ "temperature": t, "lux": l, "motion": m }'
 * 
 * to a user specified topic on a nominated MQTT server.
 * 
 * The code supports a DS18B20 temperature sensor reporting the sensed
 * value <t> in degrees centigrade.
 * 
 * Illumination (lux) level <l> (in the range 0..1023) and detected
 * motion <m> (as 0 or 1) are assumed to derive from a luxControl
 * SmartDim Sensor 2.
 * 
 * On first use (and also when the device is unable to connect to a
 * previously configured wireless network) the device will operate as
 * a wireless access point with the SSID "MULTISENSOR-xxxxxxxxxxxx",
 * where "xxxxxxxxxxxx" is the MAC address of the host wireless
 * interface.
 * 
 * Connecting to the access point will open a captured portal that
 * allows configuration of the following settings:
 * 
 * network - the SSID of the network to which the device should connect
 * password - any password required for connection to ssid
 * server name - the name or IP address of the target MQTT server
 * server port - the port on which the server listens (default 1886)
 * username - login user name required for access to the server
 * password - login password for username
 * topic - the topic on server to which sensor data should be published
 * 
 * Once the entered settings are saved the device will re-boot and
 * immediately attempt to report sensor readings to the configured
 * destination. Detected movement results in an immediate report, but
 * otherwise readings will be reported once every 60 seconds.
 * 
 * AUTHOR
 *   2022(c) Paul Reeve <preeve@pdjr.eu>
 */
 
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <Wire.h>
#include <AM232X.h>
#include <ArduinoJSON.h>

#define DEBUG_SERIAL                      // Enable debug messages
#define DEBUG_SERIAL_START_DELAY 2000     // Milliseconds wait before output

#define GPIO_SW0 14                       // D1-MINI pin D5
#define GPIO_SW1 15                       // D1-MINI pin D5
#define GPIO_SW2 16                       // D1-MINI pin D5
#define GPIO_SW3 17                       // D1-MINI pin D5

#define MODULE_ID_FORMAT "MULTISENSOR-%02x%02x%02x%02x%02x%02x"
#define MQTT_DEFAULT_TOPIC_FORMAT "%s/status"

#define WIFI_SERVER_PORT 80               
#define WIFI_ACCESS_POINT_PORTAL_TIMEOUT 180 // In seconds

#define MQTT_PUBLISH_SOFT_INTERVAL 3000
#define MQTT_PUBLISH_HARD_INTERVAL 30000
#define MQTT_CLIENT_ID "%02x%02x%02x%02x%02x%02x"
#define MQTT_STATUS_MESSAGE "{ \"humidity\": %d, \"temperature\": %d, \"tilt\": %d }" 

#define STORAGE_TEST_ADDRESS 0
#define STORAGE_TEST_VALUE 0xAE
#define MQTT_CONFIG_STORAGE_ADDRESS 1

#define AM2322_STARTUP_DELAY 2000
#define SENSOR_UNDEFINED_VALUE 999

/**********************************************************************
 * Structure to store MQTT configuration properties.
 */
struct MQTT_CONFIG { 
  char servername[40];     // MQTT server Hostname or IP address
  int  serverport;         // MQTT service port (normally 1883)
  char username[20];       // Name of user who can publish to the server
  char password[20];       // Password of named user
  char topic[60];          // MQTT topic on which to publish
  char propertyname0[20];
  char propertyname1[20];
  char propertyname2[20];
  char propertyname3[20];
};

WiFiServer wifiServer(WIFI_SERVER_PORT);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

AM232X AM2322;


/**********************************************************************
 * Setup a WiFi connection to <ssid>, <password> and only return once
 * a connection is established.
 */ 
void setup_wifi(const char* ssid, const char* password) {
  delay(10);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);  
  while (WiFi.status() != WL_CONNECTED) delay(500);
}

void connect_to_mqtt(const char* servername, const int serverport, const char* username, const char* password, const char* clientid) {
  while (!mqttClient.connected()) {
    #ifdef DEBUG_SERIAL
      Serial.print("Trying to connect to MQTT server ");
      Serial.print(servername); Serial.print(":"); Serial.print(serverport);
      Serial.print(" as ");
      Serial.print(username); Serial.print("("); Serial.print(password); Serial.print(")");
      Serial.print(" with client id ");
      Serial.println(clientid);
    #endif

    if (mqttClient.connect(clientid, username, password)) {
      #ifdef DEBUG_SERIAL
        Serial.println("connected");
      #endif
    } else {
      #ifdef DEBUG_SERIAL
        Serial.print("failed (result code = ");
        Serial.print(mqttClient.state());
        Serial.println("). Will try again in 5 seconds.");
      #endif
      delay(5000);
    }
  }
}

void dumpConfig(MQTT_CONFIG &config) {
  #ifdef DEBUG_SERIAL
  Serial.print("MQTT server name: "); Serial.println(config.servername);
  Serial.print("MQTT server port: "); Serial.println(config.serverport);
  Serial.print("MQTT username: "); Serial.println(config.username);
  Serial.print("MQTT password: "); Serial.println(config.password);
  Serial.print("MQTT topic: "); Serial.println(config.topic);
  Serial.print("MQTT SW0 property name: "); Serial.println(config.topic);
  Serial.print("MQTT SW1 property name: "); Serial.println(config.topic);
  Serial.print("MQTT SW2 property name: "); Serial.println(config.topic);
  Serial.print("MQTT SW3 property name: "); Serial.println(config.topic);
  #endif
}

boolean loadConfig(MQTT_CONFIG &config) {
  boolean retval = false;
  EEPROM.begin(512);
  if (EEPROM.read(0) == STORAGE_TEST_VALUE) {
    EEPROM.get(MQTT_CONFIG_STORAGE_ADDRESS, config);
    retval = true;
  }
  EEPROM.end();
  return(retval);
}

void saveConfig(MQTT_CONFIG &config) {
  #ifdef DEBUG_SERIAL
  Serial.println("Saving module configuration to EEPROM");
  dumpConfig(config);
  #endif
  EEPROM.begin(512);
  EEPROM.write(0, STORAGE_TEST_VALUE);
  EEPROM.put(MQTT_CONFIG_STORAGE_ADDRESS, config);
  EEPROM.commit();
  EEPROM.end();
}

bool shouldSaveConfig = false;

void saveConfigCallback() {
  shouldSaveConfig = true; 
}

byte macAddress[6];
char moduleId[40];
char defaultTopic[60];
MQTT_CONFIG mqttConfig;
StaticJsonDocument<200> jsonBuffer;

int CURRENT_HUMIDITY = SENSOR_UNDEFINED_VALUE; // Percent
int CURRENT_TEMPERATURE = SENSOR_UNDEFINED_VALUE; // Degrees Celsius
int CURRENT_SW0 = SENSOR_UNDEFINED_VALUE; // 0 = no tilt detected, 1 = tilt detected
int CURRENT_SW1 = SENSOR_UNDEFINED_VALUE; // 0 = no tilt detected, 1 = tilt detected
int CURRENT_SW2 = SENSOR_UNDEFINED_VALUE; // 0 = no tilt detected, 1 = tilt detected
int CURRENT_SW3 = SENSOR_UNDEFINED_VALUE; // 0 = no tilt detected, 1 = tilt detected

int PREVIOUS_HUMIDITY = CURRENT_HUMIDITY; // Percent
int PREVIOUS_TEMPERATURE = CURRENT_TEMPERATURE; // Degrees Celsius
int PREVIOUS_SW0 = CURRENT_SW0; // 0 = no tilt detected, 1 = tilt detected
int PREVIOUS_SW1 = CURRENT_SW1; // 0 = no tilt detected, 1 = tilt detected
int PREVIOUS_SW2 = CURRENT_SW2; // 0 = no tilt detected, 1 = tilt detected
int PREVIOUS_SW3 = CURRENT_SW3; // 0 = no tilt detected, 1 = tilt detected

void setup() {
  #ifdef DEBUG_SERIAL
  Serial.begin(57600);
  delay(DEBUG_SERIAL_START_DELAY);
  #endif

  // Recover device MAC address and make from it a module identifier
  // that will be used as access point name, MQTT client id and a
  // component of the topic path (unless overriden by the user).
  WiFi.macAddress(macAddress);
  sprintf(moduleId, MODULE_ID_FORMAT, macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
  sprintf(defaultTopic, MQTT_DEFAULT_TOPIC_FORMAT, moduleId);

  // Initialise the MQTT default configuration then try to load any saved data.
  // Create a WiFiManager instance and configure it.
  WiFiManager wifiManager;
  WiFiManagerParameter custom_mqtt_servername("server", "mqtt server", "", 40);
  WiFiManagerParameter custom_mqtt_serverport("port", "mqtt port", "1883", 6);
  WiFiManagerParameter custom_mqtt_username("user", "mqtt user", "", 20);
  WiFiManagerParameter custom_mqtt_password("pass", "mqtt pass", "", 20);
  WiFiManagerParameter custom_mqtt_topic("topic", "mqtt topic", defaultTopic, 40);
  WiFiManagerParameter custom_mqtt_property_name_0("prop0", "mqtt prop name for SW0", "", 20);
  WiFiManagerParameter custom_mqtt_property_name_1("prop1", "mqtt prop name for SW1", "", 20);
  WiFiManagerParameter custom_mqtt_property_name_2("prop2", "mqtt prop name for SW2", "", 20);
  WiFiManagerParameter custom_mqtt_property_name_3("prop3", "mqtt prop name for SW3", "", 20);
  
  // Try to load the module configuration.
  if (loadConfig(mqttConfig)) {
    // When the module WiFi service starts it may not be able to
    // connect to a wifi network and in this case will create an
    // access point to allow module configuration. We need to
    // create and initialise the WiFi manager configuration properties.
    WiFiManagerParameter custom_mqtt_servername("server", "mqtt server", mqttConfig.servername, 40);
    WiFiManagerParameter custom_mqtt_serverport("port", "mqtt port", "" + mqttConfig.serverport, 6);
    WiFiManagerParameter custom_mqtt_username("user", "mqtt user", mqttConfig.username, 20);
    WiFiManagerParameter custom_mqtt_password("pass", "mqtt pass", mqttConfig.password, 20);
    WiFiManagerParameter custom_mqtt_topic("topic", "mqtt topic", mqttConfig.topic, 40);
    WiFiManagerParameter custom_mqtt_property_name_0("prop0", "mqtt prop name for SW0", mqttConfig.propertyname0, 20);
    WiFiManagerParameter custom_mqtt_property_name_1("prop1", "mqtt prop name for SW1", mqttConfig.propertyname1, 20);
    WiFiManagerParameter custom_mqtt_property_name_2("prop2", "mqtt prop name for SW2", mqttConfig.propertyname2, 20);
    WiFiManagerParameter custom_mqtt_property_name_3("prop3", "mqtt prop name for SW3", mqttConfig.propertyname3, 20);
  } else {
    wifiManager.resetSettings();
  }  
  
  // Create a WiFiManager instance and configure it.
  wifiManager.setConfigPortalTimeout(WIFI_ACCESS_POINT_PORTAL_TIMEOUT);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setBreakAfterConfig(true);
  wifiManager.addParameter(&custom_mqtt_servername);
  wifiManager.addParameter(&custom_mqtt_serverport);
  wifiManager.addParameter(&custom_mqtt_username);
  wifiManager.addParameter(&custom_mqtt_password);
  wifiManager.addParameter(&custom_mqtt_topic);
  wifiManager.addParameter(&custom_mqtt_property_name_0);
  wifiManager.addParameter(&custom_mqtt_property_name_1);
  wifiManager.addParameter(&custom_mqtt_property_name_2);
  wifiManager.addParameter(&custom_mqtt_property_name_3);
  
  // Finally, start the WiFi manager. 
  bool res = wifiManager.autoConnect(moduleId);

  // If the configuration data has changed, then get it and save it...
  if (shouldSaveConfig) {
    strcpy(mqttConfig.servername, custom_mqtt_servername.getValue());
    mqttConfig.serverport = atoi(custom_mqtt_serverport.getValue());
    strcpy(mqttConfig.username, custom_mqtt_username.getValue());
    strcpy(mqttConfig.password, custom_mqtt_password.getValue());
    strcpy(mqttConfig.topic, custom_mqtt_topic.getValue());
    strcpy(mqttConfig.propertyname0, custom_mqtt_property_name_0.getValue());
    strcpy(mqttConfig.propertyname1, custom_mqtt_property_name_1.getValue());
    strcpy(mqttConfig.propertyname2, custom_mqtt_property_name_2.getValue());
    strcpy(mqttConfig.propertyname3, custom_mqtt_property_name_3.getValue());
    saveConfig(mqttConfig);
  }

  // If we get to this point then the WiFi manager has either entered
  // configuration mode and being timed out or we are connected to the
  // configured network.  
  if (!res) {
    #ifdef DEBUG_SERIAL
      Serial.println("WiFi configuration or connection failure: restarting system.");
    #endif
    ESP.restart();
  } else {
    #ifdef DEBUG_SERIAL
      Serial.print("Connected to wireless network '");
      Serial.print(WiFi.SSID());
      Serial.println("'");
    #endif
    // We have a WiFi connection, so configure the MQTT connection
    mqttClient.setServer(mqttConfig.servername, mqttConfig.serverport);
    // Start sensing things
    if (! AM2322.begin()) {
      Serial.println("AM2322 sensor not found");
      while (1);
    }
    AM2322.wakeUp();
    pinMode(GPIO_SW0, INPUT_PULLUP);
    delay(AM2322_STARTUP_DELAY);
  }
}

/**********************************************************************
 * Check that we have an MQTT connection and, if not, try and make one
 * and once we have a connection we can...
 * 
 * Once every MQTT_PUBLISH_SOFT_INTERVAL miliseconds read the sensors.
 * If the sensor values have changed since the most recently published
 * or MQTT_PUBLISH_HARD_INTERVAL has elapsed then update the configured
 * topic on the connected MQTT server.
 */
void loop() {
  static long mqttPublishSoftDeadline = 0L;
  static long mqttPublishHardDeadline = 0L;
  static char mqttStatusMessage[128];
  long now = millis();

  // Try and recover a failed server connection
  if (!mqttClient.connected()) connect_to_mqtt(mqttConfig.servername, mqttConfig.serverport, mqttConfig.username, mqttConfig.password, moduleId);
  
  // Perform connection houskeeping
  mqttClient.loop();

  // Check if our time has come
  if (now > mqttPublishSoftDeadline) {
    // Read sensors
    if (strlen(mqttConfig.propertyname0)) CURRENT_SW0 = digitalRead(GPIO_SW0);
    if (strlen(mqttConfig.propertyname1)) CURRENT_SW1 = digitalRead(GPIO_SW1);
    if (strlen(mqttConfig.propertyname2)) CURRENT_SW2 = digitalRead(GPIO_SW1);
    if (strlen(mqttConfig.propertyname3)) CURRENT_SW3 = digitalRead(GPIO_SW3);
    if (AM2322.read() == AM232X_OK) {
      CURRENT_TEMPERATURE = (int) round(AM2322.getTemperature());
      CURRENT_HUMIDITY = (int) round(AM2322.getHumidity());
    } else {
      CURRENT_TEMPERATURE = SENSOR_UNDEFINED_VALUE;
      CURRENT_HUMIDITY = SENSOR_UNDEFINED_VALUE;
    }
    // Check if we should actually publish this data
    if ((CURRENT_HUMIDITY != PREVIOUS_HUMIDITY) || (CURRENT_TEMPERATURE != PREVIOUS_TEMPERATURE) || (CURRENT_SW0 != PREVIOUS_SW0) || (CURRENT_SW1 != PREVIOUS_SW1) || (CURRENT_SW2 != PREVIOUS_SW2) || (CURRENT_SW3 != PREVIOUS_SW3) || (now > mqttPublishHardDeadline)) {
      jsonBuffer["humidity"] = CURRENT_HUMIDITY;
      jsonBuffer["temperature"] = CURRENT_TEMPERATURE;
      if (strlen(mqttConfig.propertyname0)) jsonBuffer[mqttConfig.propertyname0] = CURRENT_SW0;
      if (strlen(mqttConfig.propertyname1)) jsonBuffer[mqttConfig.propertyname1] = CURRENT_SW1;
      if (strlen(mqttConfig.propertyname2)) jsonBuffer[mqttConfig.propertyname2] = CURRENT_SW2;
      if (strlen(mqttConfig.propertyname3)) jsonBuffer[mqttConfig.propertyname3] = CURRENT_SW3;
      serializeJson(jsonBuffer, mqttStatusMessage);
      mqttClient.publish(mqttConfig.topic, mqttStatusMessage, true);
      PREVIOUS_HUMIDITY = CURRENT_HUMIDITY;
      PREVIOUS_TEMPERATURE = CURRENT_TEMPERATURE;
      PREVIOUS_SW0 = CURRENT_SW0;
      PREVIOUS_SW1 = CURRENT_SW1;
      PREVIOUS_SW2 = CURRENT_SW2;
      PREVIOUS_SW3 = CURRENT_SW3;

      #ifdef DEBUG_SERIAL
        Serial.print("Publishing ");
        Serial.print(mqttStatusMessage);
        Serial.print(" to ");
        Serial.println(mqttConfig.topic);
      #endif

      mqttPublishHardDeadline = (now + MQTT_PUBLISH_HARD_INTERVAL);
    }
    mqttPublishSoftDeadline = (now + MQTT_PUBLISH_SOFT_INTERVAL);
  }
}

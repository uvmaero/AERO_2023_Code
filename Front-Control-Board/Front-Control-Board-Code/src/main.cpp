/**
 * @file main.cpp
 * @author Dominic Gasperini - UVM '23
 * @brief this drives the front control board on clean speed 5.5
 * @version 0.9
 * @date 2023-01-27
 */

// *** includes *** // 
#include <stdio.h>
#include "esp_now.h"
#include "esp_err.h"
#include <esp_timer.h>
#include <esp_wifi.h>
#include "esp_netif.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <mcp_can.h>
#include <LoRa.h>
#include "pinConfig.h"


// *** defines *** // 
// gpio
#define GPIO_INPUT_PIN_SELECT           1       

// macros
#define BRAKE_LIGHT_THRESHOLD           10
#define PEDAL_DEADBAND                  10
#define PEDAL_MIN                       128
#define PEDAL_MAX                       600
#define TORQUE_DEADBAND                 5
#define MAX_TORQUE                      225         // MAX TORQUE RINEHART CAN ACCEPT, DO NOT EXCEED 230!!!

// tasks & timers
#define SENSOR_POLL_INTERVAL            100000      // 0.1 seconds in microseconds
#define CAN_WRITE_INTERVAL              100000      // 0.1 seconds in microseconds
#define ARDAN_UPDATE_INTERVAL           250000      // 0.25 seconds in microseconds
#define WCB_UPDATE_INTERVAL             200000      // 0.2 seconds in microseconds
#define TASK_STACK_SIZE                 3500        // in bytes

// debug
#define TESTING                         true
#define ENABLE_DEBUG                    true       // master debug message control


// *** global variables *** //
// Drive Mode Enumeration Type
enum DriveModes
{
  SLOW = 0,
  ECO = 10,
  FAST = 20
};


// Car Data Struct
struct CarData
{
  struct DrivingData
  {
    bool readyToDrive = false;
    bool enableInverter = false;

    bool imdFault = false;
    bool bmsFault = false;

    uint16_t commandedTorque = 0;
    float currentSpeed = 0.0f;
    bool driveDirection = true;             // true = forward | false = reverse
    DriveModes driveMode = ECO;
  } drivingData;
  
  struct BatteryStatus
  {
    uint16_t batteryChargeState = 0;
    int16_t busVoltage = 0;
    int16_t rinehartVoltage = 0;
    float pack1Temp = 0.0f;
    float pack2Temp = 0.0f;
  } batteryStatus;

  struct Sensors
  {
    uint16_t wheelSpeedFR = 0;
    uint16_t wheelSpeedFL = 0;
    uint16_t wheelSpeedBR = 0;
    uint16_t wheelSpeedBL = 0;

    uint16_t wheelHeightFR = 0;
    uint16_t wheelHeightFL = 0;
    uint16_t wheelHeightBR = 0;
    uint16_t wheelHeightBL = 0;

    uint16_t steeringWheelAngle = 0;
  } sensors;

  struct Inputs
  {
    uint16_t pedal0 = 0;
    uint16_t pedal1 = 0;
    uint16_t brake0 = 0;
    uint16_t brake1 = 0;
    uint16_t brakeRegen = 0;
    uint16_t coastRegen = 0;

    float vicoreTemp = 0.0f;
    float pumpTempIn = 0.0f;
    float pimpTempOut = 0.0f;
  } inputs;

  struct Outputs
  {
    bool buzzerActive = false;
    uint8_t buzzerCounter = 0;
    bool brakeLight = false;
  } outputs;
  
};
CarData carData;


struct WCB_Data
{
  struct DrivingData
  {
    bool readyToDrive = false;

    bool imdFault = false;
    bool bmsFault = false;

    bool driveDirection = true;             // true = forward | false = reverse
    DriveModes driveMode = ECO;
  } drivingData;
  
  struct BatteryStatus
  {
    uint16_t batteryChargeState = 0;
    int16_t busVoltage = 0;
    int16_t rinehartVoltage = 0;

    float pack1Temp = 0;
    float pack2Temp = 0;
  } batteryStatus;

    struct Sensors
  {
    uint16_t wheelSpeedFR = 0;
    uint16_t wheelSpeedFL = 0;
    uint16_t wheelSpeedBR = 0;
    uint16_t wheelSpeedBL = 0;

    uint16_t wheelHeightFR = 0;
    uint16_t wheelHeightFL = 0;
    uint16_t wheelHeightBR = 0;
    uint16_t wheelHeightBL = 0;
  } sensors;

  struct IO
  {
    // inputs
    uint8_t brakeRegen = 0;
    uint8_t coastRegen = 0;

    // outputs
    bool buzzerActive = false;
  } io;
};
WCB_Data wcbData; 


// Debug information
struct Debugger
{
  // debug toggle
  bool debugEnabled = ENABLE_DEBUG;
  bool CAN_debugEnabled = false;
  bool WCB_debugEnabled = false;
  bool IO_debugEnabled = false;
  bool scheduler_debugEnable = true;

  // debug data
  byte CAN_sentStatus;
  byte CAN_outgoingMessage[8];

  esp_err_t WCB_updateResult;
  WCB_Data WCB_updateMessage;

  CarData IO_data;

  // scheduler data
  int sensorTaskCount = 0;
  int canTaskCount = 0;
  int ardanTaskCount = 0;
  int wcbTaskCount = 0;
};
Debugger debugger;


// ESP-Now Connection
uint8_t wcbAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info wcbInfo;


// CAN
MCP_CAN CAN0(10);       // set CS pin to 10


// *** function declarations *** //
// callbacks
void SensorCallback(void* args);
void CANCallback(void* args);
void ARDANCallback(void* args);
void WCBCallback(void* args);


// tasks
void ReadSensorsTask(void* pvParameters);
void UpdateCANTask(void* pvParameters);
void UpdateARDANTask(void* pvParameters);
void UpdateWCBTask(void* pvParameters);

// ISRs
void WCBDataReceived(const uint8_t* mac, const uint8_t* incomingData, int length);

// helpers
void GetCommandedTorque();
long MapValue(long x, long in_min, long in_max, long out_min, long out_max);
void PrintDebug();
void PrintCANDebug();
void PrintWCBDebug();
void PrintIODebug();


// *** setup *** //
void setup()
{
  // --- initialize serial --- //
  Serial.begin(9600);
  Serial.printf("\n\n|--- STARTING SETUP ---|\n\n");

  struct setup
  {
    bool ioActive = false;
    bool canActive = false;
    bool ardanActive = false;
    bool wcbActive = false;
  };
  setup setup;

  // --- initialize I/O --- //
  // inputs / sensors // 
  gpio_config_t sensor_config = {
    .pin_bit_mask = GPIO_INPUT_PIN_SELECT,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  ESP_ERROR_CHECK(gpio_config(&sensor_config));

  // setup adc 1
  ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_12));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_2, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_3, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_5, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_0db));

  // setup adc 2
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_0, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_1, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_2, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_3, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_4, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_5, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_6, ADC_ATTEN_0db));
  ESP_ERROR_CHECK(adc2_config_channel_atten(ADC2_CHANNEL_7, ADC_ATTEN_0db));


  // outputs //

  setup.ioActive = true;
  // -------------------------------------------------------------------------- //


  // --- initalize CAN --- //  
  if (CAN0.begin(MCP_ANY, CAN_500KBPS, MCP_16MHZ) == CAN_OK) {
    Serial.printf("CAN INIT [ SUCCESS ]\n");

    // set mode to read and write
    CAN0.setMode(MCP_NORMAL);

    // setup mask and filter
    CAN0.init_Mask(0, 0, 0xFFFFFFFF);       // check all ID bits, excludes everything except select IDs
    CAN0.init_Filt(0, 0, 0x100);            // RCB ID 1
    CAN0.init_Filt(1, 0, 0x101);            // RCB ID 2
    CAN0.init_Filt(2, 0, 0x102);            // Rinehart ID

    setup.canActive = true;
  }
  else {
    Serial.printf("CAN INIT [ FAILED ]\n");
  }
  // --------------------------------------------------------------------------- //


  // --- initialize ESP-NOW ---//
  // turn on wifi access point 
  if (esp_netif_init() == ESP_OK) {
    Serial.printf("TCP/IP INIT: [ SUCCESS ]\n");
    
    // init wifi and config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) == ESP_OK) {
      Serial.printf("WIFI INIT: [ SUCCESS ]\n");

      ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
      ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
      ESP_ERROR_CHECK(esp_wifi_start());
      ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    }
    else {
      Serial.printf("WIFI INIT: [ FAILED ]\n");
    }
  }
  else {
    Serial.printf("ESP TCP/IP STATUS: [ FAILED ]\n");
  }
  
  // init ESP-NOW service
  if (esp_now_init() == ESP_OK) {
    Serial.printf("ESP-NOW INIT [ SUCCESS ]\n");
  }
  else {
    Serial.printf("ESP-NOW INIT [ FAILED ]\n");
  }

  // get peer informtion about WCB
  memcpy(wcbInfo.peer_addr, wcbAddress, sizeof(wcbAddress));
  wcbInfo.channel = 0;
  wcbInfo.encrypt = false;

  // add WCB as a peer
  if (esp_now_add_peer(&wcbInfo) == ESP_OK) {
    Serial.printf("ESP-NOW CONNECTION [ SUCCESS ]\n");

    setup.wcbActive = true;
  }
  else {
    Serial.printf("ESP-NOW CONNECTION [ FAILED ]\n");
  }

  // attach message received ISR to the data received function
  esp_now_register_recv_cb(WCBDataReceived);
  // ------------------------------------------------------------------- //


  // --- initialize ARDAN ---//
  if (LoRa.begin(915E6)) {         // 915E6 is for use in North America 
    Serial.printf("ARDAN INIT [SUCCESSS ]\n");

    // init LoRa chip pins
    LoRa.setPins(ARDAN_SS_PIN, ARDAN_RST_PIN, ARDAN_DIO_PIN);

    // set the sync word so the car and monitoring station can communicate
    LoRa.setSyncWord(0xA1);         // the channel to be transmitting on (range: 0x00 - 0xFF)

    setup.ardanActive = true;
  }
  else { 
    Serial.printf("ARDAN INIT [ FAILED ]\n");
  }
  // -------------------------------------------------------------------------------------------- //


  // --- initialize timer interrupts --- //
  // timer 1 - Read Sensors 
  const esp_timer_create_args_t timer1_args = {
    .callback = &SensorCallback,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "Sensor Timer"
  };
  esp_timer_handle_t timer1;
  ESP_ERROR_CHECK(esp_timer_create(&timer1_args, &timer1));

  // timer 2 - CAN Update
  const esp_timer_create_args_t timer2_args = {
    .callback = &CANCallback,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "CAN Write Timer"
  };
  esp_timer_handle_t timer2;
  ESP_ERROR_CHECK(esp_timer_create(&timer2_args, &timer2));

  // timer 3 - ARDAN Update
  const esp_timer_create_args_t timer3_args = {
    .callback = &ARDANCallback,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "ARDAN Update Timer"
  };
  esp_timer_handle_t timer3;
  ESP_ERROR_CHECK(esp_timer_create(&timer3_args, &timer3));

  // timer 4 - WCB Update
  const esp_timer_create_args_t timer4_args = {
    .callback = &WCBCallback,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "WCB Update Timer"
  };
  esp_timer_handle_t timer4;
  ESP_ERROR_CHECK(esp_timer_create(&timer4_args, &timer4));

  // start timers
  if (setup.ioActive)
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer1, SENSOR_POLL_INTERVAL));
  if (setup.canActive)
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer2, CAN_WRITE_INTERVAL));
  if (setup.ardanActive)
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer3, ARDAN_UPDATE_INTERVAL));
  if (setup.wcbActive)
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer4, WCB_UPDATE_INTERVAL));

  Serial.printf("Timer 1 STATUS: %s\n", esp_timer_is_active(timer1) ? "RUNNING" : "FAILED");
  Serial.printf("Timer 2 STATUS: %s\n", esp_timer_is_active(timer2) ? "RUNNING" : "FAILED");
  Serial.printf("Timer 3 STATUS: %s\n", esp_timer_is_active(timer3) ? "RUNNING" : "FAILED");
  Serial.printf("Timer 4 STATUS: %s\n", esp_timer_is_active(timer4) ? "RUNNING" : "FAILED");
  // ----------------------------------------------------------------------------------------- //

  // --- End Setup Section in Serial Monitor --- //
  if (xTaskGetSchedulerState() == 2) {
    Serial.printf("\nScheduler Status: RUNNING");
  }
  else {
    Serial.printf("\nScheduler STATUS: FAILED\nHALTING OPERATIONS");
    while (1) {};
  }
  Serial.printf("\n\n|--- END SETUP ---|\n\n\n");
  // ---------------------------------------------------------------------------------------- //
}


// *** loop *** // 
void loop()
{
  // everything is on timers so nothing happens here! 
  vTaskDelay(1);    // prevent watchdog from getting upset

  // debugging
  if (debugger.debugEnabled) {
    PrintDebug();
  }
}


/**
 * @brief callback function for creating a new sensor poll task
 * 
 * @param args arguments to be passed to the task
 */
void SensorCallback(void* args) {
  static uint8_t ucParameterToPass;
  TaskHandle_t xHandle = NULL;
  xTaskCreate(ReadSensorsTask, "Poll-Senser-Data", TASK_STACK_SIZE, &ucParameterToPass, tskIDLE_PRIORITY, &xHandle);
}


/**
 * @brief callback function for creating a new CAN Update task
 * 
 * @param args arguments to be passed to the task
 */
void CANCallback(void* args) {
  static uint8_t ucParameterToPass;
  TaskHandle_t xHandle = NULL;
  xTaskCreate(UpdateCANTask, "CAN-Update", TASK_STACK_SIZE, &ucParameterToPass, tskIDLE_PRIORITY, &xHandle);
}

/**
 * @brief callback function for creating a new ARDAN Update task
 * 
 * @param args arguments to be passed to the task
 */
void ARDANCallback(void* args) {
  static uint8_t ucParameterToPass;
  TaskHandle_t xHandle = NULL;
  xTaskCreate(UpdateARDANTask, "ARDAN-Update", TASK_STACK_SIZE, &ucParameterToPass, tskIDLE_PRIORITY, &xHandle);
}


/**
 * @brief callback function for creating a new WCB Update task
 * 
 * @param args arguments to be passed to the task
 */
void WCBCallback(void* args) {
  static uint8_t ucParameterToPass;
  TaskHandle_t xHandle = NULL;
  xTaskCreate(UpdateWCBTask, "WCB-Update", TASK_STACK_SIZE, &ucParameterToPass, tskIDLE_PRIORITY, &xHandle);
}


/**
 * @brief reads sensors and updates car data 
 * 
 * @param pvParameters parameters passed to task
 */
void ReadSensorsTask(void* pvParameters)
{
  // turn off wifi for ADC channel 2 to function
  esp_wifi_stop();

  // get pedal positions
  float tmpPedal0 = adc1_get_raw(ADC1_GPIO32_CHANNEL);
  carData.inputs.pedal0 = MapValue(tmpPedal0, 0, 1024, 0, 255);   // starting min and max values must be found via testing!!!

  float tmpPedal1 = adc1_get_raw(PEDAL_1_PIN);
  carData.inputs.pedal1 = MapValue(tmpPedal1, 0, 1024, 0, 255);   // starting min and max values must be found via testing!!!

  // Calculate commanded torque
  GetCommandedTorque();

  // update wheel speed values
  carData.sensors.wheelSpeedFR = adc1_get_raw(WHEEL_SPEED_FR_SENSOR);
  carData.sensors.wheelSpeedFL = adc1_get_raw(WHEEL_HEIGHT_FL_SENSOR);

  // update wheel ride height values
  carData.sensors.wheelHeightFR = adc1_get_raw(WHEEL_HEIGHT_FR_SENSOR);
  carData.sensors.wheelHeightFL = adc1_get_raw(WHEEL_HEIGHT_FL_SENSOR);

  // update steering wheel position
  carData.sensors.steeringWheelAngle = adc1_get_raw(STEERING_WHEEL_POT);

  // buzzer logic
  if (carData.outputs.buzzerActive)
  {
    gpio_set_level((gpio_num_t)BUZZER_PIN, carData.outputs.buzzerActive);
    carData.outputs.buzzerCounter++;
    if (carData.outputs.buzzerCounter >= (2 * (100 / (SENSOR_POLL_INTERVAL / 10000))))    // get 2 seconds worth of interrupt counts
    {
      // update buzzer state and turn off the buzzer
      carData.outputs.buzzerActive = false;
      gpio_set_level((gpio_num_t)BUZZER_PIN, carData.outputs.buzzerActive);

      carData.outputs.buzzerCounter = 0;                        // reset buzzer count
      carData.drivingData.enableInverter = true;                // enable the inverter so that we can tell rinehart to turn inverter on
    }
  }

  // get brake positions
  float tmpBrake0 = adc1_get_raw(BRAKE_0_PIN);
  carData.inputs.brake0 = MapValue(tmpBrake0, 0, 1024, 0, 255);   // starting min and max values must be found via testing!!!

  float tmpBrake1 = adc1_get_raw(BRAKE_1_PIN);
  carData.inputs.brake1 = MapValue(tmpBrake1, 0, 1024, 0, 255);   // starting min and max values must be found via testing!!!

  // brake light logic 
  int brakeAverage = (carData.inputs.brake0 + carData.inputs.brake1) / 2;
  if (brakeAverage >= BRAKE_LIGHT_THRESHOLD) {
    carData.outputs.brakeLight = true;      // turn it on 
  }

  else {
    carData.outputs.brakeLight = false;     // turn it off
  }
  
  // debugging
  if (debugger.debugEnabled) {
    debugger.IO_data = carData;
    debugger.sensorTaskCount++;
  }

  // turn wifi back on to re-enable esp-now connection to wheel board
  esp_wifi_start();

  // end task
  vTaskDelete(NULL);
}


/**
 * @brief reads and writes to the CAN bus
 * 
 * @param pvParameters parameters passed to task
 */
void UpdateCANTask(void* pvParameters)
{
  // inits
  byte outgoingMessage[8];
  unsigned long receivingID = 0;
  byte messageLength = 0;
  byte receivingBuffer[8];

  // --- receive messages --- //
  // check for new messages in the CAN buffer
  if (CAN0.checkReceive() == CAN_MSGAVAIL) {
    CAN0.readMsgBuf(&receivingID, &messageLength, receivingBuffer);

    // filter for only the IDs we are interested in
    switch (receivingID)
    {
      // message from RCB: Sensor Data
      case 0x100:
      // do stuff with the data in the message
      break;

      // message from RCB: BMS and electrical data
      case 0x101:
      // do stuff with the data in the message
      break;

      default:
      // do nothing because we didn't get any messages of interest
      return;
      break;
    }
  }

  // --- send message --- // 
  // build message
  outgoingMessage[0] = 0x00;
  outgoingMessage[1] = 0x01;
  outgoingMessage[2] = 0x02;
  outgoingMessage[3] = 0x03;
  outgoingMessage[4] = 0x04;
  outgoingMessage[5] = 0x05;
  outgoingMessage[6] = 0x06;
  outgoingMessage[7] = 0x07;

  // send the message and get its sent status
  byte sentStatus = CAN0.sendMsgBuf(0x100, 0, sizeof(outgoingMessage), outgoingMessage);    // (sender address, STD CAN frame, size of message, message)

  // debugging
  if (debugger.debugEnabled) {
    debugger.CAN_sentStatus = sentStatus;
    for (int i = 0; i < 7; ++i) {
      debugger.CAN_outgoingMessage[i] = outgoingMessage[i];
    }
    debugger.canTaskCount++;
  }

  // end task
  vTaskDelete(NULL);
}


/**
 * @brief updates WCB with car data
 * 
 * @param pvParameters parameters passed to task
 */
void UpdateWCBTask(void* pvParameters)
{
  #if !TESTING
  // update battery & electrical data
  wcbData.batteryStatus.batteryChargeState = carData.batteryStatus.batteryChargeState;
  wcbData.batteryStatus.pack1Temp = carData.batteryStatus.pack1Temp;
  wcbData.batteryStatus.pack2Temp = carData.batteryStatus.pack2Temp;

  // update sensor data
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;

  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;
  wcbData.sensors.wheelSpeedFR = carData.sensors.wheelSpeedFR;

  // send message
  esp_err_t result = esp_now_send(wcbAddress, (uint8_t *) &wcbData, sizeof(wcbData));

  #endif

  // debugging 
  if (debugger.debugEnabled) {
    // debugger.WCB_updateResult = result;
    debugger.wcbTaskCount++;
  }




  // end task
  vTaskDelete(NULL);
}


/**
 * @brief updates the ARDAN 
 * 
 * @param pvParameters parameters passed to task
 */
void UpdateARDANTask(void* pvParameters)
{
  #if !TESTING
  // send LoRa update
  LoRa.beginPacket();
  LoRa.write((uint8_t *) &carData, sizeof(carData));
  LoRa.endPacket();
  #endif

  if (debugger.debugEnabled) {
    debugger.ardanTaskCount++;
  }

  // end task
  vTaskDelete(NULL);
}


/**
 * @brief Get the Commanded Torque from pedal values
 */
void GetCommandedTorque()
{
  // get the pedal average
  int pedalAverage = (carData.inputs.pedal0 + carData.inputs.pedal0) / 2;

  // drive mode logic
  switch (carData.drivingData.driveMode)
  {
    case SLOW:  // runs at 50% power
      carData.drivingData.commandedTorque = MapValue(pedalAverage, PEDAL_MIN, PEDAL_MAX, 0, MAX_TORQUE * 0.50);
    break;

    case ECO:   // runs at 75% power
      carData.drivingData.commandedTorque = MapValue(pedalAverage, PEDAL_MIN, PEDAL_MAX, 0, MAX_TORQUE * 0.75);
    break;

    case FAST:  // runs at 100% power
      carData.drivingData.commandedTorque = MapValue(pedalAverage, PEDAL_MIN, PEDAL_MAX, 0, MAX_TORQUE);
    break;
    
    // error state, set the mode to ECO
    default:
      // set the state to ECO for next time
      carData.drivingData.driveMode = ECO;

      // we don't want to send a torque if we are in an undefined state
      carData.drivingData.commandedTorque = 0;
    break;
  }

  // calculate rinehart command value
  carData.drivingData.commandedTorque = map(pedalAverage, PEDAL_DEADBAND, 255, 0, (MAX_TORQUE * 10));   // rinehart expects the value as 10x


  // --- safety checks --- //

  // for throttle safety, we will have a deadband
  if (carData.drivingData.commandedTorque <= TORQUE_DEADBAND)   // if less than 5% power is requested, just call it 0
  {
    carData.drivingData.commandedTorque = 0;
  }

  // check if ready to drive
  if (!carData.drivingData.readyToDrive) {
    carData.drivingData.commandedTorque = 0;    // if not ready to drive then block all torque
  }
} 


/**
 * @brief a callback function for when data is received from WCB
 * 
 * @param mac             the address of the WCB
 * @param incomingData    the structure of incoming data
 * @param length          size of the incoming data
 */
void WCBDataReceived(const uint8_t* mac, const uint8_t* incomingData, int length)
{
  // copy data to the wcbData struct 
  memcpy(&wcbData, incomingData, sizeof(wcbData));

  // get updated WCB data
  carData.drivingData.driveDirection = wcbData.drivingData.driveDirection;
  carData.drivingData.driveMode = wcbData.drivingData.driveMode;
  carData.inputs.coastRegen = wcbData.io.coastRegen;
  carData.inputs.brakeRegen = wcbData.io.brakeRegen;
  carData.outputs.buzzerActive = wcbData.io.buzzerActive;
  carData.drivingData.readyToDrive = wcbData.drivingData.readyToDrive;

  return;
}


/**
 * @brief scale a value inside a range of values to a new range of values
 * 
 * @param x             input value to be re-mapped
 * @param in_min        input value min of range
 * @param in_max        input value max of range
 * @param out_min       output value min of range
 * @param out_max       output value max of range
 * @return              the remapped value
 */
long MapValue(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}


/**
 * @brief some nice in-depth debugging for CAN
 * 
 */
void PrintCANDebug() {
  Serial.printf("\n--- START CAN DEBUG ---\n");

  // sent status
  Serial.printf("CAN Message Send Status: %s\n", debugger.CAN_sentStatus ? "Success" : "Failed");

  // message
  for (int i = 0; i < 7; ++i) {
    Serial.printf("CAN Raw Data Byte %d: %d\n\n", i, debugger.CAN_outgoingMessage[i]);
  }

  Serial.printf("\n--- END CAN DEBUG ---\n");
}


/**
 * @brief some nice in-depth debugging for WCB updates
 * 
 */
void PrintWCBDebug() {
  Serial.printf("\n--- START WCB DEBUG ---\n");

  // send status
  Serial.printf("WCB ESP-NOW Update: %s\n", debugger.WCB_updateResult ? "Success" : "Failed");


  // message
  // TODO: decide what to put here

  Serial.printf("\n--- END WCB DEBUG ---\n");
}


/**
 * @brief some nice in-depth debugging for I/O
 * 
 */
void PrintIODebug() {
  Serial.printf("\n--- START I/O DEBUG ---\n");

  // 

  Serial.printf("\n--- END I/O DEBUG ---\n");
}


/**
 * @brief manages toggle-able debug settings
 * 
 */
void PrintDebug() {
  // CAN
  if (debugger.CAN_debugEnabled) {
      PrintCANDebug();
  }

  // WCB
  if (debugger.WCB_debugEnabled) {
    PrintWCBDebug();
  }

  // I/O
  if (debugger.IO_debugEnabled) {
    PrintIODebug();
  }

  // Scheduler
  if (debugger.scheduler_debugEnable) {
    Serial.printf("sensor: %d | can: %d | wcb: %d | ardan: %d\n", debugger.sensorTaskCount, debugger.canTaskCount, debugger.wcbTaskCount, debugger.ardanTaskCount);
  }
}
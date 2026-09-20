/* An example code to show out Arduino / ESP32 interrupt usage

   Tested with ESP32 devkit V1, rotary encoder KY-040, and two push buttons that close circuit when pressed
   both encoder A and B pins must be connected to interrupt enabled pins on the ESP32, see here for more info:

   https://www.arduino.cc/reference/en/language/functions/external-interrupts/attachinterrupt/

   Rotary Encorder code Based on Oleg Mazurov's code for rotary encoder interrupt service routines for AVR micros
   here using interrupts: https://chome.nerpa.tech/mcu/rotary-encoder-interrupt-service-routine-for-avr-micros/

 This code was modified and extended by Antti L S Ketola in 2026, to demonstrate using queues with interrupts.

 The code is Free, but would like to get mentioned if you find useful recycling this.
 
 */

#include <time.h>
#include <LiquidCrystal_I2C.h>
// I2C pins for the LCD
#define LCD_SDA_PIN 21
#define LCD_SCL_PIN 22
// Define rotary encoder and switch pins
#define ENC_CLK_PIN 32
#define ENC_DT_PIN 33
#define ENC_SW_PIN 35
#define FIRST_SW_PIN 12
#define SECOND_SW_PIN 15
#define EVENT_WAIT_DELAY portMAX_DELAY

// We'll have only max 8 switches, so bit masks
#define ENC_SWITCH_MASK 0x1
#define FIRST_SWITCH_MASK 0x2
#define SECOND_SWITCH_MASK 0x4
#define SWITCH4_MASK 0x8

const uint16_t PAUSE_MSEC = 25000;
const uint16_t FAST_INCREMENT = 5;
const uint16_t SWITCH_DEBOUNCE_US = 60000;  // microsec

const unsigned char LCD_I2C_ADDR = 0x27;
const int LCD_SIZE_COLUMNS = 16;
const int LCD_SIZE_ROWS = 2;

typedef enum EventType_e {
  noEvent = 0x0,
  encChangeEvent = 0x1,  // HEX to emphasize that using bit masks
  switchEvent = 0x2,
  refreshTimerEvent = 0x4
};

typedef union EventData_u {
  int8_t encChange;
  uint8_t switches;
};

LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_SIZE_COLUMNS, LCD_SIZE_ROWS);

typedef struct UserEvent_t {
  EventType_e eventType;
  EventData_u data;
  unsigned long timestamp;
};

QueueHandle_t messageQueue;
hw_timer_t *refreshTimer = NULL;

unsigned long lastIncReadTime = esp_timer_get_time();
unsigned long lastDecReadTime = esp_timer_get_time();
unsigned long lastSwitchReadTime = esp_timer_get_time();

// demo vars
volatile int adjustable;
volatile int change;

void setup() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);  // SDA, SCL
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Hello Rotary");
  messageQueue = xQueueCreate(1, sizeof(UserEvent_t));

  // Set encoder pins and attach interrupts
  pinMode(ENC_CLK_PIN, INPUT);
  pinMode(ENC_DT_PIN, INPUT);
  pinMode(ENC_SW_PIN, INPUT);
  pinMode(FIRST_SW_PIN, INPUT_PULLUP);
  pinMode(SECOND_SW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), read_encoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT_PIN), read_encoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW_PIN), read_switches, FALLING);
  attachInterrupt(digitalPinToInterrupt(FIRST_SW_PIN), read_switches, FALLING);
  attachInterrupt(digitalPinToInterrupt(SECOND_SW_PIN), read_switches, FALLING);

  refreshTimer = timerBegin(1000000);
  timerAlarm(refreshTimer, 3000000, false, 3000000);
  timerAttachInterrupt(refreshTimer, &sendTimerEvent);

  // The demo variable
  adjustable = 0;
  change = 0;
  // Start the serial monitor to show output
  Serial.begin(115200);
}

void loop() {
  // static int lastCounter = 0;
  bool sw_enc = false;
  bool sw_1 = false;
  bool sw_2 = false;

  // If count has changed print the new value to serial
  UserEvent_t event;
  event.eventType = noEvent;
  event.data.switches = 0;
  if (xQueueReceive(messageQueue, &event, (TickType_t)EVENT_WAIT_DELAY)) {
    Serial.printf("Event type: 0x%x \n", event.eventType);
    switch (event.eventType) {
      case encChangeEvent:
        change = event.data.encChange;
        adjustable += change;
        break;
      case switchEvent:
        if (event.data.switches & ENC_SWITCH_MASK) {
          sw_enc = true;
        }
        if (event.data.switches & FIRST_SWITCH_MASK) {
          sw_1 = true;
        }
        if (event.data.switches & SECOND_SWITCH_MASK) {
          sw_2 = true;
        }
        break;
      // end case switchEvent
      case refreshTimerEvent:
        Serial.println("Refresh Timer event");

        // Reset and restart timer
        timerWrite(refreshTimer, 0);
        timerAlarm(refreshTimer, 3000000, false, 3000000);
        break;
    }
    lcd.setCursor(0, 0);
    lcd.printf("Rot: %d Adj:%d ", change, adjustable);
    lcd.setCursor(0, 1);
    lcd.printf("S1:%d SE:%d S2:%d", sw_1, sw_enc, sw_2);
  }
}

/*
 Rotary Encorder code Based on Oleg Mazurov's code for rotary encoder interrupt service routines for AVR micros
 here using interrupts: https://chome.nerpa.tech/mcu/rotary-encoder-interrupt-service-routine-for-avr-micros/

 It's a bit of smart bit manipulation.
*/
void read_encoder() {
  // Encoder interrupt routine for both pins. Updates counter
  // if they are valid and have rotated a full indent

  static uint8_t old_enc = 3;                                                                 // Lookup table index
  static int8_t encval = 0;                                                                   // Encoder value
  static const int8_t enc_states[] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };  // Lookup table
  static UserEvent_t isrEvent;

  isrEvent.eventType = encChangeEvent;

  old_enc <<= 2;  // Remember previous state

  if (digitalRead(ENC_CLK_PIN)) old_enc |= 0x02;  // Add current state of pin A
  if (digitalRead(ENC_DT_PIN)) old_enc |= 0x01;   // Add current state of pin B

  encval += enc_states[(old_enc & 0x0f)];

  // Update counter if encoder has rotated a full indent, that is at least 4 steps
  if (encval > 3) {  // Four steps forward
    int changevalue = 1;
    if ((esp_timer_get_time() - lastIncReadTime) < PAUSE_MSEC) {
      changevalue = FAST_INCREMENT * changevalue;
    }
    lastIncReadTime = esp_timer_get_time();
    // counter = counter + changevalue;              // Update counter
    encval = 0;
    isrEvent.data.encChange = changevalue;
    xQueueSendFromISR(messageQueue, &isrEvent, NULL);
  } else if (encval < -3) {  // Four steps backward
    int changevalue = -1;
    if ((esp_timer_get_time() - lastDecReadTime) < PAUSE_MSEC) {
      changevalue = FAST_INCREMENT * changevalue;
    }
    lastDecReadTime = esp_timer_get_time();
    //counter = counter + changevalue;              // Update counter
    encval = 0;
    isrEvent.data.encChange = changevalue;
    xQueueSendFromISR(messageQueue, &isrEvent, NULL);
  }
}

void read_switches() {
  if ((esp_timer_get_time() - lastSwitchReadTime) < SWITCH_DEBOUNCE_US) {
    return;
  }
  lastSwitchReadTime = esp_timer_get_time();

  static UserEvent_t isrEvent;
  isrEvent.eventType = noEvent;
  isrEvent.data.switches = 0;
  static uint8_t switches_status = 0;
  static uint8_t s1 = 1;
  s1 = digitalRead(ENC_SW_PIN);  // == LOW) * ENC_SWITCH_MASK;
  static uint8_t s2 = 1;
  s2 = digitalRead(FIRST_SW_PIN);  //== LOW)1 * FIRST_SWITCH_MASK; //
  static uint8_t s3 = 1;
  s3 = digitalRead(SECOND_SW_PIN);  //== LOW) * SECOND_SWITCH_MASK;

  switches_status = !s1 * ENC_SWITCH_MASK | !s2 * FIRST_SWITCH_MASK | !s3 * SECOND_SWITCH_MASK;
  if (switches_status) {
    isrEvent.eventType = switchEvent;
    isrEvent.data.switches = switches_status;
    xQueueSendFromISR(messageQueue, &isrEvent, NULL);
  }
}

void ARDUINO_ISR_ATTR sendTimerEvent() {
  static UserEvent_t isrEvent;
  isrEvent.eventType = refreshTimerEvent;
  isrEvent.data.switches = 0;
  xQueueSendFromISR(messageQueue, &isrEvent, NULL);
}

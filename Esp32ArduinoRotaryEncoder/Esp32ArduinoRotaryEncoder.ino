/* ESP32ArduinoRotaryEncoder 

  An example code to show out Arduino / ESP32 interrupt usage
  Tested with ESP32 devkit V1, rotary encoder KY-040, and two push buttons that close circuit when pressed
  both encoder A and B pins must be connected to interrupt enabled pins on the ESP32, see here for more info:

  https://www.arduino.cc/reference/en/language/functions/external-interrupts/attachinterrupt/

  The code in https://github.com/mo-thunderz/RotaryEncoder by mo_thunderz was viewed.

  The code mentioned was modified and extended by Antti L S Ketola in 2026, to demonstrate using queues with interrupts.

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

// Timing constants
const uint16_t PAUSE_USEC = 25000; // HW timer precision microseconds
const uint16_t FAST_INCREMENT = 5;
const uint16_t SWITCH_DEBOUNCE_US = 60000;  // microsec

// LCD constants
const unsigned char LCD_I2C_ADDR = 0x27;
const int LCD_SIZE_COLUMNS = 16;
const int LCD_SIZE_ROWS = 2;
// Get the LCD handle initialized
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_SIZE_COLUMNS, LCD_SIZE_ROWS);

// Enum for different event types
typedef enum EventType_e {
  noEvent = 0x0,
  encChangeEvent = 0x1,  // HEX to emphasize that using bit masks
  switchEvent = 0x2,
  refreshTimerEvent = 0x4,
  debugEvent = 0x80
};

// The union is for getting easily different views to the data carried by event data-
typedef union EventData_u {
  int8_t encChange;
  uint8_t switches;
  int8_t debug;
};

// An event is composed of event type, evet data and a time stamp
typedef struct UserEvent_t {
  EventType_e eventType;
  EventData_u data;
  unsigned long timestamp;
};

// Just one message queue for all the events in this example
QueueHandle_t messageQueue;
#define EVENT_QUEUE_LENGTH 3

// We're using a hardware timer, as we need just one for the example
hw_timer_t *refreshTimer = NULL;

// Initialize current microsecond values for times to start with
unsigned long lastIncReadTime = esp_timer_get_time();
unsigned long lastDecReadTime = esp_timer_get_time();
unsigned long lastSwitchReadTime = esp_timer_get_time();

// The global variables that are adjusted in this example ; 
// a single thread access through queued events protects the access
// volatile necessary, not letting the compiler optimize out the writes and reads
volatile int adjustable;
volatile int change;

void setup() {
  // Light up the LCD
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);  // SDA, SCL
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Hello Rotary");


  messageQueue = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(UserEvent_t));

  // Set encoder pins and attach interrupts
  pinMode(ENC_CLK_PIN, INPUT);
  pinMode(ENC_DT_PIN, INPUT);
  pinMode(ENC_SW_PIN, INPUT);
  pinMode(FIRST_SW_PIN, INPUT_PULLUP);
  pinMode(SECOND_SW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK_PIN), read_encoder_ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT_PIN), read_encoder_ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_SW_PIN), read_switches_ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(FIRST_SW_PIN), read_switches_ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(SECOND_SW_PIN), read_switches_ISR, FALLING);

  refreshTimer = timerBegin(1000000);
  timerAlarm(refreshTimer, 3000000, false, 3000000);
  timerAttachInterrupt(refreshTimer, &send_timer_event_ISR);

  // Init the variables adjusted in this example
  adjustable = 0;
  change = 0;

  // Start the serial monitor to show output in serial
  Serial.begin(115200);
}

// Arduino default main task
void loop() {

  bool sw_enc = false;
  bool sw_1 = false;
  bool sw_2 = false;

  UserEvent_t event;
  event.eventType = noEvent;
  event.data.switches = 0;
  if (xQueueReceive(messageQueue, &event, (TickType_t)EVENT_WAIT_DELAY)) {
    Serial.printf("Event type: 0x%x \n", event.eventType);
    switch (event.eventType) {
      case encChangeEvent:  // encode change event
        change = event.data.encChange;
        adjustable += change;
        break;
      case switchEvent: // switch press event
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
      case refreshTimerEvent: // HW timer triggered refresh timeout event
        Serial.println("Refresh Timer event");

        // Reset and restart timer
        timerWrite(refreshTimer, 0);
        timerAlarm(refreshTimer, 3000000, false, 3000000);
        break;
      case debugEvent:
        Serial.printf("debug:%02x:\n",(event.data.debug & 0x0f));
        break;
    }

    // Update LCD
    // Write upper row of the 16x2 display
    lcd.setCursor(0, 0);
    lcd.printf("Rot: %d Adj:%d ", change, adjustable);
    // Write lower row of the 16x2 display
    lcd.setCursor(0, 1);
    lcd.printf("S1:%d SE:%d S2:%d", sw_1, sw_enc, sw_2);
  }
}

/*
 Rotary Encorder code Based on Oleg Mazurov's code for rotary encoder interrupt service routines for AVR micros
 here using interrupts: https://chome.nerpa.tech/mcu/rotary-encoder-interrupt-service-routine-for-avr-micros/
 (inaccesible at the time of writing)
 It's a bit of smart bit manipulation.

  rotating clockwise old_enc goes
  0b1100, 0b0001, 0b0101, 0b0101 ...

  rotating counterclockwise old_enc goes
  0b1100, 0b0010, 0b1010, 0b1010 ...

*/

void read_encoder_ISR() {
  // Encoder interrupt routine for both pins. Updates counter
  // if they are valid and have rotated a full indent
  // message to be sent
  static UserEvent_t isrEvent;
  // set event type in the message
  isrEvent.eventType = encChangeEvent;

  static uint8_t old_enc = 3; // Lookup table index; n.b. static is initialized only once 
  static const int8_t enc_states[] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };  // Lookup table
  static int8_t encval = 0;   // Value representing change of encoder position

  old_enc <<= 2;  // Remember previous state

  if (digitalRead(ENC_CLK_PIN)) old_enc |= 0x02;  // Add current state of pin A
  if (digitalRead(ENC_DT_PIN)) old_enc |= 0x01;   // Add current state of pin B

  encval += enc_states[(old_enc & 0x0f)]; // keeps array index <= 0xF (15)
  //debug_ISR(&isrEvent, old_enc);
  
  // Update counter if encoder has rotated a full indent, that is at least 4 steps
  if (encval > 3) {  // Four steps forward
    int changevalue = 1;
    if ((esp_timer_get_time() - lastIncReadTime) < PAUSE_USEC) {
      changevalue = FAST_INCREMENT * changevalue;
    }
    lastIncReadTime = esp_timer_get_time();
    // counter = counter + changevalue;              // Update counter
    encval = 0;
      // set event type in the message
    isrEvent.eventType = encChangeEvent;
    isrEvent.data.encChange = changevalue;
    xQueueSendFromISR(messageQueue, &isrEvent, NULL);
  } else if (encval < -3) {  // Four steps backward
    int changevalue = -1;
    if ((esp_timer_get_time() - lastDecReadTime) < PAUSE_USEC) {
      changevalue = FAST_INCREMENT * changevalue;
    }
    lastDecReadTime = esp_timer_get_time();
    //counter = counter + changevalue;              // Update counter
    encval = 0;
      // set event type in the message
    isrEvent.eventType = encChangeEvent;
    isrEvent.data.encChange = changevalue;
    xQueueSendFromISR(messageQueue, &isrEvent, NULL);
  }
}

/*
  Interrupt routine: read_switches()
*/
void read_switches_ISR() {
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

/*
  Interrupt routine: send_timer_event_ISR()
*/
void ARDUINO_ISR_ATTR send_timer_event_ISR() {
  static UserEvent_t isrEvent;
  isrEvent.eventType = refreshTimerEvent;
  isrEvent.data.switches = 0;
  xQueueSendFromISR(messageQueue, &isrEvent, NULL);
}

// A way to send a debug message
inline void debug_ISR(UserEvent_t *eventPtr, int value) {
  EventType_e tmpEvent=eventPtr->eventType;
  eventPtr->eventType = debugEvent;
  eventPtr->data.debug = value;
  xQueueSendFromISR(messageQueue, eventPtr, NULL);
  eventPtr->eventType = tmpEvent;
}
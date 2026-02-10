// ====== SMART DESK COMPANION - PRESENCE + REMINDERS + EXERCISE ======
// Uses WS2812 NeoPixel Ring (12 LEDs)
// 
// Messages TO ProtoPie:
//   "userDetected"       - User sat down
//   "userLeft"           - User left the chair  
//   "sittingTooLong"     - Need to take a walk
//   "reminderCleared"    - Walk reminder cleared
//   "completed:taskId"   - Reminder task completed (e.g., completed:water)
//   "setDone"            - Exercise set completed (touch sensor pressed)
//   "nextSet"            - Ready for next set (button pressed during rest/overtime)
//
// Messages FROM ProtoPie:
//   "reminder:taskId:RRRGGGBBB" - Show reminder (e.g., reminder:water:000100255)
//   "cancel"                    - Cancel current reminder/exercise
//   "exercise:start"            - Start exercise set (green light)
//   "exercise:rest"             - Rest period (yellow light)
//   "exercise:overtime"         - Rest time exceeded (red light)
//   "exercise:complete"         - All sets done (celebration!)
//   "nightlight:on"             - Turn on soft night light
//   "nightlight:off"            - Turn off night light
//   "sleeplight:on:MINUTES"     - Start sleep light (dims over X minutes)
//   "sleeplight:off"            - Turn off sleep light

#include <Adafruit_NeoPixel.h>

// ====== Pin definitions ======
const int TRIG_PIN   = 7;
const int ECHO_PIN   = 8;
const int TOUCH_PIN  = 2;
const int BUZZER_PIN = 3;
const int BUTTON_PIN = 4;
const int LED_PIN    = 12;   // WS2812 data pin

// ====== NeoPixel Setup ======
const int NUM_LEDS = 12;
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// ====== Color definitions ======
const byte WARM_Y_R = 255, WARM_Y_G = 215, WARM_Y_B = 120;  // Warm Yellow (presence)
const byte RED_R = 255, RED_G = 0, RED_B = 0;                // Red (warning/overtime)
const byte GREEN_R = 0, GREEN_G = 255, GREEN_B = 100;        // Green (exercise active)
const byte YELLOW_R = 255, YELLOW_G = 180, YELLOW_B = 0;     // Yellow/Orange (rest)
const byte NIGHT_R = 255, NIGHT_G = 150, NIGHT_B = 0;        // Warm yellow (night light) - no blue!
const byte SLEEP_R = 255, SLEEP_G = 80, SLEEP_B = 0;         // Warm orange-red (sleep light)

// Store current LED color
byte curR = 0, curG = 0, curB = 0;

// ====== Timing Configuration ======
const unsigned long SITTING_TOO_LONG_MS = 5000UL;  // 5 seconds for testing
const unsigned long PRESENCE_TIMEOUT_MS = 2000UL;
const int PRESENCE_THRESHOLD_CM = 10;

// ====== State Machine ======
enum SystemState {
  STATE_IDLE,              // No one / light off
  STATE_USER_PRESENT,      // User sitting, warm yellow
  STATE_SITTING_TOO_LONG,  // Red warning
  STATE_WAITING_CLEAR,     // Waiting for touch to clear walk reminder
  STATE_REMINDER_ACTIVE,   // Reminder from app is active
  STATE_EXERCISE_SET,      // Doing an exercise set (green)
  STATE_EXERCISE_REST,     // Rest between sets (yellow)
  STATE_EXERCISE_OVERTIME, // Rest time exceeded (red)
  STATE_EXERCISE_COMPLETE, // All sets done (celebration)
  STATE_NIGHT_LIGHT,       // Night light mode
  STATE_SLEEP_LIGHT        // Sleep light mode (dims over time)
};

SystemState currentState = STATE_IDLE;
unsigned long userDetectedTime = 0;
unsigned long lastSeenTime = 0;
bool userCurrentlyDetected = false;
bool lastUserPresent = false;

// Reminder state
byte reminderR = 0, reminderG = 0, reminderB = 0;
bool reminderActive = false;
unsigned long reminderStartTime = 0;
String currentTaskId = "";

// Exercise state
unsigned long exerciseStateStart = 0;

// Sleep light state
unsigned long sleepLightStartTime = 0;
unsigned long sleepLightDurationMs = 0;

// Blink variables  
bool blinkState = false;
unsigned long lastBlinkChange = 0;

// Debounce tracking for touch and button
int lastTouchState = LOW;
int lastButtonState = LOW;

// ====== NeoPixel LED Functions ======
void setColor(int r, int g, int b) {
  curR = r; curG = g; curB = b;
  for (int i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void fadeToColor(byte targetR, byte targetG, byte targetB, unsigned long durationMs) {
  int steps = 50;
  unsigned long stepDelay = durationMs / steps;
  int startR = curR, startG = curG, startB = curB;
  
  for (int i = 1; i <= steps; i++) {
    byte r = startR + (targetR - startR) * i / steps;
    byte g = startG + (targetG - startG) * i / steps;
    byte b = startB + (targetB - startB) * i / steps;
    setColor(r, g, b);
    delay(stepDelay);
  }
  curR = targetR; curG = targetG; curB = targetB;
}

void blinkColor(byte r, byte g, byte b, unsigned long onMs, unsigned long offMs) {
  unsigned long now = millis();
  if (blinkState) {
    if (now - lastBlinkChange >= onMs) { blinkState = false; lastBlinkChange = now; }
  } else {
    if (now - lastBlinkChange >= offMs) { blinkState = true; lastBlinkChange = now; }
  }
  if (blinkState) { setColor(r, g, b); } else { setColor(0, 0, 0); }
}

// Smooth pulsing animation (fades brightness up and down)
void pulseColor(byte r, byte g, byte b, unsigned long periodMs) {
  unsigned long now = millis();
  // Use sine wave for smooth pulsing (0 to 1 to 0)
  float phase = (float)(now % periodMs) / (float)periodMs;  // 0.0 to 1.0
  float brightness = (sin(phase * 2.0 * 3.14159) + 1.0) / 2.0;  // 0.0 to 1.0
  
  // Apply brightness to color (minimum 20% so it never fully turns off)
  float minBrightness = 0.2;
  brightness = minBrightness + brightness * (1.0 - minBrightness);
  
  setColor((byte)(r * brightness), (byte)(g * brightness), (byte)(b * brightness));
}

// Fast celebration blink
void fastBlink(byte r, byte g, byte b) {
  blinkColor(r, g, b, 200, 200);
}

// Spinning animation for celebration
void spinAnimation(byte r, byte g, byte b, int spinCount) {
  for (int spin = 0; spin < spinCount; spin++) {
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.clear();
      // Light up 3 consecutive LEDs for a "comet" effect
      strip.setPixelColor(i, strip.Color(r, g, b));
      strip.setPixelColor((i + 1) % NUM_LEDS, strip.Color(r/2, g/2, b/2));
      strip.setPixelColor((i + 2) % NUM_LEDS, strip.Color(r/4, g/4, b/4));
      strip.show();
      delay(50);
    }
  }
}

// ====== Distance Sensor ======
long getDistance() {
  digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

bool isUserPresent() {
  long dist = getDistance();
  return (dist > 0 && dist < PRESENCE_THRESHOLD_CM);
}

// ====== Buzzer Functions ======
void playWarningBeep() {
  tone(BUZZER_PIN, 880); delay(150); noTone(BUZZER_PIN);
  delay(100);
  tone(BUZZER_PIN, 880); delay(150); noTone(BUZZER_PIN);
}

void playClearedMelody() {
  int notes[] = {523, 587, 659, 784};
  for (int i = 0; i < 4; i++) {
    tone(BUZZER_PIN, notes[i]); delay(150);
    noTone(BUZZER_PIN); delay(50);
  }
}

void playReminderBeep() {
  tone(BUZZER_PIN, 660); delay(100); noTone(BUZZER_PIN);
  delay(50);
  tone(BUZZER_PIN, 880); delay(150); noTone(BUZZER_PIN);
}

void playCompletedBeep() {
  tone(BUZZER_PIN, 523); delay(100); noTone(BUZZER_PIN);
  delay(50);
  tone(BUZZER_PIN, 659); delay(100); noTone(BUZZER_PIN);
  delay(50);
  tone(BUZZER_PIN, 784); delay(150); noTone(BUZZER_PIN);
}

// Exercise buzzer sounds
void playSetDoneBeep() {
  tone(BUZZER_PIN, 784); delay(100); noTone(BUZZER_PIN);
}

void playRestStartBeep() {
  tone(BUZZER_PIN, 659); delay(150); noTone(BUZZER_PIN);
  delay(50);
  tone(BUZZER_PIN, 523); delay(150); noTone(BUZZER_PIN);
}

void playOvertimeBeep() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 440); delay(100); noTone(BUZZER_PIN);
    delay(50);
  }
}

void playExerciseComplete() {
  // Happy victory melody with spinning LED animation
  int notes[] = {523, 659, 784, 1047, 784, 1047};
  int durations[] = {150, 150, 150, 300, 150, 400};
  for (int i = 0; i < 6; i++) {
    // Blink green LED with each note
    if (i % 2 == 0) {
      setColor(GREEN_R, GREEN_G, GREEN_B);
    } else {
      setColor(0, 0, 0);
    }
    tone(BUZZER_PIN, notes[i]); delay(durations[i]);
    noTone(BUZZER_PIN); delay(50);
  }
  // Spin animation at the end
  spinAnimation(GREEN_R, GREEN_G, GREEN_B, 2);
  setColor(GREEN_R, GREEN_G, GREEN_B);
}

// ====== Parse incoming commands ======
void processCommand(String cmd) {
  cmd.trim();
  
  // ===== REMINDER COMMANDS =====
  if (cmd.startsWith("reminder:")) {
    String params = cmd.substring(9);
    int colonIndex = params.indexOf(':');
    if (colonIndex > 0) {
      currentTaskId = params.substring(0, colonIndex);
      String colorStr = params.substring(colonIndex + 1);
      
      if (colorStr.length() >= 9) {
        reminderR = colorStr.substring(0, 3).toInt();
        reminderG = colorStr.substring(3, 6).toInt();
        reminderB = colorStr.substring(6, 9).toInt();
        
        lastTouchState = digitalRead(TOUCH_PIN);
        lastButtonState = digitalRead(BUTTON_PIN);
        
        currentState = STATE_REMINDER_ACTIVE;
        reminderActive = true;
        reminderStartTime = millis();
        blinkState = true;
        lastBlinkChange = millis();
        playReminderBeep();
      }
    }
  }
  
  // ===== EXERCISE COMMANDS =====
  else if (cmd == "exercise:start") {
    currentState = STATE_EXERCISE_SET;
    exerciseStateStart = millis();
    lastTouchState = digitalRead(TOUCH_PIN);
    lastButtonState = digitalRead(BUTTON_PIN);
    fadeToColor(GREEN_R, GREEN_G, GREEN_B, 300);
    playSetDoneBeep();
  }
  else if (cmd == "exercise:rest") {
    currentState = STATE_EXERCISE_REST;
    exerciseStateStart = millis();
    blinkState = true;
    lastBlinkChange = millis();
    lastTouchState = digitalRead(TOUCH_PIN);
    lastButtonState = digitalRead(BUTTON_PIN);
    fadeToColor(YELLOW_R, YELLOW_G, YELLOW_B, 300);
    playRestStartBeep();
  }
  else if (cmd == "exercise:overtime") {
    currentState = STATE_EXERCISE_OVERTIME;
    exerciseStateStart = millis();
    blinkState = true;
    lastBlinkChange = millis();
    lastTouchState = digitalRead(TOUCH_PIN);
    lastButtonState = digitalRead(BUTTON_PIN);
    fadeToColor(RED_R, RED_G, RED_B, 300);
    playOvertimeBeep();
  }
  else if (cmd == "exercise:complete") {
    currentState = STATE_EXERCISE_COMPLETE;
    exerciseStateStart = millis();
    blinkState = true;
    lastBlinkChange = millis();
    playExerciseComplete();
  }
  
  // ===== NIGHT LIGHT COMMANDS =====
  else if (cmd == "nightlight:on") {
    currentState = STATE_NIGHT_LIGHT;
    fadeToColor(NIGHT_R, NIGHT_G, NIGHT_B, 500);
  }
  else if (cmd == "nightlight:off") {
    currentState = STATE_IDLE;
    fadeToColor(0, 0, 0, 500);
  }
  
  // ===== SLEEP LIGHT COMMANDS =====
  else if (cmd.startsWith("sleeplight:on")) {
    // Format: sleeplight:on:MINUTES or just sleeplight:on (default 30 min)
    int minutes = 30;  // Default
    if (cmd.length() > 14 && cmd.charAt(13) == ':') {
      minutes = cmd.substring(14).toInt();
      if (minutes <= 0) minutes = 30;
    }
    
    currentState = STATE_SLEEP_LIGHT;
    sleepLightStartTime = millis();
    sleepLightDurationMs = (unsigned long)minutes * 60UL * 1000UL;
    setColor(SLEEP_R, SLEEP_G, SLEEP_B);  // Start at full brightness
  }
  else if (cmd == "sleeplight:off") {
    currentState = STATE_IDLE;
    fadeToColor(0, 0, 0, 500);
  }
  
  // ===== CANCEL COMMAND =====
  else if (cmd == "cancel") {
    reminderActive = false;
    
    if (userCurrentlyDetected) {
      currentState = STATE_USER_PRESENT;
      userDetectedTime = millis();
      setColor(WARM_Y_R, WARM_Y_G, WARM_Y_B);
    } else {
      currentState = STATE_IDLE;
      setColor(0, 0, 0);
    }
  }
}

// ====== Setup ======
void setup() {
  Serial.begin(9600);
  Serial.setTimeout(50);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  
  // Initialize NeoPixel
  strip.begin();
  strip.setBrightness(150);  // Set brightness (0-255)
  strip.show();  // Initialize all pixels to off
  
  setColor(0, 0, 0);
}

// ====== Main Loop ======
void loop() {
  unsigned long now = millis();
  int touchState = digitalRead(TOUCH_PIN);
  int buttonState = digitalRead(BUTTON_PIN);
  
  // ====== Read serial commands from ProtoPie ======
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    processCommand(command);
  }
  
  // ====== Update presence tracking (only in normal states) ======
  if (currentState == STATE_IDLE || currentState == STATE_USER_PRESENT || 
      currentState == STATE_SITTING_TOO_LONG || currentState == STATE_WAITING_CLEAR) {
    bool userPresent = isUserPresent();
    
    if (userPresent) {
      lastSeenTime = now;
      userCurrentlyDetected = true;
    } else {
      if (userCurrentlyDetected && (now - lastSeenTime >= PRESENCE_TIMEOUT_MS)) {
        userCurrentlyDetected = false;
      }
    }
  }

  // ====== State machine ======
  switch (currentState) {
    
    case STATE_IDLE:
      setColor(0, 0, 0);
      if (userCurrentlyDetected && !lastUserPresent) {
        currentState = STATE_USER_PRESENT;
        userDetectedTime = now;
        fadeToColor(WARM_Y_R, WARM_Y_G, WARM_Y_B, 1000);
        Serial.println("userDetected");
      }
      break;

    case STATE_USER_PRESENT:
      setColor(WARM_Y_R, WARM_Y_G, WARM_Y_B);
      
      if (now - userDetectedTime >= SITTING_TOO_LONG_MS) {
        currentState = STATE_SITTING_TOO_LONG;
        blinkState = true;
        lastBlinkChange = now;
        fadeToColor(RED_R, RED_G, RED_B, 500);
        playWarningBeep();
        Serial.println("sittingTooLong");
      }
      
      if (!userCurrentlyDetected) {
        currentState = STATE_IDLE;
        fadeToColor(0, 0, 0, 1000);
        Serial.println("userLeft");
      }
      break;

    case STATE_SITTING_TOO_LONG:
      blinkColor(RED_R, RED_G, RED_B, 1000, 500);
      
      if (!userCurrentlyDetected) {
        currentState = STATE_WAITING_CLEAR;
        setColor(0, 0, 0);
        Serial.println("userLeft");
      }
      break;

    case STATE_WAITING_CLEAR:
      blinkColor(RED_R / 3, 0, 0, 2000, 1000);
      
      if (touchState == HIGH) {
        currentState = STATE_IDLE;
        playClearedMelody();
        fadeToColor(0, 0, 0, 500);
        Serial.println("reminderCleared");
        delay(500);
      }
      break;
      
    case STATE_REMINDER_ACTIVE:
      blinkColor(reminderR, reminderG, reminderB, 2000, 500);
      
      if (millis() - reminderStartTime > 500) {
        bool touchPressed = (touchState == HIGH && lastTouchState == LOW);
        bool buttonPressed = (buttonState == LOW && lastButtonState == HIGH);
        
        if (touchPressed || buttonPressed) {
          reminderActive = false;
          playCompletedBeep();
          Serial.print("completed:");
          Serial.println(currentTaskId);
          
          if (userCurrentlyDetected) {
            currentState = STATE_USER_PRESENT;
            userDetectedTime = millis();
            fadeToColor(WARM_Y_R, WARM_Y_G, WARM_Y_B, 500);
          } else {
            currentState = STATE_IDLE;
            fadeToColor(0, 0, 0, 500);
          }
          delay(500);
        }
      }
      break;
      
    // ===== EXERCISE STATES =====
    
    case STATE_EXERCISE_SET:
      setColor(GREEN_R, GREEN_G, GREEN_B);
      
      if (now - exerciseStateStart > 500) {
        bool touchPressed = (touchState == HIGH && lastTouchState == LOW);
        
        if (touchPressed) {
          playSetDoneBeep();
          Serial.println("setDone");
        }
      }
      break;
      
    case STATE_EXERCISE_REST:
      pulseColor(YELLOW_R, YELLOW_G, YELLOW_B, 2000);  // Smooth 2-second pulse
      
      // Use millis() directly to avoid timing issues
      if (millis() - exerciseStateStart > 500) {
        bool buttonPressed = (buttonState == LOW && lastButtonState == HIGH);
        
        
        if (buttonPressed) {
          Serial.println("nextSet");
          delay(300);
        }
      }
      break;
      
    case STATE_EXERCISE_OVERTIME:
      blinkColor(RED_R, RED_G, RED_B, 300, 300);
      
      if (now - exerciseStateStart > 500) {
        bool buttonPressed = (buttonState == LOW && lastButtonState == HIGH);
        
        if (buttonPressed) {
          Serial.println("nextSet");
          delay(300);
        }
      }
      break;
      
    case STATE_EXERCISE_COMPLETE:
      fastBlink(GREEN_R, GREEN_G, GREEN_B);
      
      if (now - exerciseStateStart > 3000) {
        if (userCurrentlyDetected) {
          currentState = STATE_USER_PRESENT;
          userDetectedTime = millis();
          fadeToColor(WARM_Y_R, WARM_Y_G, WARM_Y_B, 500);
        } else {
          currentState = STATE_IDLE;
          fadeToColor(0, 0, 0, 500);
        }
      }
      break;
      
    case STATE_NIGHT_LIGHT:
      // Steady soft warm yellow - no action needed, color already set
      setColor(NIGHT_R, NIGHT_G, NIGHT_B);
      break;
      
    case STATE_SLEEP_LIGHT:
      {
        // Use millis() directly (not 'now') because startTime was set after 'now' was captured
        unsigned long currentTime = millis();
        unsigned long elapsed = currentTime - sleepLightStartTime;
        
        if (elapsed >= sleepLightDurationMs) {
          // Time's up - turn off completely
          currentState = STATE_IDLE;
          setColor(0, 0, 0);
        } else {
          // Calculate brightness (1.0 to 0.0)
          float progress = (float)elapsed / (float)sleepLightDurationMs;
          float brightness = 1.0 - progress;  // Starts at 1.0, ends at 0.0
          
          // Apply brightness to sleep color
          byte r = (byte)(SLEEP_R * brightness);
          byte g = (byte)(SLEEP_G * brightness);
          byte b = (byte)(SLEEP_B * brightness);
          setColor(r, g, b);
        }
      }
      break;
  }

  lastUserPresent = userCurrentlyDetected;
  lastTouchState = touchState;
  lastButtonState = buttonState;
  delay(50);
}

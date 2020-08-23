/*
  Stimulater for speed corrector.
  
  This skech allow the generation of a variable frequency square wave.
  
  Two input pins are used to drive the frequency up or down.

  Tested on Arduino Nano.
  
*/


#define BTN_PLUS_PIN 12
#define BTN_LESS_PIN 11
#define BTN_STOP_PIN 10
#define BTN_LOOP_PIN 9
#define OUT_PIN LED_BUILTIN

#define MAX_PERIOD 720000
#define MIN_PERIOD 3000

int32_t  period = 10000;
int32_t  delta = 1;
unsigned long lastTick = 0;
unsigned long lastRead = 0;
bool state = false;

void setup()
{
  pinMode(BTN_PLUS_PIN, INPUT_PULLUP);
  pinMode(BTN_LESS_PIN, INPUT_PULLUP);
  pinMode(BTN_STOP_PIN, INPUT_PULLUP);
  pinMode(BTN_LOOP_PIN, INPUT_PULLUP);
  pinMode(OUT_PIN, OUTPUT);
}

void loop()
{
//  unsigned long now = millis();
  unsigned long now = micros();
  if (now - lastRead > 250000)
  {
    if (not digitalRead(BTN_PLUS_PIN))
    {
      period += period / 50;
      if (period > MAX_PERIOD)
          period = MAX_PERIOD;
    }
    if (not digitalRead(BTN_LESS_PIN))
    {
      period -= period / 50;
      if (period < MIN_PERIOD)
          period = MIN_PERIOD;
    }
    lastRead = now;
  }

  if (not digitalRead(BTN_LOOP_PIN))
  {
    period = period + delta;
    if (period > MAX_PERIOD)
    {
      delta = -1;
      period = MAX_PERIOD;
    }
    else if (period < MIN_PERIOD)
    {
      delta = 1;
      period = MIN_PERIOD;
    }
    
  }
  
  if (not digitalRead(BTN_STOP_PIN))
  {
//    digitalWrite(OUT_PIN, false);
//    state = true;
  }
  else if (now > lastTick + period / 2)
  {
    digitalWrite(OUT_PIN, state);
      
    state = !state;
    lastTick = now;
  }
}

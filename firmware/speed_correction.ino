/*

  GTO/3000GT/Stealth Speedometer corrector
 
  Principle of operation :
   - The Arduino samples the speed signal that normaly goes to the speedometer.
   - The arduino generates an output signal for the speedometer. 
   - By default, the input signal is simply copied on the  output.
   - A push button allows to indicates the real speed of the car (as read with a GPS) : 50km/h (3 pulses), 100km/h (2 pulse), 140km/h (1 pulse)
   - A second push allows to indicates the speed read on the speedo (same as above)
   - The program store this 6 points and build a correction curve
  
  Input signal is generated by the speed sensor that short a transistor to ground to generates pulses.
  The reader must provide a 5V supply voltage with a forward diode to prevent return of 5V from other 
  reader modules.

  Measurement taken on a running 3000GT with GPS at steady speed:
  ---------------------------------------------------------------
    - @ 50km/h (GPS) :  35.46Hz
    - @ 99km/h (GPS) :  72.75Hz
    - @ 86km/h (GPS) :  62.48Hz
    - @143km/h (GPS) : 106.6Hz

  From this data, we can deduce the following:
    - Average pulse per km/h is ~0.73
    - Pulse per km is ~2683

  Input capture pre-scaler:
  -------------------------
  We expect a rezolution at least of 1 km/h @300km/h, this lead to the followoing :
    - @300km/h, F=219Hz, Period = 4.566ms
    - @299km/h, F=218.27Hz, Period = 4.581ms
    - delta Period for 1km/h @300km/h is 4.5µs
    - With a clock frequency of 16MHz, the pre-scaler need to be set to 64. This gives a resolution of 
      4µs
      (1 cycle @16Mhz is 62ns)
  Maximal measurable period (without overflow) :
    65536 * 4µs = 262144µs => 3.81Hz => 5.225km/h

  If measuring half periods : 
    3.81Hz/2 =>   1.905Hz => 2.55km/h
  
  
  Measuring input :
    - The input period is measured by the input capture function available on timer 1
    - Timer 1 is a 16 bits counter, thus allowing reading from 64µs up to 65535*4µs=262ms
    - Timer 1 is configured to divide the CPU frequency by 64

 Speed limiter remover:
 ---------------------
 In addition to speedometer correction, the correcter also provides a speed limiter remover function.
 The principle is simply to limit the speed input of the ECU to a maximum value configurable in the correcter.
 Once the maximum  
 
 */

/* Version
 *  
 *  0.6 : Added lowpass input filter to reduce effect of outlier input value when comming to a stop.
 *  0.5 : fixed output burst when the input period is greater than 0xffff
 *  
 */

#include <EEPROM.h>

#include "snxp.h"

#define TICK_VALUE  ((1/16000000.0) * 64.0)
#define PULSE_PER_KM  2683
#define PERIOD_50  ((1.0/((PULSE_PER_KM * 50.0) / 3600.0)) / (TICK_VALUE))
#define PERIOD_100  ((1.0/((PULSE_PER_KM * 100.0) / 3600.0)) / (TICK_VALUE))
#define PERIOD_150  ((1.0/((PULSE_PER_KM * 150.0) / 3600.0)) / (TICK_VALUE))

#define FREQ_50  (((PULSE_PER_KM * 50.0) / 3600.0))
#define FREQ_100  (((PULSE_PER_KM * 100.0) / 3600.0))
#define FREQ_150  (((PULSE_PER_KM * 150.0) / 3600.0))

// Min speed : 3km/h
#define MIN_SPEED 3
#define MIN_FREQ (((PULSE_PER_KM * MIN_SPEED) / 3600.0))

// Timeout : min speed -15%
#define TIMEOUT_TICK (((1.0/((PULSE_PER_KM * MIN_SPEED * 0.85) / 3600.0)) / (TICK_VALUE)) / 1.0)  

volatile uint16_t  gLastCounter = 0;
volatile uint32_t  gInputPeriod = 0;
volatile uint32_t  gOutputPeriod = 0;
volatile uint16_t  gOutputOverflow = 0;
volatile uint16_t  gOverflowCounter = 0;
volatile unsigned long gLastWriteLogTime = 0;
volatile bool gStopped = true;
volatile bool gCaptureLive = false;
volatile bool gDisplayCapture = false;
volatile bool gLastOutput = false;


float gLastInputFreq = 0.0;
String gInputString;
bool gStringComplete = false;

struct CorrectionItem
{
  uint16_t  inputPeriod;
  uint16_t  outputPeriod;
};

struct CorrectionFreq
{
  float inputFreq;
  float outputFreq;
};

// Speed correction table
// Each entry correspond to a point on the frequency correction curve
// First iten is always 0 => 0 to define the base of the curve.
CorrectionFreq gFreqTable[] = 
{
  {0.0, 0.0},
  {FREQ_50, FREQ_50},
  {FREQ_100, FREQ_100},
  {FREQ_150, FREQ_150}
};


void saveConf()
{
  int ee = 0;

  EEPROM.write(ee++, 'S');
  EEPROM.write(ee++, 'N');
  EEPROM.write(ee++, 'X');
  const byte* p = (const byte*)(const void*)gFreqTable;
  for (int i=0; i<sizeof(gFreqTable); ++i)
  {
        EEPROM.write(ee++, *p++);
  }
}

void loadConf()
{
  int ee = 0;
  char s, n, x;
  s = EEPROM.read(ee++);
  n = EEPROM.read(ee++);
  x = EEPROM.read(ee++);

  if (s != 'S' or n != 'N' or x != 'X')
  {
    Serial.println("No configuration found");
    return;
  }

  byte* p = (byte*)(void*)gFreqTable;
  for (int i=0; i<sizeof(gFreqTable); ++i)
  {
        *p++ = EEPROM.read(ee++);
  }
  Serial.println("Configuration loaded");
}

// Input capture interrupt routine for Speed input
// Input capture side is inverted each time to capture both raising and falling edge.
ISR (TIMER1_CAPT_vect)
{
  uint8_t oldSREG = SREG;
  cli();
  uint16_t tc1 = ICR1;

  // TEST TEST TEST
//    gLastOutput = not gLastOutput;
//    digitalWrite(13, gLastOutput);
//	
  // TEST TEST TEST

  // invert input capture edge
  TCCR1B ^= (1<<ICES1);
	
  if (gCaptureLive)
  {
    // compute duration between event
    gInputPeriod = (int32_t)tc1 - (int32_t)gLastCounter;
    // add overflow count
    gInputPeriod += int32_t(gOverflowCounter) * 0x10000;
    gOverflowCounter = 0;
    
    // Overflow interrupt is pending. If we read a timer less than
    // 0xffff, this mean that we read a timer after the overflow, therefore, we need to 
    // account for an additional overflow value.
    if ((TIFR1 & _BV(TOV1)) && (tc1 < 0xffff))
    {
      gInputPeriod += 0x10000;
      // Set overflow to -1 because this overflow has already been accounted for, and once
      // the overflow interrupt will be triggered, it will add 1, thus leading to a value 
      // of zero.
      gOverflowCounter = -1;
    }
  }
  else
  {
    // capture is not live, reset the overflow counter
    gOverflowCounter = 0;
    // Overflow interrupt is pending. If we read a timer less than
    // 0xffff, this mean that we read a timer after the overflow, therefore, we need to 
    // account for an additional overflow value.
    if ((TIFR1 & _BV(TOV0)) && (tc1 < 0xffff))
    {
      // Set overflow to -1 because this overflow has already been accounted for, and once
      // the overflow interrupt will be triggered, it will add 1, thus leading to a value 
      // of zero.
      gOverflowCounter = -1;
    }

    // immediately swap the output
    gLastOutput = not gLastOutput;
    digitalWrite(13, gLastOutput);
  }
  gLastCounter = tc1;
  gCaptureLive = true;
  ICR1 = 0;

  SREG = oldSREG;

//  Serial.print("Input capture : ");
//  Serial.println(gInputPeriod);

  gDisplayCapture = true;
}

bool ovf = false;
// Timer 1 overflow interrupt
ISR (TIMER1_OVF_vect)
{
  uint8_t oldSREG = SREG;
  cli();
  ++gOverflowCounter;

   digitalWrite(12, ovf);
   ovf = !ovf;

  SREG = oldSREG;

//  Serial.print("Overflow ! = ");
//  Serial.println(gOverflowCounter);
}

ISR (TIMER1_COMPA_vect)
{
  uint8_t oldSREG = SREG;
  cli();

//  uint16_t counter = TCNT1;

  // check timeout (input freq is too slow)
  if (not gStopped)
  {
    gLastOutput = not gLastOutput;
    digitalWrite(13, gLastOutput);

    // program next event
    OCR1A = OCR1A + gOutputPeriod;
  }

  SREG = oldSREG;
}

void setup()
{
  // Input Capture setup
  // ICNC1: Enable Input Capture Noise Canceler
  // ICES1: =1 for trigger on rising edge
  // CS10+CS11: =1 set prescaler to 64x system clock (F_CPU)
  TCCR1A = 0;
  TCCR1B = (1<<ICNC1) | (1<<ICES1) | (1<<CS10) | (1<<CS11);
  TCCR1C = 0;
   
  // initialize to catch Falling Edge
  TCCR1B &= ~(1<<ICES1); 
  TIFR1 |= (1<<ICF1); 
   
  // Interrupt setup
  // ICIE1: Input capture
  // TOIE1: Timer1 overflow
  TIFR1 = (1<<ICF1) | (1<<TOV1);	// clear pending interrupts
  TIMSK1 = (1<<ICIE1) | (1<<TOIE1);	// enable interupts

  // Set up the Input Capture pin, ICP1, Arduino Uno pin 8
  pinMode(8, INPUT_PULLUP);
  //digitalWrite(8, 0);	// floating may have 60 Hz noise on it.
  //digitalWrite(8, 1); // or enable the pullup

  pinMode(13, OUTPUT);
  pinMode(12, OUTPUT);

  // activate output compare match interrupt
  TIMSK1 |= 1<<OCIE1A;

  gInputString.reserve(50);

  Serial.begin(57600);

  // load saved configuration
  loadConf();

  Serial.print("Init info : TICK = ");
  Serial.print(TICK_VALUE*1000*1000);
  Serial.print("µs, Timeout Tick = ");
  Serial.print(TIMEOUT_TICK);

  Serial.print(", F50 = ");
  Serial.print(FREQ_50);
  Serial.print("Hz F100 = ");
  Serial.print(FREQ_100);
  Serial.print("Hz F150 = ");
  Serial.print(FREQ_150);
  Serial.print("Hz, TIMEOUT=");
  Serial.println(TIMEOUT_TICK);

  Serial.println("Setup done");

}

void serialEvent()
{
//  Serial.println("Serial event !");
  while (Serial.available()) {
    // get the new byte:
    char inChar = (char)Serial.read();
    // if the incoming character is a newline, set a flag
    // so the main loop can do something about it:
    if (inChar == '\n') {
      gStringComplete = true;
      Serial.print("Line complete  : ");
      Serial.println(gInputString);
    }
    else
    {
      // add it to the inputString:
      gInputString += inChar;
    }
  }
}

int32_t myParseInt(const char*& it, const char* last)
{
  int32_t ret = 0;
  
  // skip any white space before
  while (it != last and *it == ' ')
    ++it;

  while( it != last)
  {
    char c = *it++;
    if (c >= '0' and c <= '9')
    {
      ret = (ret * 10) + (c - '0');
    }
    else
      break;
  }

  // skip any white space after
  while (it != last and *it == ' ')
    ++it;

  return ret;
}

int32_t gLoopPerSec = 0;

enum State
{
  OPEN_CER_RAISE,
  OPEN_CER_LOWER,
  NORMAL
};

State gState = NORMAL;
unsigned int gLastUpdate = 0u;


class LockInterrupt
{
  uint8_t mOldSreg;
public:
  LockInterrupt()
  {
    // Save State register
    mOldSreg = SREG;
    // Clear interrupt bit
    cli();
  }

  ~LockInterrupt()
  {
    // Restaure state register
    SREG = mOldSreg;
  }
};

void loop()
{
  unsigned long now = millis();

  if (gState == OPEN_CER_RAISE)
  {
    double freq = 3 + 300.0 * (now / 1000.0);
    uint32_t period = 1.0 / freq / TICK_VALUE;
//    uint8_t oldSREG = SREG;
    {
      LockInterrupt locker;
//      cli();
      gOutputPeriod = period;
//      SREG = oldSREG;
    }
    if( gStopped == true)
    {
          OCR1A = TCNT1 + (gOutputPeriod >> 1);
    }
    gStopped = false;
    gCaptureLive = true;
    if (now > 1000)
    {
      gState = OPEN_CER_LOWER;

      Serial.print(now);
      Serial.println(" End RAISE");
    }
    return;
  }
  else if (gState == OPEN_CER_LOWER)
  {
    double freq = 3 + 300.0 * ((1000-(now - 1000)) / 1000.0);
    uint32_t period = 1.0 / freq / TICK_VALUE;
    uint8_t oldSREG = SREG;
    cli();
    gOutputPeriod = period;
    SREG = oldSREG;
    if (now > 2000)
    {
      gState = NORMAL;
      gStopped = true;
      gCaptureLive = false;

      Serial.print(now);
      Serial.println(" End LOWER");
    }
    return;
  }

  ++gLoopPerSec;

  uint32_t period;
  uint32_t lastCapture;
  uint32_t overFlow; 
  uint32_t tcnt1;
  float inFreq;
  uint8_t index;
  uint32_t outputPeriod;
  float outFreq = 0.0f;


  // every 1/10th of seconds
  if (now - gLastUpdate > 100)
  {
    gLastUpdate = now; 
    // read input
    uint8_t oldSREG = SREG;
    cli();
    period = gInputPeriod;
    lastCapture = gLastCounter;
    overFlow = gOverflowCounter; 
    tcnt1 = TCNT1;
  //  bool beforeOverflow = false;
  //  if ((TIFR1 & _BV(TOV1)) && (tcnt1 < 0xffff))
  //    beforeOverflow = true;
    SREG = oldSREG;
  
  //  uint32_t currentPeriod = int32_t(tcnt1) - int32_t(lastCapture) + overFlow * 0x10000;
  
    // check for stop condition
  //  if (not gStopped and currentPeriod > int32_t(TIMEOUT_TICK))
    if (overFlow >= 2 or period >= 0x10000)
    {
      uint8_t oldSREG = SREG;
      cli();
      // input is stalled, stop output
      gOutputPeriod = 0;
      gInputPeriod = 0;
  //    digitalWrite(13, false);
  //    gLastOutput = false;
      gStopped = true;
      gCaptureLive = false;
  
      SREG = oldSREG;
      period = 0;
  
      Serial.print(now);
      Serial.print(" STOP detected with period of ");
      Serial.print(period);
      Serial.print(", last capture = ");
      Serial.print(lastCapture);
      Serial.print(", tcnt1 = ");
      Serial.print(tcnt1);
      Serial.print(", Overflow = ");
      Serial.println(overFlow);
    }
  
    uint32_t baseInputPeriod = period;
  
  //  // Automatically decrease output freq if no input is seen
  //  if (not gStopped and period != 0 and currentPeriod > period)
  //  {
  //    period = min(0xfff1, currentPeriod);
  //  }
    // compute input freq based on input period
  
    if (period != 0)
      inFreq = 1.0 / (period * 2 * TICK_VALUE);
    else
      inFreq = 0.0;
  
	// Filter input freq to reject outlier
	// 0..100km/h in 3s => 0->33km/h /s => delta=24Hz/s=> 2.4Hz per update
	// => For filtering, we accept only a variation of 10Hz per update
	
	const float MAX_VAR_PER_CYCLE = 2.5f;
	float deltaFreq = inFreq - gLastInputFreq;
	if (fabs(deltaFreq) > MAX_VAR_PER_CYCLE)
	{
		// input freq variation if too high ! Limit variation to 10Hz
		if (deltaFreq > 0.0)
			inFreq = gLastInputFreq + MAX_VAR_PER_CYCLE;
		else
			inFreq = gLastInputFreq - MAX_VAR_PER_CYCLE;
	}
	                                            
	// store last input freq for next loop
	gLastInputFreq = inFreq;
  
    // compute output period 
  
    // select the correction range
    if (inFreq < gFreqTable[1].inputFreq)
    {
      // Input speed is less than 50km/h   
      index = 1;
    }
    else if (inFreq < gFreqTable[2].inputFreq)
    {
      index = 2;
    }
    else
    {
      index = 3;
    }
  
  
    // Once index is determined,  
    // Compute out freq
    if (period != 0)
    {
      float di = gFreqTable[index].inputFreq - gFreqTable[index-1].inputFreq;
      float doo = gFreqTable[index].outputFreq - gFreqTable[index-1].outputFreq;
      outFreq = (inFreq - gFreqTable[index-1].inputFreq) / di * doo;
      outFreq += gFreqTable[index-1].outputFreq;
  
      // And convert to output period
      outputPeriod = (1.0 / outFreq) / TICK_VALUE;
    }
    else
    {
      outputPeriod = 0;
    }
  
    // clamp output period to 0xffff
  //  if (outputPeriod > 0x1ffff)
  //  {
  //    outputPeriod = 0x1ffff;
  //    Serial.println("**** MAXED ****");
  //  }
  
  //  if (gStopped and outputPeriod != 0)
  //  {
  //    Serial.print(now);
  //    Serial.print(" RESTART with period ");
  //    Serial.println(outputPeriod);
  //  }
  
    uint32_t previousOutputPeriod;
	previousOutputPeriod = gOutputPeriod;
	{
		oldSREG = SREG;
		cli();
		// update output period param
		
		gOutputPeriod = outputPeriod >> 1;
		// need to program first timer ?
		if (gStopped and outputPeriod != 0)
		{
		  OCR1A = TCNT1 + (outputPeriod >> 1);
		  gStopped = false;
		  // and swap output immediatel
		// TEST : COMMENTED
			//gLastOutput = not gLastOutput;
			//digitalWrite(13, gLastOutput);
		// TEST : COMMENTED
		}
		SREG = oldSREG;
	}
	
//    if (gDisplayCapture)
//    {
//      Serial.print(100000.0/period);
//      Serial.print(", ");
//      Serial.println(100000.0/outputPeriod);
//      gDisplayCapture = false;
//    }
  
    if (previousOutputPeriod != (outputPeriod >> 1) and false)
    {
      Serial.print("Output changed from ");
      Serial.print(previousOutputPeriod<<1);
      Serial.print(" to ");
      Serial.print(outputPeriod);
      Serial.print(", base input = ");
      Serial.print(baseInputPeriod);
      Serial.print(", input = ");
      Serial.println(period);
    }
  }
  // process input command
  if (gStringComplete)
  {
    Serial.println("Line received");

    if (gInputString == "SAVE")
    {
      Serial.println("Saving conf");
      saveConf();
    }
    else
    {
      // configure command :
      // Format is as follow:
      // INDEX IN_FREQ*1000 OUT_FREQ*1000
      //  with INDEX [1..3]
      // e.g :
      //   1 27000 29000
      //    => Set the correction point 1 to convert 27.000Hz to 29.000Hz
      
      const char* it = gInputString.c_str();
      const char* last = it + gInputString.length();
     
      int index = myParseInt(it, last);
  
      bool ok = it != last;
  
      if (not ok)
        Serial.println("Failed to parse index");
  
      float inVal = myParseInt(it, last) / 1000.0f;
      ok &= it != last;
  
      if (not ok)
        Serial.println("Failed to parse input");
      
      float outVal = myParseInt(it, last) / 1000.0f;
      ok &= it == last;
    
      if (not ok)
        Serial.println("Failed to parse output");
  
      ok &= index < 4;
  
      if (ok)
      {
        // apply the new value for the correction table
        gFreqTable[index].inputFreq = inVal;
        gFreqTable[index].outputFreq = outVal;
  
        Serial.print("Updated value : ");
        Serial.print(index);
        Serial.print(" : ");
        Serial.print(inVal);
        Serial.print(" - ");
        Serial.println(outVal);
      }
    }
    gInputString = "";
    gStringComplete = false;
  }
  
  // debug output
  if (now - gLastWriteLogTime > 1000 and false)
  {
    Serial.print("Period=");
    Serial.print(period);
    Serial.print(", last capture = ");
    Serial.print(lastCapture);
    Serial.print(", tcnt1 = ");
    Serial.print(tcnt1);
    Serial.print(", Overflow = ");
    Serial.println(overFlow);
    
    Serial.print("Speed = ");
    Serial.print(inFreq * 3600.0f / 2683.0f);
    Serial.print("km/h ");
    Serial.print("Index : ");
    Serial.print(index);
    Serial.print(" : ");
    Serial.print(period);
    Serial.print(" - ");
    Serial.print(outputPeriod);
    Serial.print(" InF = ");
    Serial.print(inFreq);
    Serial.print("Hz, OutF = ");
    Serial.print(outFreq);
    Serial.print("Hz, ");
    Serial.print(gLoopPerSec);
    Serial.print(" loop/s\n");
    

    for (int i = 0; i<4; ++i)
    {
      Serial.print(i);
      Serial.print(" : ");
      Serial.print(gFreqTable[i].inputFreq);
      Serial.print(" - ");
      Serial.print(gFreqTable[i].outputFreq);
      Serial.print("; ");
    }
    Serial.println("");
      
    gLastWriteLogTime = now;
    gLoopPerSec = 0;
  }
}


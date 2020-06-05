/*
Copyright (c) 2020, Joris Robijn
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following condition is met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <Arduino.h>
#include <pins_arduino.h>
#include <LiquidCrystal.h>
#include "RotaryEncoder.h"
#include "VerticalGraph.h"
#include "RespirationAnalysis.h"
#include "CircularBuffer.h"
#include "ADCReader.h"
#include "KeyScanner.h"
#include "MainScreen.h"
#include "MenuScreen.h"
#include "MemoryFree.h"


/*
Timer output  Arduino output  Chip pin  Pin name
OC0A  6 12  PD6
OC0B  5 11  PD5
OC1A  9 15  PB1
OC1B  10  16  PB2
OC2A  11  17  PB3
OC2B  3 5 PD3

https://etechnophiles.com/change-frequency-pwm-pins-arduino-uno/
TCCR0B = TCCR0B & B11111000 | B00000011; // for PWM frequency of 976.56 Hz (The DEFAULT) Needed for millis to work correctly!
TCCR1B = TCCR1B & B11111000 | B00000011; // for PWM frequency of 490.20 Hz (The DEFAULT)
TCCR2B = TCCR2B & B11111000 | B00000100; // for PWM frequency of 490.20 Hz (The DEFAULT)
*/

#define DOUT_LCD_EN 9     // Usually pin 6 on LCD
#define DOUT_LCD_RS 8     // Usually pin 4 on LCD
#define DOUT_LCD_DB4 7    // Usually pin 11 on LCD
#define DOUT_LCD_DB5 6    // Usually pin 12 on LCD
#define DOUT_LCD_DB6 5    // Usually pin 13 on LCD
#define DOUT_LCD_DB7 4    // Usually pin 14 on LCD
#define DIN_ROTENC_A 2    // Needs to be on this pin because it needs interrupt on change
#define DIN_ROTENC_B 3    // Needs to be on this pin because it needs interrupt on change
#define DIN_KEYS_X0 12    // This pin reads the key status. It is internally pulled up.
#define DOUT_KEYS_Y0 7    // These 5 key pins are shared with the LCD
#define DOUT_KEYS_Y1 6    // On all of them, a diode needs to be connected with 
#define DOUT_KEYS_Y2 5    // the cathode towards the pin, and the anode towards
#define DOUT_KEYS_Y3 4    // the switch. The other side of the switch should be connected to the KEYS_X0 pin.
#define DOUT_KEYS_Y4 8    // Finally, the Enter switch (on the rotary encoder) is directly connected from KEY_X0 to ground.
#define DOUT_MOTOR_PWM 10 // This PWM pin is output B on Timer 1. From its two outputs, only this output is usable if we want to program the top value ourselves.
#define DIN_MOTOR_PARK 11
#define DOUT_BEEPER 13    // A beeper that will beep if it gets voltage. A GND pin is right next to pin 13.

#define AIN_VSUPPLY A0    // Detects the supply voltage with a resistor divider network of e.g. 22k and 3k3. 
                          // Values are not critical, but you should set the correct dividing in the #define further below.
#define AIN_IMOTOR A1     // Detects the current through the motor from supply, with a shunt resistor on ground side of 0.05 ohm. 
#define AIN_LUNGPRES A2   // Connected to MPX(V)5010 for detecting pressure.
#define AIN_LUNGFLOW A3   // Connected to MPX(V)7002 for detecting differential pressure to detect flow.
                          // If your Arduino is 3V3, you can best choose the 3V3 version of the sensors. They have a 3 in their prefix.

// Free pins: A4 (=SDA) and A5 (=SCK)

LiquidCrystal lcd( DOUT_LCD_RS, DOUT_LCD_EN, DOUT_LCD_DB4, DOUT_LCD_DB5, DOUT_LCD_DB6, DOUT_LCD_DB7 );
RotaryEncoder rotEnc( DIN_ROTENC_A, DIN_ROTENC_B ); 
VerticalGraph vgraph;
CircularBuffer circBuf;
RespirationAnalysis an;
byte ADCPins[4] = { AIN_VSUPPLY, AIN_IMOTOR, AIN_LUNGPRES, AIN_LUNGFLOW };
ADCReader ADCR( 4, ADCPins );

#define VSUPPLYRATIO ((10000.0/3300.0)+1) // 10k and 3k3 resistor
#define IMOTORCONDUCTANCE (1/0.05)

uint8_t X_pinList[] = { DIN_KEYS_X0 };
uint8_t Y_pinList[] = { DOUT_KEYS_Y0, DOUT_KEYS_Y1, DOUT_KEYS_Y2, DOUT_KEYS_Y3, DOUT_KEYS_Y4 };
Key directKeyList[] = { KEY_ENTER};
Key matrixKeyList[] = { KEY_0, KEY_1, KEY_2, KEY_3, KEY_4 };
KeyScanner keySc( sizeof(X_pinList), sizeof(Y_pinList), X_pinList, Y_pinList, directKeyList, matrixKeyList );


void setup() {
  // put your setup code here, to run once:
  Serial.begin( 115200 );
  if( circBuf.init( 1, 80 ) != 0 ) {
    Serial.println( "Buffer allocation failed" );
  }
  ADCR.init( 125 ); // This sets averaging. On Arduino Uno (on 16 MHz), you will get 250 samples per 13 seconds (sorry I couldn't get a nicer fraction). That's one every 52 ms exactly.
  keySc.init();
  lcd.begin( 20, 4 );
  lcd.noAutoscroll();
  rotEnc.init();
  vgraph.prepare();
  switchScreen( mainScreen );

  analogWrite( DOUT_MOTOR_PWM, 0 );
  OCR1A = 249;        // Program top to 249 i.o. 255
  TCCR1B = B00001001; // for PWM frequency of 32kHz, and PWM mode 5 (WGM22=1)

  pinMode( DIN_MOTOR_PARK, INPUT_PULLUP );

  setDefaultSettings();

  Serial.print( freeMemory() );
  Serial.println( F(" bytes free") );
}

void loop() {

  float VA[4], Vsup, Imot, p, Q, Vmot;

  // The timing of this loop has to be based on the availability of data. Each cycle should not take longer than the sample time (which actually is an averaged sample).
  while( !ADCR.areSamplesReady() ) // this also works on carry-over
    delay( 1 );

  long t = millis();

  int a = ADCR.getAveraging();
  for( byte ch=0; ch<4; ch++ ) {
    VA[ch] = 0.004883 * ((float)ADCR.getSample( ch ) / a);
    //Serial.print( " VA[" );
    //Serial.print( ch );
    //Serial.print( "]=" );
    //Serial.print( VA[ch] );
  }
  Serial.println();
  Vsup = VA[0] * VSUPPLYRATIO;
  Imot = VA[1] * IMOTORCONDUCTANCE;
  p    = (VA[2] - 0.2) * 22.66;
  Q    = VA[3]; // TODO: determine conversion formula
  Vmot = OCR1B * Vsup / 250;

  ADCR.signalSamplesRead();

  Serial.print( " Vsup=" );
  Serial.print( Vsup );
  Serial.print( " Imot=" );
  Serial.print( Imot );
  Serial.print( " p=" );
  Serial.print( p );
  Serial.print( " Q=" );
  Serial.print( Q );
  Serial.println();

  float row[1];
  //row[0] = t;
  row[0] = p;
  int ar = circBuf.appendRow( row );

  measValues[M_p] = p;
  //flow = values[M_Q];
  measValues[M_Vsup] = Vsup;
  measValues[M_Vmot] = Vmot;
  measValues[M_Imot] = Imot;
  measValues[M_Pmot] = Vmot * Imot;

  float V = settings[S_VmotTEMP];
  byte PWM = COERCE( 250.0 * V / Vsup, 0, 250 );
  analogWrite( DOUT_MOTOR_PWM, PWM );

  an.processData( p, 0 );

  measValues[M_pPk] = an.getPP();
  measValues[M_PEEP] = an.getPEEP();
  measValues[M_RR] = an.getRR();
  measValues[M_EI] = an.getEI();

  activeScreen->process();

  activeScreen->draw();

  Serial.print( F("t end=") );
  Serial.println( millis() - t );
}

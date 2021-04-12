#if !(defined(__AVR) || defined(AVR) || defined(__AVR_ATmega328P__))
#define __AVR_ATmega328P__
#endif
#include <avr/interrupt.h>
#include <avr/io.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "buffer.h"
#include "constants.h"
#include "packet.h"
#include "serialize.h"

#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#define BUF_LEN 512

volatile TBuffer sendbuf, recvbuf;

typedef enum {
    STOP = 0,
    FORWARD = 1,
    BACKWARD = 2,
    LEFT = 3,
    RIGHT = 4
} TDirection;

volatile TDirection dir = STOP;

/*
 * Alex's configuration constants
 */

// Number of ticks per revolution from the
// wheel encoder.

#define COUNTS_PER_REV 200

// Wheel circumference in cm.
// We will use this to calculate forward/backward distance traveled
// by taking revs * WHEEL_CIRC

#define WHEEL_CIRC 20.42

// Motor control pins. You need to adjust these till
// Alex moves in the correct direction
#define LF 5            // Left forward pin
#define LR 6            // Left reverse pin
#define RF 10           // Right forward pin
#define RR 9            // Right reverse pin
#define ALEX_LENGTH 16  // alex length
#define ALEX_BREADTH 6  // alex breath

// For colour sensor pins
#define s0 0
#define s1 1
#define s2 4
#define s3 7
#define out 8

// mask definitions

// PRR[7] is set to 1 to shut down the Two Wire Interface (TWI) / I2C
#define PRR_TWI_MASK 0b10000000

// PRR[6] is set to
#define PRR_TIMER2_MASK 0b01000000

// PRR[5] 
#define PRR_TIMER0_MASK 0b00100000

 
#define PRR_SPI_MASK 0b00000100
#define PRR_ADC_MASK 0b00000001

#define PRR_TIMER1_MASK 0b00001000

// SMCR[0] is the SE bit. SE is set to 1 to allow the MCU to enter the sleep mode when sleeping instruction is executed. 
#define SMCR_SLEEP_ENABLE_MASK 0b00000001

// SMCR[0] is the SE bit. SE is set to 1 to allow the MCU to enter the sleep mode when sleeping instruction is executed. 
// SMCR[3:1] are SM2, SM1 and SM0, SM[2:0] == 000 to put Arduino into idle mode 
#define SMCR_IDLE_MODE_MASK 0b00000001

// 
#define ADCSRA_ADC_MASK 0b1000000



void WDT_off(void) {
  /* Global interrupt should be turned OFF here if not already done so */

  // Clear WDRF in MCUSR
  MCUSR &= ~(1 << WDRF);

  // Write logical one to WDCE and WDE
  // Keep old prescaler setting to prevent unintentional time-out
  WDTCSR |= (1 << WDCE) | (1 << WDE);

  //Turn off WDT
  WDTCSR = 0x00;

  //Global interrupt should be turned ON here if subsequent operations after calling this function do not require turning off global interrupt
}

void setupPowerSaving() {
  // Turn off the Watchdog Timer
  WDT_off();
  // Modify PRR to shut down TWI
  PRR |= PRR_TWI_MASK;
  // Modify PRR to shut down SPI
  PRR |= PRR_SPI_MASK;
  // Modify ADCSRA to disable ADC,
  // then modify PRR to shut down ADC
  ADCSRA |= ADCSRA_ADC_MASK;
  PRR |= PRR_ADC_MASK;
  // Set the SMCR to choose the IDLE sleep mode
  // Do not set the Sleep Enable (SE) bit yet
  SMCR &= SMCR_IDLE_MODE_MASK;
  // Set Port B Pin 5 as output pin, then write a logic LOW
  // to it such that the LED tied to Arduino's Pin 13 is OFF.
  // e.g.
  // DDRB |= 0b00100000; // Arduino PIN 13 only
  // PORTB &= 0b11011111;

  // Pins in use are Arduino pins 5,6,10 & 11, the rest will be turned off accordingly like above
  DDRD |= 0b1001111;
  PORTD &= 0b01100000;

  DDRB |= 0b00110011;
  PORTD &= 0b11001100;
}

void putArduinoToIdle() {
  // Modify PRR to shut down TIMER 0, 1 and 2
  PRR |= PRR_TIMER0_MASK;
  PRR |= PRR_TIMER1_MASK;
  PRR |= PRR_TIMER2_MASK;
  // Modify SE bit in SMCR to enable (i.e., allow) sleep
  SMCR |= SMCR_SLEEP_ENABLE_MASK;
  // The following function puts ATmega328P's MCU into sleep;
  // it wakes up from sleep when USART serial data arrives sleep_cpu();
  sleep_cpu();
  // Modify SE bit in SMCR to disable (i.e., disallow) sleep
  SMCR &= (~SMCR_SLEEP_ENABLE_MASK);
  // Modify PRR to power up TIMER 0, 1, and 2
  PRR &= (~PRR_TIMER0_MASK);
  PRR &= (~PRR_TIMER1_MASK);
  PRR &= (~PRR_TIMER2_MASK);
}


// For colour sensor initial RGB values
volatile unsigned long Red = 0, Blue = 0, Green = 0;

// Colour detected by the colour sensor
volatile unsigned long colour;


// Ultrasonic Sensor
const int trigPin = 11;
const int echoPin = 12;

float duration, distance;


// Alex's diagonal. We compute and store this value once
// since it is expensive to compute and really does not change
float alexDiagonal = 0.0;
float alexCirc = 0.0;

/*
 *    Alex's State Variables
 */

// Store the ticks from Alex's left and
// right encoders.
volatile unsigned long leftForwardTicks = 0;
volatile unsigned long rightForwardTicks = 0;
volatile unsigned long leftReverseTicks = 0;
volatile unsigned long rightReverseTicks = 0;

volatile unsigned long leftForwardTicksTurns = 0;
volatile unsigned long rightForwardTicksTurns = 0;
volatile unsigned long leftReverseTicksTurns = 0;
volatile unsigned long rightReverseTicksTurns = 0;

// Store the revolutions on Alex's left
// and right wheels
volatile unsigned long leftRevs = 0;
volatile unsigned long rightRevs = 0;


// Forward and backward distance traveled
volatile unsigned long forwardDist = 0;
volatile unsigned long reverseDist = 0;

// Variables to keep track of whether we have moved a command distance//
volatile unsigned long deltaDist = 0;
volatile unsigned long newDist = 0;

// Variables to keep track of our turning angle
volatile unsigned long deltaTicks = 0;
volatile unsigned long targetTicks = 0;

void pwmWrite(uint8_t pin, int val) {
    volatile uint8_t* timer_comp;
    switch (pin) {
        case LF:
            timer_comp = &OCR0B;
            break;
        case LR:
            timer_comp = &OCR0A;
            break;
        case RF:
            timer_comp = &OCR1AL;
            break;
        case RR:
            timer_comp = &OCR1BL;
            break;
		default:
			return;
    }
    *timer_comp = val;
}

// Read the serial port. Returns the read character in
// ch if available. Also returns TRUE if ch is valid.
// This will be replaced later with bare-metal code.

int readSerial(char* buffer) {
    int count = 0;
    for (count = 0; dataAvailable(&recvbuf); count += 1) {
        readBuffer(&recvbuf, (unsigned char*)&buffer[count]);
    }
    return count;
}

// Write to the serial port. Replaced later with
// bare-metal code

void writeSerial(const char* buffer, int len) {
    for (int i = 0; i < len; i += 1) {
        writeBuffer(&sendbuf, buffer[i]);
    }
    sbi(UCSR0B, UDRIE0);
}

/*
 *
 * Alex Communication Routines.
 *
 */

TResult readPacket(TPacket* packet) {
    // Reads in data from the serial port and
    // deserializes it.Returns deserialized
    // data in "packet".

    char buffer[PACKET_SIZE];

    int len = readSerial(buffer);

    if (len == 0) {
        return PACKET_INCOMPLETE;
    } else {
        return deserialize(buffer, len, packet);
    }
}

void sendResponse(TPacket* packet) {
    // Takes a packet, serializes it then sends it out
    // over the serial port.
    char buffer[PACKET_SIZE];
    int len;

    len = serialize(buffer, packet, sizeof(TPacket));
    writeSerial(buffer, len);
}

void sendStatus() {
    // Implement code to send back a packet containing key
    // information like leftTicks, rightTicks, leftRevs, rightRevs
    // forwardDist and reverseDist
    // Use the params array to store this information, and set the
    // packetType and command files accordingly, then use sendResponse
    // to send out the packet. See sendMessage on how to use sendResponse.
    //
    TPacket statusPacket;
    statusPacket.packetType = PACKET_TYPE_RESPONSE;
    statusPacket.command = RESP_STATUS;
    statusPacket.params[0] = leftForwardTicks;
    statusPacket.params[1] = rightForwardTicks;
    statusPacket.params[2] = leftReverseTicks;
    statusPacket.params[3] = rightReverseTicks;
    statusPacket.params[4] = leftForwardTicksTurns;
    statusPacket.params[5] = rightForwardTicksTurns;
    statusPacket.params[6] = leftReverseTicksTurns;
    statusPacket.params[7] = rightReverseTicksTurns;
    statusPacket.params[8] = forwardDist;
    statusPacket.params[9] = reverseDist;
	
	// S2/S3 levels define which set of photodiodes we are using LOW/LOW is for RED LOW/HIGH is for Blue and HIGH/HIGH is for green
  // Here we wait until "out" go LOW, we start measuring the duration and stops when "out" is HIGH again, if you have trouble with this expression check the bottom of the code
  PORTD &= 0b01101111;
  Red = pulseIn(out, digitalRead(out) == HIGH ? LOW : HIGH);
  delay(20);

  PORTD |= 0b10000000;
  Blue = pulseIn(out, digitalRead(out) == HIGH ? LOW : HIGH);
  delay(20);

  PORTD |= 0b00010000;
  Green = pulseIn(out, digitalRead(out) == HIGH ? LOW : HIGH);
  delay(20);

  if (Red <= 30 && Green <= 40 && Blue <= 30)      //If the values are low it's likely the white color (all the colors are present)
    colour = 0; // strcpy(colour, "White");
  else if (Red <= Green && Red < 50 && Green > 60) //if Red value is the lowest one and smaller thant 23 it's likely Red
    colour = 1; // strcpy(colour, "Red");
  else if (Blue < Green && Blue < Red && Blue < 40) //Same thing for Blue
    colour = 2; // strcpy(colour, "Blue");
  else if (Green < Red && Green - Blue <= 5)      //Green is a little tricky, you can do it using the same method as above (the lowest), but here I used a reflective object
    colour = 3; // strcpy(colour, "Green");                   //which means the blue value is very low too, so I decided to check the difference between green and blue and see if it's acceptable
  else if (Red >= 50 && Green >= 55 && Blue >= 45)
    colour = 4; // strcpy(colour, "Black");
    
  statusPacket.params[10] = Red;
  statusPacket.params[11] = Green;
  statusPacket.params[12] = Blue;
  statusPacket.params[13] = colour;
    sendResponse(&statusPacket);
}

void sendMessage(const char* message) {
    // Sends text messages back to the Pi. Useful
    // for debugging.

    TPacket messagePacket;
    messagePacket.packetType = PACKET_TYPE_MESSAGE;
    strncpy(messagePacket.data, message, MAX_STR_LEN);
    sendResponse(&messagePacket);
}

void dbprint(const char* format, ...) {
    va_list args;
    char buffer[128];
    va_start(args, format);
    vsprintf(buffer, format, args);
    sendMessage(buffer);
}

void sendBadPacket() {
    // Tell the Pi that it sent us a packet with a bad
    // magic number.

    TPacket badPacket;
    badPacket.packetType = PACKET_TYPE_ERROR;
    badPacket.command = RESP_BAD_PACKET;
    sendResponse(&badPacket);
}

void sendBadChecksum() {
    // Tell the Pi that it sent us a packet with a bad
    // checksum.

    TPacket badChecksum;
    badChecksum.packetType = PACKET_TYPE_ERROR;
    badChecksum.command = RESP_BAD_CHECKSUM;
    sendResponse(&badChecksum);
}

void sendBadCommand() {
    // Tell the Pi that we don't understand its
    // command sent to us.

    TPacket badCommand;
    badCommand.packetType = PACKET_TYPE_ERROR;
    badCommand.command = RESP_BAD_COMMAND;
    sendResponse(&badCommand);
}

void sendBadResponse() {
    TPacket badResponse;
    badResponse.packetType = PACKET_TYPE_ERROR;
    badResponse.command = RESP_BAD_RESPONSE;
    sendResponse(&badResponse);
}

void sendOK() {
    TPacket okPacket;
    okPacket.packetType = PACKET_TYPE_RESPONSE;
    okPacket.command = RESP_OK;
    sendResponse(&okPacket);
}

/*
 * Setup and start codes for external interrupts and
 * pullup resistors.
 *
 */
// Enable pull up resistors on pins 2 and 3
void enablePullups() {
    DDRD &= 0b11110011;
    // PIND |= 0b110;
    PORTD |= 0b00001100;
    // Use bare-metal to enable the pull-up resistors on pins
    // 2 and 3. These are pins PD2 and PD3 respectively.
    // We set bits 2 and 3 in DDRD to 0 to make them inputs.
}

// Functions to be called by INT0 and INT1 ISRs.
void leftISR() {
    if (dir == FORWARD) {
        leftForwardTicks++;
        forwardDist = (unsigned long)((float)leftForwardTicks / COUNTS_PER_REV *
                                      WHEEL_CIRC);
    } else if (dir == BACKWARD) {
        leftReverseTicks++;
        reverseDist = (unsigned long)((float)leftReverseTicks / COUNTS_PER_REV *
                                      WHEEL_CIRC);
    } else if (dir == LEFT) {
        leftReverseTicksTurns++;
    } else if (dir == RIGHT) {
        leftForwardTicksTurns++;
    }
}

void rightISR() {
    if (dir == FORWARD) {
        rightForwardTicks++;
    } else if (dir == BACKWARD) {
        rightReverseTicks++;
    } else if (dir == LEFT) {
        rightForwardTicksTurns++;
    } else if (dir == RIGHT) {
        rightReverseTicksTurns++;
    }
}

// Set up the external interrupt pins INT0 and INT1
// for falling edge triggered. Use bare-metal.
void setupEINT() {
    // Use bare-metal to configure pins 2 and 3 to be
    // falling edge triggered. Remember to enable
    // the INT0 and INT1 interrupts.
    EIMSK |= 0b11;
    EICRA = 0b1010;
}

// Implement the external interrupt ISRs below.
// INT0 ISR should call leftISR while INT1 ISR
// should call rightISR.

ISR(INT0_vect) {
    leftISR();
}

ISR(INT1_vect) {
    rightISR();
}

// Implement INT0 and INT1 ISRs above.

/*
 * Setup and start codes for serial communications
 *
 */
// Set up the serial connection. For now we are using
// Arduino Wiring, you will replace this later
// with bare-metal code.
void setupSerial() {
    initBuffer(&sendbuf, BUF_LEN);
    initBuffer(&recvbuf, BUF_LEN);
    // async
    cbi(UCSR0C, UMSEL00);
    cbi(UCSR0C, UMSEL01);
    cbi(UCSR0C, UCPOL0);
    // parity: none
    cbi(UCSR0C, UPM00);
    cbi(UCSR0C, UPM01);
    // stop bit: 1
    cbi(UCSR0C, USBS0);
    // data size: 8
    sbi(UCSR0C, UCSZ00);
    sbi(UCSR0C, UCSZ01);
    cbi(UCSR0B, UCSZ02);
    cbi(UCSR0B, TXB80);
    // baud rate: 9600
    uint16_t b = F_CPU / 16 / 9600 - 1;
    UBRR0H = (uint8_t)(b >> 8);
    UBRR0L = (uint8_t)b;
    // single processor, normal transmission speed
    cbi(UCSR0A, MPCM0);
    cbi(UCSR0A, U2X0);
}

ISR(USART_RX_vect) {
    unsigned char data = UDR0;
    writeBuffer(&recvbuf, data);
}

ISR(USART_UDRE_vect) {
    unsigned char data;
    if (readBuffer(&sendbuf, &data) == BUFFER_OK) {
        UDR0 = data;
    } else {
        cbi(UCSR0B, UDRIE0);
    }
}

// Start the serial connection. For now we are using
// Arduino wiring and this function is empty. We will
// replace this later with bare-metal code.

void startSerial() {
    // enable rx and tx
    sbi(UCSR0B, TXEN0);
    sbi(UCSR0B, RXEN0);
    // enable interrupts
    cbi(UCSR0B, TXCIE0);
    sbi(UCSR0B, RXCIE0);
    cbi(UCSR0B, UDRIE0);  // data reg empty interrupt
}

/*
 * Alex's motor drivers.
 *
 */

// Set up Alex's motors. Right now this is empty, but
// later you will replace it with code to set up the PWMs
// to drive the motors.
void setupMotors() {
    // set as output
    DDRB |= 0b110;
    DDRD |= 0b1100000;
    // No prescaling, max freq
    sbi(TCCR0B, CS00);
    cbi(TCCR0B, CS01);
    cbi(TCCR0B, CS02);
    sbi(TCCR1B, CS10);
    cbi(TCCR1B, CS11);
    cbi(TCCR1B, CS12);
    // initialise counter
    TCNT0 = 0;
    TCNT1L = 0;
    TCNT1H = 0;
	OCR0A = 0;
	OCR0B = 0;
	OCR1AL = 0;
	OCR1AH = 0;
	OCR1BL = 0;
	OCR1BH = 0;
    // phase correct
    sbi(TCCR0A, WGM00);
    cbi(TCCR0A, WGM01);
    cbi(TCCR0B, WGM02);
    sbi(TCCR1A, WGM10);
    cbi(TCCR1A, WGM11);
    cbi(TCCR1B, WGM12);
    cbi(TCCR1B, WGM13);
    /* Our motor set up is:
     *    A1IN - Pin 5, PD5, OC0B
     *    A2IN - Pin 6, PD6, OC0A
     *    B1IN - Pin 9, PB1, OC1A
     *    B2IN - Pin 10, PB2, OC1B
     */
}

// Start the PWM for Alex's motors.
// We will implement this later. For now it is
// blank.
void startMotors() {
    // clear on compare, non-inverted
    cbi(TCCR0A, COM0A0);
    sbi(TCCR0A, COM0A1);
    cbi(TCCR0A, COM0B0);
    sbi(TCCR0A, COM0B1);
    cbi(TCCR1A, COM1A0);
    sbi(TCCR1A, COM1A1);
    cbi(TCCR1A, COM1B0);
    sbi(TCCR1A, COM1B1);
}

// Convert percentages to PWM values
int pwmVal(float speed) {
    if (speed < 0.0) {
        speed = 0;
    }

    if (speed > 100.0) {
        speed = 100.0;
    }

    return (int)((speed / 100.0) * 255.0);
}

// Move Alex forward "dist" cm at speed "speed".
// "speed" is expressed as a percentage. E.g. 50 is
// move forward at half speed.
// Specifying a distance of 0 means Alex will
// continue moving forward indefinitely.
void forward(float dist, float speed) {
    // Code tells us how far to move
    if (dist > 0) {
        deltaDist = dist;
    } else {
        deltaDist = 9999999;
    }

    newDist = forwardDist + deltaDist;

    dir = FORWARD;
    int val = pwmVal(speed);

    // For now we will ignore dist and move
    // forward indefinitely. We will fix this
    // in Week 9.

    // LF = Left forward pin, LR = Left reverse pin
    // RF = Right forward pin, RR = Right reverse pin
    // This will be replaced later with bare-metal code.

    pwmWrite(LF, val );
    pwmWrite(RF, val - 5);
    pwmWrite(LR, 0);
    pwmWrite(RR, 0);
}

// Reverse Alex "dist" cm at speed "speed".
// "speed" is expressed as a percentage. E.g. 50 is
// reverse at half speed.
// Specifying a distance of 0 means Alex will
// continue reversing indefinitely.
void reverse(float dist, float speed) {
    // Code tells us how far to move
    if (dist > 0) {
        deltaDist = dist;
    } else {
        deltaDist = 9999999;
    }

    newDist = reverseDist + deltaDist;

    dir = BACKWARD;

    int val = pwmVal(speed);

    // For now we will ignore dist and
    // reverse indefinitely. We will fix this
    // in Week 9.

    // LF = Left forward pin, LR = Left reverse pin
    // RF = Right forward pin, RR = Right reverse pin
    // This will be replaced later with bare-metal code.
    pwmWrite(LR, val );
    pwmWrite(RR, val - 5);
    pwmWrite(LF, 0);
    pwmWrite(RF, 0);
}
// New function to estimate number of wheel leftTicks
// needed to turn an angle
unsigned long computeDeltaTicks(float ang) {
    // We will assume that angular distance  moved=linear distance moved in one
    // wheels revolution.This is (probably) incorrect but simplifes calculation.
    //# of wheel revs to make one full 360 turn is alexCirc/WHEEL_CIRC
    // This is for 360.For ang degrees it will be(ang *alexCirc)/(360 *
    // WHEEL_CIRC)
    // To convert to ticks, we multiply by COUNTS_PER_REV.

    unsigned long ticks =
        (unsigned long)((ang * alexCirc * COUNTS_PER_REV) / (360 * WHEEL_CIRC));

    return ticks;
}

// Turn Alex left "ang" degrees at speed "speed".
// "speed" is expressed as a percentage. E.g. 50 is
// turn left at half speed.
// Specifying an angle of 0 degrees will cause Alex to
// turn left indefinitely.

void left(float ang, float speed) {
    int val = pwmVal(speed);

    dir = LEFT;

    if (ang == 0) {
        deltaTicks = 99999999;
    } else {
        deltaTicks = computeDeltaTicks(ang);
    }

    targetTicks = leftReverseTicksTurns + deltaTicks;

    // For now we will ignore ang. We will fix this in Week 9.
    // We will also replace this code with bare-metal later.
    // To turn left we reverse the left wheel and move
    // the right wheel forward.
    pwmWrite(LR, val );
    pwmWrite(RF, val - 5);
    pwmWrite(LF, 0);
    pwmWrite(RR, 0);
}

// Turn Alex right "ang" degrees at speed "speed".
// "speed" is expressed as a percentage. E.g. 50 is
// turn left at half speed.
// Specifying an angle of 0 degrees will cause Alex to
// turn right indefinitely.
void right(float ang, float speed) {
    int val = pwmVal(speed);

    dir = RIGHT;

    if (ang == 0) {
        deltaTicks = 99999999;
    } else {
        deltaTicks = computeDeltaTicks(ang);
    }

    targetTicks = rightReverseTicksTurns + deltaTicks;

    // For now we will ignore ang. We will fix this in Week 9.
    // We will also replace this code with bare-metal later.
    // To turn right we reverse the right wheel and move
    // the left wheel forward.
    pwmWrite(RR, val - 5);
    pwmWrite(LF, val );
    pwmWrite(LR, 0);
    pwmWrite(RF, 0);
}

// Stop Alex. To replace with bare-metal code later.
void stop() {
    pwmWrite(LF, 0);
    pwmWrite(LR, 0);
    pwmWrite(RF, 0);
    pwmWrite(RR, 0);
}

/*
 * Alex's setup and run codes
 *
 */

// Clears all our counters
void clearCounters() {
    leftForwardTicks = 0;
    rightForwardTicks = 0;
    leftReverseTicks = 0;
    rightReverseTicks = 0;

    leftForwardTicksTurns = 0;
    rightForwardTicksTurns = 0;
    leftReverseTicksTurns = 0;
    rightReverseTicksTurns = 0;

    leftRevs = 0;
    rightRevs = 0;
    forwardDist = 0;
    reverseDist = 0;
}

// Clears one particular counter
void clearOneCounter(int which) {
    clearCounters();
}
// Intialize Vincet's internal states

void initializeState() {
    clearCounters();
}

void handleCommand(TPacket* command) {
    switch (command->command) {
        // For movement commands, param[0] = distance, param[1] = speed.
        case COMMAND_FORWARD:
            sendOK();
            forward((float)command->params[0], (float)command->params[1]);
            break;
        case COMMAND_REVERSE:
            sendOK();
            reverse((float)command->params[0], (float)command->params[1]);
            break;
        case COMMAND_TURN_LEFT:
            sendOK();
            left((float)command->params[0], (float)command->params[1]);
            break;
        case COMMAND_TURN_RIGHT:
            sendOK();
            right((float)command->params[0], (float)command->params[1]);
            break;
        case COMMAND_STOP:
            sendOK();
            stop();
            break;
        case COMMAND_GET_STATS:
            sendOK();
            sendStatus();
            break;
        case COMMAND_CLEAR_STATS:
            sendOK();
            clearOneCounter(command->params[0]);
            break;
        default:
            sendBadCommand();
    }
}

void waitForHello() {
    int exit = 0;

    while (!exit) {
        TPacket hello;
        TResult result;

        do {
            result = readPacket(&hello);
        } while (result == PACKET_INCOMPLETE);

        if (result == PACKET_OK) {
            if (hello.packetType == PACKET_TYPE_HELLO) {
                sendOK();
                exit = 1;
            } else {
                sendBadResponse();
            }
        } else if (result == PACKET_BAD) {
            sendBadPacket();
        } else if (result == PACKET_CHECKSUM_BAD) {
            sendBadChecksum();
        }
    }  // !exit
}
void setupColourSensor()
{
  DDRD |= 0b10010011;
  DDRB &= 0b11111110;
  PORTD |= 0b00000011;
}

void setupUltrasonicSensor()
{
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
}

int calculateUltrasonic(){
	
}


void setup() {
    // put your setup code here, to run once:
    alexDiagonal =
        sqrt((ALEX_LENGTH * ALEX_LENGTH) + (ALEX_BREADTH * ALEX_BREADTH));

    alexCirc = M_PI * alexDiagonal;

    cli();
    setupEINT();
    setupSerial();
    startSerial();
    setupMotors();
    startMotors();
    enablePullups();
    initializeState();
	setupColourSensor();
	setupUltrasonicSensor();
	//setupPowerSaving();
    sei();
}

void handlePacket(TPacket* packet) {
    switch (packet->packetType) {
        case PACKET_TYPE_COMMAND:
            handleCommand(packet);
            break;

        case PACKET_TYPE_RESPONSE:
            break;

        case PACKET_TYPE_ERROR:
            break;

        case PACKET_TYPE_MESSAGE:
            break;

        case PACKET_TYPE_HELLO:
            break;
    }
}

void loop() {
    // Uncomment the code below for Step 2 of Activity 3 in Week 8 Studio 2

    // Uncomment the code below for Week 9 Studio 2

    // put your main code here, to run repeatedly:
	PORTD |= 0b11;
	
    TPacket recvPacket;  // This holds commands from the Pi

    TResult result = readPacket(&recvPacket);

    if (result == PACKET_OK) {
        handlePacket(&recvPacket);
    } else if (result == PACKET_BAD) {
        sendBadPacket();
    } else if (result == PACKET_CHECKSUM_BAD) {
        sendBadChecksum();
    }

    if (deltaDist > 0) {
        if (dir == FORWARD) {
            if (forwardDist >= newDist) {
                deltaDist = 0;
                newDist = 0;
                stop();
            }
        } else if (dir == BACKWARD) {
            if (reverseDist >= newDist) {
                deltaDist = 0;
                newDist = 0;
                stop();
            }
        } else if (dir == STOP) {
            deltaDist = 0;
            newDist = 0;
            stop();
			//putArduinoToIdle();
        }
    }

    if (deltaTicks > 0) {
        if (dir == LEFT) {
            if (leftReverseTicksTurns >= targetTicks) {
                deltaTicks = 0;
                targetTicks = 0;
                stop();
            }
        } else if (dir == RIGHT) {
            if (rightReverseTicksTurns >= targetTicks) {
                deltaTicks = 0;
                targetTicks = 0;
                stop();
            }
        } else if (dir == STOP) {
            deltaTicks = 0;
            targetTicks = 0;
            stop();
			//putArduinoToIdle();
        }
    }
}

#ifndef ARDUINO
int main() {
    setup();
    while (1) {
        // writeSerial("please", 7);
        loop();
    }
}
#endif
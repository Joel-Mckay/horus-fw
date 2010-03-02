/*
  stepper.c - stepper motor interface
  Part of Grbl

  Copyright (c) 2009 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

/* The timer calculations of this module informed by the 'RepRap cartesian firmware' by Zack Smith
   and Philipp Tiefenbacher. The circle buffer implementation gleaned from the wiring_serial library 
   by David A. Mellis */

#include "stepper.h"
#include "config.h"
#include <math.h>
#include <util/delay.h>
#include "nuts_bolts.h"
#include <avr/interrupt.h>

#include "wiring_serial.h"

#define TICKS_PER_MICROSECOND (F_CPU/1000000)
#define STEP_BUFFER_SIZE 100

// A marker used to notify the stepper handler of a pace change
#define PACE_CHANGE_MARKER 0xff

volatile uint8_t step_buffer[STEP_BUFFER_SIZE]; // A buffer for step instructions
volatile int step_buffer_head = 0;
volatile int step_buffer_tail = 0;
volatile uint32_t current_pace;
volatile uint32_t next_pace = 0;

uint8_t stepper_mode = STEPPER_MODE_STOPPED;

void config_pace_timer(uint32_t microseconds);

// This timer interrupt is executed at the pace set with st_buffer_pace. It pops one instruction from
// the step_buffer, executes it. Then it starts timer2 in order to reset the motor port after
// five microseconds.
SIGNAL(SIG_OUTPUT_COMPARE1A)
{
  if (step_buffer_head != step_buffer_tail) {
    PORTD &= ~(1<<3);
    uint8_t popped = step_buffer[step_buffer_tail]; 
    if(popped == PACE_CHANGE_MARKER) {
      // This is not a step-instruction, but a pace-change-marker: change pace
      config_pace_timer(next_pace);
      next_pace = 0;
    } else {      
      popped ^= STEPPING_INVERT_MASK;
      // Set the direction pins a cuple of nanoseconds before we step the steppers
      STEPPING_PORT = (STEPPING_PORT & ~DIRECTION_MASK) | (popped & DIRECTION_MASK);
      // Then pulse the stepping pins
      STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | popped;
      // Reset step pulse reset timer
      TCNT2 = -(((STEP_PULSE_MICROSECONDS-4)*TICKS_PER_MICROSECOND)/8);  
    }
    // move the step buffer tail to the next instruction
    step_buffer_tail = (step_buffer_tail + 1) % STEP_BUFFER_SIZE;
  } else {
    PORTD |= (1<<3);    
  }
}

// This interrupt is set up by SIG_OUTPUT_COMPARE1A when it sets the motor port bits. It resets
// the motor port after a short period (STEP_PULSE_MICROSECONDS) completing one step cycle.
SIGNAL(SIG_OVERFLOW2)
{
  // reset stepping pins (leave the direction pins)
  STEPPING_PORT = (STEPPING_PORT & ~STEP_MASK) | (STEPPING_INVERT_MASK & STEP_MASK); 
}

// Initialize and start the stepper motor subsystem
void st_init()
{
	// Configure directions of interface pins
  STEPPING_DDR   |= STEPPING_MASK;
  STEPPING_PORT = (STEPPING_PORT & ~STEPPING_MASK); //| STEPPING_INVERT_MASK;
  LIMIT_DDR &= ~(LIMIT_MASK);
  STEPPERS_ENABLE_DDR |= 1<<STEPPERS_ENABLE_BIT;
  
	// waveform generation = 0100 = CTC
	TCCR1B &= ~(1<<WGM13);
	TCCR1B |=  (1<<WGM12);
	TCCR1A &= ~(1<<WGM11); 
	TCCR1A &= ~(1<<WGM10);

	// output mode = 00 (disconnected)
	TCCR1A &= ~(3<<COM1A0); 
	TCCR1A &= ~(3<<COM1B0); 
	
	// Configure Timer 2
  TCCR2A = 0; // Normal operation
  TCCR2B = (1<<CS21); // Full speed, 1/8 prescaler
  TIMSK2 = 0; // All interrupts disabled
  
  sei();
  
	// start off with a mellow pace
  config_pace_timer(20000);
}

inline void st_buffer_step(uint8_t motor_port_bits)
{
  // Buffer nothing unless stepping subsystem is running
  if (stepper_mode != STEPPER_MODE_RUNNING) { return; }
  // Calculate the buffer head after we push this byte
	int next_buffer_head = (step_buffer_head + 1) % STEP_BUFFER_SIZE;	
	// If the buffer is full: good! That means we are well ahead of the robot. 
	// Nap until there is room for more steps.
  while(step_buffer_tail == next_buffer_head) { sleep_mode(); }
	// Push byte
  step_buffer[step_buffer_head] = motor_port_bits;
  step_buffer_head = next_buffer_head;
}

// Block until all buffered steps are executed
void st_synchronize()
{
  if (stepper_mode == STEPPER_MODE_RUNNING) {
    while(step_buffer_tail != step_buffer_head) { sleep_mode(); }    
  } else {
    st_flush();
  }
}

// Cancel all pending steps
void st_flush()
{
  cli();
  step_buffer_tail = step_buffer_head;
  sei();
}

// Start the stepper subsystem
void st_start()
{
  // Enable timer interrupts
	TIMSK1 |= (1<<OCIE1A);
  TIMSK2 |= (1<<TOIE2);      
  // set enable pin   
  STEPPERS_ENABLE_PORT |= 1<<STEPPERS_ENABLE_BIT;
  stepper_mode = STEPPER_MODE_RUNNING;
}

// Execute all buffered steps, then stop the stepper subsystem
inline void st_stop()
{
  // flush pending operations
  st_synchronize();
  // disable timer interrupts
	TIMSK1 &= ~(1<<OCIE1A);
  TIMSK2 &= ~(1<<TOIE2);           
  // reset enable pin
  STEPPERS_ENABLE_PORT &= ~(1<<STEPPERS_ENABLE_BIT);
  stepper_mode = STEPPER_MODE_STOPPED;
}

// Buffer a pace change. Pace is the rate with which steps are executed. It is measured in microseconds from step to step. 
// It is continually adjusted to achieve constant actual feed rate. Unless pace-changes was buffered along with the steps 
// they govern they might change at slightly wrong moments in time as the pace would change while the stepper buffer was
// still churning out the previous movement.
void st_buffer_pace(uint32_t microseconds)
{
  // Do nothing if the pace in unchanged or the stepping subsytem is not running
  if ((current_pace == microseconds) || (stepper_mode != STEPPER_MODE_RUNNING)) { return; }
  // If the single-element pace "buffer" is full, sleep until it is popped
  while (next_pace != 0) {
    sleep_mode();
  }  
  // Buffer the pace change
  next_pace = microseconds;
  st_buffer_step(PACE_CHANGE_MARKER); // Place a pace-change marker in the step-buffer
}

// Returns a bitmask with the stepper bit for the given axis set
uint8_t st_bit_for_stepper(int axis) {
  switch(axis) {
    case X_AXIS: return(1<<X_STEP_BIT);
    case Y_AXIS: return(1<<Y_STEP_BIT);
    case Z_AXIS: return(1<<Z_STEP_BIT);
  }
  return(0);
}

// Configures the prescaler and ceiling of timer 1 to produce the given pace as accurately as possible.
void config_pace_timer(uint32_t microseconds)
{
  uint32_t ticks = microseconds*TICKS_PER_MICROSECOND;
  uint16_t ceiling;
  uint16_t prescaler;
	if (ticks <= 0xffffL) {
		ceiling = ticks;
    prescaler = 0; // prescaler: 0
	} else if (ticks <= 0x7ffffL) {
    ceiling = ticks >> 3;
    prescaler = 1; // prescaler: 8
	} else if (ticks <= 0x3fffffL) {
		ceiling =  ticks >> 6;
    prescaler = 2; // prescaler: 64
	} else if (ticks <= 0xffffffL) {
		ceiling =  (ticks >> 8);
    prescaler = 3; // prescaler: 256
	} else if (ticks <= 0x3ffffffL) {
		ceiling = (ticks >> 10);
    prescaler = 4; // prescaler: 1024
	} else {
	  // Okay, that was slower than we actually go. Just set the slowest speed
		ceiling = 0xffff;
    prescaler = 4;
	}
	// Set prescaler
  TCCR1B = (TCCR1B & ~(0x07<<CS10)) | ((prescaler+1)<<CS10);
  // Set ceiling
  OCR1A = ceiling;
  current_pace = microseconds;
}

int check_limit_switches()
{
  // Dual read as crude debounce
  return((LIMIT_PORT & LIMIT_MASK) | (LIMIT_PORT & LIMIT_MASK));
}

int check_limit_switch(int axis)
{
  uint8_t mask = 0;
  switch (axis) {
    case X_AXIS: mask = 1<<X_LIMIT_BIT; break;
    case Y_AXIS: mask = 1<<Y_LIMIT_BIT; break;
    case Z_AXIS: mask = 1<<Z_LIMIT_BIT; break;
  }
  return((LIMIT_PORT&mask) || (LIMIT_PORT&mask));    
}

void st_go_home()
{
  // Todo: Perform the homing cycle
}

// Convert from millimeters to step-counts along the designated axis
int32_t st_millimeters_to_steps(double millimeters, int axis) {
  switch(axis) {
    case X_AXIS: return(round(millimeters*X_STEPS_PER_MM));
    case Y_AXIS: return(round(millimeters*Y_STEPS_PER_MM));
    case Z_AXIS: return(round(millimeters*Z_STEPS_PER_MM));
  }
  return(0);
}
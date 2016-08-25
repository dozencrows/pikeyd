/**** joy_RPi.c ****************************/
/* M. Moller   2013-01-16                  */
/*   Universal RPi GPIO keyboard daemon    */
/*******************************************/

/*
   Copyright (C) 2013 Michael Moller.
   This file is part of the Universal Raspberry Pi GPIO keyboard daemon.

   This is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The software is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  
*/

// How polling works:
//	joy_RPi_poll is called from main thread every 4 ms (timing via usleep):
//		if /dev/mem method is set up:
//			read bit vector of GPIO status from memory mapped location
//		else
//			iterate chosen GPIO pins, open/read/close its value pseudo-file to
//			build bit vector of GPIO status
//
//		if GPIO bit vec != last GPIO bit vec
//			zero bounce timer
//			XOR GPIO bit vector against last bit vector to get changes, and or into
//			'changed' bit vector
//
//		set last GPIO bit vec to new GPIO bit vec
//
//		if bounce timer >= limit
//			update button values and changed flags from GPIO and 'changed' bit vecs
//			clear 'changed' bit vec
//
//		increment bounce timer if < limit
//
//		if any buttons have changed
//			clear button changed flag and generate corresponding key presses in uinput
//			(will iterate to generate multiple key presses per GPIO as configured)
//
//		process auto-repeat of last key sent (looks like literally the last key press sent,
//		not repeat sequences)
//
//		use latest 'raw' GPIO bit vec to drive rotary control keypress generation
//			for each rotary control, extracts 2 relevant GPIO values from bit vec
//			and applies Gray code decoding using those values & stored previous values
//			to determine if key should be sent.
//

// Implementing interrupt driven GPIO reading for rotary controls...
//
// - Add a pthread_mutex to uinput.c to make it thread safe
//		- set up in init_uinput, tear down in close_uinput
//		- wrap sendKey and sendRel in mutex lock/unlock calls
//
// - Refactor rotary control implementation in config.c:
//		- get_next_rotary_key to be 'update_rotary_keys', with gpio state and rotary control
//	 	  struct as arguments.
//		- remove send_gpio_rotary_keys, restart_rotaries, got_more_rotaries and associated
//		  calling code and data structures
//		- add code to query for rotary use, get and iterate rotary list based on GPIO pin
//
// - For each GPIO pin associated with a rotary control (2 per control)
//		- set interrupt edge to 'both' (e.g. via writing 'both' to /sys/class/gpio/gpio?/edge)
//		- open file descriptor for value as non-blocking and store it
//		- start an interrupt handler thread for the pin:
//			- Pass in pin number
//			- Enter polling loop (infinite timeout) on the file descriptor for the pin
//			- on receiving interrupt:
//				- read 1 char to clear interrupt
//				- read GPIO status from memory map (entire)
//				- call update_rotary_keys with new GPIO status and each rotary control struct
//				  in linked list associated with pin ()
//
// - Add shutdown code that iterates all rotary control threads, cancelling them and
//	 joining them, then closing the associated file descriptors.
//
// Useful references:
//	https://git.drogon.net/?p=wiringPi;a=blob_plain;f=wiringPi/wiringPi.c;hb=HEAD
//	https://developer.ridgerun.com/wiki/index.php/Gpio-int-test.c
//	https://github.com/metachris/RPIO/blob/ed01a67a4cfa03db91e571994771a650ac5818f0/source/RPIO/_RPIO.py


/*******************************************/
/* based on the xmame driver by
/* Jason Birch   2012-11-21   V1.00        */
/* Joystick control for Raspberry Pi GPIO. */
/*******************************************/

//#include "xmame.h"
//#include "devices.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "joy_RPi.h"
#include "config.h"

#if defined RPI_JOYSTICK

#if defined RPI1
#define GPIO_PERI_BASE        0x20000000
#else
#define GPIO_PERI_BASE        0x3F000000
#endif
#define GPIO_BASE             (GPIO_PERI_BASE + 0x200000)
#define BLOCK_SIZE            (4 * 1024)
#define PAGE_SIZE             (4 * 1024)
#define GPIO_ADDR_OFFSET      13
#define BUFF_SIZE             128
#define JOY_BUTTONS           32
#define JOY_AXES              2
#define JOY_DIRS              2

#define OFFSET_PULLUPDN     37  // 0x0094 / 4
#define OFFSET_PULLUPDNCLK  38  // 0x0098 / 4

#define PUD_OFF  0
#define PUD_DOWN 1
#define PUD_UP   2

#define BOUNCE_TIME 2

// Raspberry Pi V1 GPIO
//static int GPIO_Pin[] = { 0, 1, 4, 7, 8, 9, 10, 11, 14, 15, 17, 18, 21, 22, 23, 24, 25 };
// Raspberry Pi V2 GPIO
//static int GPIO_Pin[] = { 2, 3, 4, 7, 8, 9, 10, 11, 14, 15, 17, 18, 22, 23, 24, 25, 27 };
//MameBox pins
//static int GPIO_Pin[] = { 4, 17, 18, 22, 23, 24, 10, 25, 11, 8, 7 };
static char GPIO_Filename[JOY_BUTTONS][BUFF_SIZE];

static int GpioFile;
static char* GpioMemory;
static char* GpioMemoryMap;
volatile unsigned* GPIO;
static int AllMask;
static int lastGpio=0;
static int xGpio=0;
static int bounceCount=0;
static int doRepeat=0;
static int pullUpDown=PUD_OFF;

struct joydata_struct
{
  int fd;
  int num_axes;
  int num_buttons;
  int button_raw;
  int button_mask;
  int change_mask;
  int xio_mask;
  int buttons[JOY_BUTTONS];
  int change[JOY_BUTTONS];
  int is_xio[JOY_BUTTONS];
} joy_data[1];

static void short_wait(void)
{
  int i;
  for (i=0; i<150; i++) {
    asm volatile("nop");
  }
}

static void set_pullupdn(int gpio, int pud)
{
    int clk_offset = OFFSET_PULLUPDNCLK + (gpio/32);
    int shift = (gpio%32);

    if (pud == PUD_DOWN)
       *(GPIO+OFFSET_PULLUPDN) = (*(GPIO+OFFSET_PULLUPDN) & ~3) | PUD_DOWN;
    else if (pud == PUD_UP)
       *(GPIO+OFFSET_PULLUPDN) = (*(GPIO+OFFSET_PULLUPDN) & ~3) | PUD_UP;
    else  // pud == PUD_OFF
       *(GPIO+OFFSET_PULLUPDN) &= ~3;

    short_wait();
    *(GPIO+clk_offset) = 1 << shift;
    short_wait();
    *(GPIO+OFFSET_PULLUPDN) &= ~3;
    *(GPIO+clk_offset) = 0;
}

void joy_pullup(void)
{
  pullUpDown = PUD_UP;
}

void joy_pulldown(void)
{
  pullUpDown = PUD_DOWN;
}

int joy_RPi_init(void)
{
  FILE* File;
  int Index;
  char Buffer[BUFF_SIZE];
  int n = gpios_used();
  int xios = 0;

  for (Index = 0; Index < n; ++Index){
    sprintf(Buffer, "/sys/class/gpio/export");
    if (!(File = fopen(Buffer, "w"))){
      perror(Buffer);
      //printf("Failed to open file: %s\n", Buffer);
      return -1;
    }
    else{
      fprintf(File, "%u", gpio_pin(Index));
      fclose(File);

      sprintf(Buffer, "/sys/class/gpio/gpio%u/direction", gpio_pin(Index));
      if (!(File = fopen(Buffer, "w"))){
    	  perror(Buffer);
    	  //printf("Failed to open file: %s\n", Buffer);
      }
      else{
		fprintf(File, "in");
		fclose(File);
		sprintf(GPIO_Filename[Index], "/sys/class/gpio/gpio%u/value", gpio_pin(Index));
      }
      AllMask |= (1 << gpio_pin(Index));
      xios |= ( is_xio(gpio_pin(Index)) << gpio_pin(Index) );
      joy_data[0].is_xio[Index] = is_xio(gpio_pin(Index));
    }
  }

  GPIO = NULL;
  GpioMemory = NULL;
  if((GpioFile = open("/dev/mem", O_RDWR | O_SYNC)) < 0){
    perror("/dev/mem");
    printf("Failed to open memory\n");
    return -1;
  }
  else if(!(GpioMemory = malloc(BLOCK_SIZE + PAGE_SIZE - 1))){
    perror("malloc");
    printf("Failed to allocate memory map\n");
    return -1;
  }
  else{
    if ((unsigned long)GpioMemory % PAGE_SIZE){
      GpioMemory += PAGE_SIZE - ((unsigned long)GpioMemory % PAGE_SIZE);
    }

    if ((long)(GpioMemoryMap = 
	       (unsigned char*)mmap(
				    (caddr_t)GpioMemory, 
				    BLOCK_SIZE, 
				    PROT_READ | PROT_WRITE,
				    MAP_SHARED | MAP_FIXED, 
				    GpioFile, 
				    GPIO_BASE
				    )
	       ) < 0){
      perror("mmap");
      printf("Failed to map memory\n");
      return -1;
    }
    else{
      close(GpioFile);
      GPIO = (volatile unsigned*)GpioMemoryMap;
      lastGpio = ((int*)GPIO)[GPIO_ADDR_OFFSET];

      for (Index = 0; Index < n; ++Index){
        set_pullupdn(gpio_pin(Index), pullUpDown); 
      }
    }
  }

  /* Set the file descriptor to a dummy value. */
  joy_data[0].fd = 1;
  joy_data[0].num_buttons = n;
  joy_data[0].num_axes = 0;
  joy_data[0].button_mask=0;
  joy_data[0].button_raw=0;
  joy_data[0].xio_mask=xios;

  bounceCount=0;

  printf("Joystick init OK.\n");

  return 0;
}


void joy_RPi_exit(void)
{
   if (GpioFile >= 0)
      close(GpioFile);
}


void joy_RPi_poll(void)
{
  FILE* File;
  int Joystick;
  int Index;
  int Char;
  int newGpio;
  int iomask;

  Joystick = 0;

  if(joy_data[Joystick].fd){			
    if (!GPIO){ /* fallback I/O? don't use - very slow. */
      for (Index = 0; Index < joy_data[Joystick].num_buttons; ++Index){
		if( (File = fopen(GPIO_Filename[Index], "r")) ){
		  Char = fgetc(File);
		  fclose(File);

		  iomask = (1 << gpio_pin(Index));
		  if (Char == '0'){
			newGpio |= iomask;
			//joy_data[Joystick].buttons[Index] = 1;
		  }
		  else{
			newGpio &= ~iomask;
			//joy_data[Joystick].buttons[Index] = 0;
		  }
		}
      }
    }
    else{
      newGpio = ((int*)GPIO)[GPIO_ADDR_OFFSET];
    }
    newGpio &= AllMask;

    //printf("%d: %08x\n", bounceCount, newGpio);
    
    if(newGpio != lastGpio){
      bounceCount=0;
      xGpio |= newGpio ^ lastGpio;
      //printf("%08x\n", xGpio);
    }
    lastGpio = newGpio;
    xGpio &= ~joy_data[Joystick].xio_mask; /* remove expanders from change monitor */

    if(bounceCount>=BOUNCE_TIME){
      joy_data[Joystick].button_mask = newGpio;
      joy_data[Joystick].change_mask = xGpio;

      for (Index = 0; Index < joy_data[Joystick].num_buttons; ++Index){
		iomask = (1 << gpio_pin(Index));
		joy_data[Joystick].buttons[Index] = !(newGpio & iomask);
		joy_data[Joystick].change[Index] = !!(xGpio & iomask);
      }
      xGpio = 0;
    }

    if(bounceCount<BOUNCE_TIME)bounceCount++;

    joy_data[Joystick].button_raw = newGpio;
    joy_handle_event();

  }
}

void joy_enable_repeat(void)
{
  doRepeat = 1;
}

static void joy_handle_repeat(void)
{
  const struct {
    int time[4];
    int value[4];
    int next[4];
  }mxkey = {
    {80, 200, 40, 40},
    {0, 1, 0, 1},
    {1, 2, 3, 2}
  };
  /* key repeat metrics: release after 80ms, press after 200ms, release after 40ms, press after 40ms */

  static int idx = -1;
  static int prev_key = -1;
  static unsigned t_now = 0;
  static unsigned t_next = 0;
  keyinfo_s ks;

  get_last_key(&ks);

  if(doRepeat){
    if(!ks.val || (ks.key != prev_key)){ /* restart on release or key change */
      prev_key = ks.key;
      idx=-1;
      t_next = t_now;
    }
    else if(idx<0){ /* start new cycle */
      idx = 0;
      t_next = t_now + mxkey.time[idx];
    }
    else if(t_now == t_next){
      sendKey(ks.key, mxkey.value[idx]);
      idx = mxkey.next[idx];
      t_next = t_now + mxkey.time[idx];
    }
    t_now+=4; /* runs every 4 ms */
  }
}

void joy_handle_event(void)
{
  int Joystick = 0;
  int Index;

  /* handle all active irqs */
  if(~joy_data[Joystick].button_mask & joy_data[Joystick].xio_mask){ /* if active ints exist */
    //printf("XIO = %08x\n", ~joy_data[Joystick].button_mask & joy_data[Joystick].xio_mask);
    for (Index = 0; Index < joy_data[Joystick].num_buttons; ++Index){
      if( joy_data[Joystick].is_xio[Index] & joy_data[Joystick].buttons[Index]){
	send_gpio_keys(gpio_pin(Index), joy_data[Joystick].buttons[Index]);
      } 
    }
  }
  /* handle normal gpios */
  if(joy_data[Joystick].change_mask){
    //printf("GPIOs = %08x\n", joy_data[Joystick].button_mask);
    joy_data[Joystick].change_mask = 0;
    for (Index = 0; Index < joy_data[Joystick].num_buttons; ++Index){
      if( joy_data[Joystick].change[Index] ){
	joy_data[Joystick].change[Index] = 0;
	//printf("Button %d = %d\n", Index, joy_data[Joystick].buttons[Index]);
	send_gpio_keys(gpio_pin(Index), joy_data[Joystick].buttons[Index]);
      } 
    }
  }
  joy_handle_repeat();
  send_gpio_rotary_keys(joy_data[Joystick].button_raw);
}


#endif


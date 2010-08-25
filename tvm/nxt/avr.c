/*
 * avr.c - NXT AVR interface functions
 *
 * Copyright (C) 2007  the NxOS developers (see NXOS)
 * Copyright (C) 2010  Carl G. Ritson
 *
 * Redistribution of this file is permitted under
 * the terms of the GNU Public License (GPL) version 2.
 */

#include "tvm-nxt.h"
#include "at91sam7s256.h"
/*{{{  Defines */
#define AVR_ADDRESS 1
#define AVR_MAX_FAILED_CHECKSUMS 3
/*}}}*/

/*{{{  Static Data */
static const char avr_init_handshake[] =
	"\xCC" "Let's samba nxt arm in arm, (c)LEGO System A/S";

static volatile struct {
	/* The current state of the TWI driver state machine. */
	enum twi_mode {
		TWI_UNINITIALISED = 0,
		TWI_FAILED,
		TWI_READY,
		TWI_TX_BUSY,
		TWI_RX_BUSY,
	} mode;

	/* Address and size of a send/receive buffer. */
	uint8_t *ptr;
	uint32_t len;
} twi_state;

static volatile struct {
	/* The current mode of the AVR state machine. */
	enum {
		AVR_UNINITIALISED = 0, /* Initialization not completed. */
		AVR_LINK_DOWN,         /* No handshake has been sent. */
		AVR_INIT,              /* Handshake send in progress. */
		AVR_WAIT_2MS,          /* Timed wait after the handshake. */
		AVR_WAIT_1MS,          /* More timed wait. */
		AVR_SEND,              /* Sending of to_avr in progress. */
		AVR_RECV,              /* Reception of from_avr in progress. */
	} mode;

	/* Used to detect link failures and restart the AVR link. */
	uint8_t failed_consecutive_checksums;
} avr_state;

/* Contains all the commands that are periodically sent to the AVR. */
static volatile struct {
	/* Tells the AVR to perform power management: */
	enum {
		AVR_RUN = 0,    /* No power management (normal runtime mode). */
		AVR_POWER_OFF,  /* Power down the brick. */
		AVR_RESET_MODE, /* Go into SAM-BA reset mode. */
	} power_mode;

	/* The speed and braking configuration of the motor ports. */
	int8_t motor_speed[NXT_N_MOTORS];
	uint8_t motor_brake;

	/* TODO: enable controlling of input power. Currently the input
	 * stuff is ignored.
	 */
} to_avr;

/* Contains all the status data periodically received from the AVR. */
static volatile struct {
	/* The analog reading of the analog pin on all active sensors. */
	uint16_t adc_value[NXT_N_SENSORS];

	/* The state of the NXT's buttons. Given the way that the buttons
	 * are handled in hardware, only one button is reported pressed at a
	 * time. See the nx_avr_button_t enumeration for values to test for.
	 */
	uint8_t buttons;

	/* Battery information. */
	struct {
		bool is_aa; /* True if the power supply is AA batteries (as
			     * opposed to a battery pack).
			     */
		uint16_t charge; /* The remaining battery charge in mV. */
	} battery;

	/* The version of the AVR firmware. The currently supported version
	 * is 1.1.
	 */
	struct {
		uint8_t major;
		uint8_t minor;
	} version;
} from_avr;


/* The following two arrays hold the data structures above, converted
 * into the raw ARM-AVR communication format. Data to send is
 * serialized into this buffer prior to sending, and received data is
 * received into here before being deserialized into the status
 * struct.
 */
static uint8_t raw_from_avr[
	(2 * NXT_N_SENSORS) + /* Sensor A/D value. */
	2 + /* Buttons reading.  */
	2 + /* Battery type, charge and AVR firmware version. */
	1]; /* Checksum. */
static uint8_t raw_to_avr[
	1 + /* Power mode    */
	1 + /* PWM frequency */
	4 + /* output % for the 4 (?!)  motors */
	1 + /* Output modes (brakes) */
	1 + /* Input modes (sensor power) */
	1]; /* Checksum */
/*}}}*/

/*{{{  Two-Wire Interface */
static void twi_isr (void)
{
	/* Read the status register once to acknowledge all TWI interrupts. */
	uint32_t status = *AT91C_TWI_SR;

	/* Read mode and the status indicates a byte was received. */
	if (twi_state.mode == TWI_RX_BUSY && (status & AT91C_TWI_RXRDY)) {
		*twi_state.ptr = *AT91C_TWI_RHR;
		twi_state.ptr++;
		twi_state.len--;

		/* If only one byte is left to read, instruct the TWI to send a STOP
		 * condition after the next byte.
		 */
		if (twi_state.len == 1) {
			*AT91C_TWI_CR = AT91C_TWI_STOP;
		}

		/* If the read is over, inhibit all TWI interrupts and return to the
		 * ready state.
		 */
		if (twi_state.len == 0) {
			*AT91C_TWI_IDR = ~0;
			twi_state.mode = TWI_READY;
		}
	}

	/* Write mode and the status indicated a byte was transmitted. */
	if (twi_state.mode == TWI_TX_BUSY && (status & AT91C_TWI_TXRDY)) {
		/* If that was the last byte, inhibit TWI interrupts and return to
		 * the ready state.
		 */
		if (twi_state.len == 0) {
			*AT91C_TWI_IDR = ~0;
			twi_state.mode = TWI_READY;
		} else {
			/* Instruct the TWI to send a STOP condition at the end of the
			 * next byte if this is the last byte.
			 */
			if (twi_state.len == 1)
				*AT91C_TWI_CR = AT91C_TWI_STOP;

			/* Write the next byte to the transmit register. */
			*AT91C_TWI_THR = *twi_state.ptr;
			twi_state.ptr++;
			twi_state.len--;
		}
	}

	/* Check for error conditions. There are pretty critical failures,
	 * since they indicate something is very wrong with either this
	 * driver, or the coprocessor, or the hardware link between them.
	 */
	if (status & (AT91C_TWI_OVRE | AT91C_TWI_UNRE | AT91C_TWI_NACK)) {
		*AT91C_TWI_CR = AT91C_TWI_STOP;
		*AT91C_TWI_IDR = ~0;
		twi_state.mode = TWI_FAILED;
		/* TODO: This should be an assertion failed, ie. a hard crash. */
	}
}

static void twi_init (void)
{
	uint32_t clocks = 9;

	nxt_interrupts_disable ();

	/* Power up the TWI and PIO controllers. */
	*AT91C_PMC_PCER = (1 << AT91C_ID_TWI) | (1 << AT91C_ID_PIOA);

	/* Inhibit all TWI interrupt sources. */
	*AT91C_TWI_IDR = ~0;

	/* If the system is rebooting, the coprocessor might believe that it
	 * is in the middle of an I2C transaction. Furthermore, it may be
	 * pulling the data line low, in the case of a read transaction.
	 *
	 * The TWI hardware has a bug that will lock it up if it is
	 * initialized when one of the clock or data lines is low.
	 *
	 * So, before initializing the TWI, we manually take control of the
	 * pins using the PIO controller, and manually drive a few clock
	 * cycles, until the data line goes high.
	 */
	*AT91C_PIOA_MDER = AT91C_PA3_TWD | AT91C_PA4_TWCK;
	*AT91C_PIOA_PER = AT91C_PA3_TWD | AT91C_PA4_TWCK;
	*AT91C_PIOA_ODR = AT91C_PA3_TWD;
	*AT91C_PIOA_OER = AT91C_PA4_TWCK;

	while (clocks > 0 && !(*AT91C_PIOA_PDSR & AT91C_PA3_TWD)) {
		*AT91C_PIOA_CODR = AT91C_PA4_TWCK;
		systick_wait_ns (1500);
		*AT91C_PIOA_SODR = AT91C_PA4_TWCK;
		systick_wait_ns (1500);
		clocks--;
	}

	nxt_interrupts_enable ();

	/* Now that the I2C lines are clean, hand them back to the TWI
	 * controller.
	 */
	*AT91C_PIOA_PDR = AT91C_PA3_TWD | AT91C_PA4_TWCK;
	*AT91C_PIOA_ASR = AT91C_PA3_TWD | AT91C_PA4_TWCK;

	/* Reset the controller and configure its clock. The clock setting
	 * makes the TWI run at 380kHz.
	 */
	*AT91C_TWI_CR = AT91C_TWI_SWRST | AT91C_TWI_MSDIS;
	*AT91C_TWI_CWGR = 0x020f0f;
	*AT91C_TWI_CR = AT91C_TWI_MSEN;
	twi_state.mode = TWI_READY;

	/* Install the TWI interrupt handler. */
	aic_install_isr (AT91C_ID_TWI, AIC_PRIO_RT, AIC_TRIG_LEVEL, twi_isr);
}

void twi_read_async (uint32_t dev_addr, uint8_t *data, uint32_t len)
{
	uint32_t mode = ((dev_addr & 0x7f) << 16) | AT91C_TWI_IADRSZ_NO | AT91C_TWI_MREAD;

	//NX_ASSERT(dev_addr == 1);
	//NX_ASSERT(data != NULL);
	//NX_ASSERT(len > 0);
	//NX_ASSERT(nx__twi_ready());

	/* TODO: assert(nx__twi_ready()) */

	twi_state.mode = TWI_RX_BUSY;
	twi_state.ptr = data;
	twi_state.len = len;

	*AT91C_TWI_MMR = mode;
	*AT91C_TWI_CR = AT91C_TWI_START | AT91C_TWI_MSEN;
	*AT91C_TWI_IER = AT91C_TWI_RXRDY;
}

static void twi_write_async (uint32_t dev_addr, uint8_t *data, uint32_t len)
{
	uint32_t mode = ((dev_addr & 0x7f) << 16) | AT91C_TWI_IADRSZ_NO;

	//NX_ASSERT(dev_addr == 1);
	//NX_ASSERT(data != NULL);
	//NX_ASSERT(len > 0);
	//NX_ASSERT(nx__twi_ready());

	twi_state.mode = TWI_TX_BUSY;
	twi_state.ptr = data;
	twi_state.len = len;

	*AT91C_TWI_MMR = mode;
	*AT91C_TWI_CR = AT91C_TWI_START | AT91C_TWI_MSEN;
	*AT91C_TWI_IER = AT91C_TWI_TXRDY;
}

static bool twi_ready (void)
{
	return (twi_state.mode == TWI_READY) ? TRUE : FALSE;
}
/*}}}*/

/*{{{  AVR Interface */
/* Serialize the to_avr data structure into raw_to_avr, ready for
 * sending to the AVR.
 */
static void avr_pack_to_avr (void)
{
	uint32_t i;
	uint8_t checksum = 0;

	memset (raw_to_avr, 0, sizeof (raw_to_avr));

	/* Marshal the power mode configuration. */
	switch (to_avr.power_mode) {
		case AVR_RUN:
			/* Normal operating mode. First byte is 0, and the second (PWM
			 * frequency is set to 8.
			 */
			raw_to_avr[1] = 8;
			break;
		case AVR_POWER_OFF:
			/* Tell the AVR to shut us down. */
			raw_to_avr[0] = 0x5A;
			raw_to_avr[1] = 0;
			break;
		case AVR_RESET_MODE:
			/* Tell the AVR to boot SAM-BA. */
			raw_to_avr[0] = 0x5A;
			raw_to_avr[1] = 0xA5;
	}

	/* Marshal the motor speed settings. */
	raw_to_avr[2] = to_avr.motor_speed[0];
	raw_to_avr[3] = to_avr.motor_speed[1];
	raw_to_avr[4] = to_avr.motor_speed[2];

	/* raw_to_avr[5] is the value for the 4th motor, which doesn't
	 * exist. This is probably a bug in the AVR firmware, but it is
	 * required. So we just latch the value to zero.
	 */

	/* Marshal the motor brake settings. */
	raw_to_avr[6] = to_avr.motor_brake;

	/* Calculate the checksum. */
	for (i = 0; i < ((sizeof (raw_to_avr)) - 1); i++)
		checksum += raw_to_avr[i];
	raw_to_avr[(sizeof (raw_to_avr)) - 1] = ~checksum;
}

/* Small helper to convert two bytes into an uint16_t. */
static inline uint16_t unpack_word (uint8_t *word) {
	return *((uint16_t *)word);
}

/* Deserialize the AVR data structure in raw_from_avr into the
 * from_avr status structure.
 */
static void avr_unpack_from_avr (void)
{
	uint8_t checksum = 0;
	uint16_t word;
	uint32_t voltage;
	uint32_t i;
	uint8_t *p = raw_from_avr;

	/* Compute the checksum of the received data. This is done by doing
	 * the unsigned sum of all the bytes in the received buffer. They
	 * should add up to 0xFF.
	 */
	for (i =0; i < sizeof (raw_from_avr); i++)
		checksum += raw_from_avr[i];

	if (checksum != 0xff) {
		avr_state.failed_consecutive_checksums++;
		return;
	} else {
		avr_state.failed_consecutive_checksums = 0;
	}

	/* Unpack and store the 4 sensor analog readings. */
	for (i = 0; i < NXT_N_SENSORS; i++) {
		from_avr.adc_value[i] = unpack_word (p);
		p += 2;
	}

	/* Grab the buttons word (an analog reading), and compute the state
	 * of buttons from that.
	 */
	word = unpack_word (p);
	p += 2;

	if (word > 1023)
		from_avr.buttons = BUTTON_OK;
	else if (word > 720)
		from_avr.buttons = BUTTON_CANCEL;
	else if (word > 270)
		from_avr.buttons = BUTTON_RIGHT;
	else if (word > 60)
		from_avr.buttons = BUTTON_LEFT;
	else
		from_avr.buttons = BUTTON_NONE;

	/* Process the last word, which is a mix and match of many
	 * values.
	 */
	word = unpack_word (p);

	/* Extract the AVR firmware version, as well as the type of power
	 * supply connected.
	 */
	from_avr.version.major = (word >> 13) & 0x3;
	from_avr.version.minor = (word >> 10) & 0x7;
	from_avr.battery.is_aa = (word & 0x8000) ? TRUE : FALSE;

	/* The rest of the word is the voltage value, in units of
	 * 13.848mV. As the NXT does not have a floating point unit, the
	 * multiplication by 13.848 is approximated by a multiplication by
	 * 3545 followed by a division by 256.
	 */
	voltage = word & 0x3ff;
	voltage = (voltage * 3545) >> 9;
	from_avr.battery.charge = voltage;
}

/* Initialise the NXT-AVR communication. */
void avr_init (void)
{
	/* Set up the TWI driver to turn on the i2c bus, and kickstart the
	 * state machine to start transmitting.
	 */
	twi_init ();
	avr_state.mode = AVR_LINK_DOWN;
}

/* Initialise the NXT-AVR data structures. */
void avr_data_init (void)
{
	int i;

	avr_state.mode		= AVR_UNINITIALISED;
	avr_state.failed_consecutive_checksums = 0;
	
	to_avr.power_mode	= AVR_RUN;
	to_avr.motor_brake	= 0;
	for (i = 0; i < NXT_N_MOTORS; ++i)
		to_avr.motor_speed[i] = 0;
	
	twi_state.mode		= TWI_UNINITIALISED;
	twi_state.ptr		= NULL;
	twi_state.len		= 0;
}

/* The main AVR driver state machine. This routine gets called
 * periodically every millisecond by the system timer code.
 *
 * It is called directly in the main system timer interrupt, and so
 * must return as fast as possible.
 */
void avr_systick_update (void)
{
	/* The action taken depends on the state of the AVR
	 * communication.
	 */
	switch (avr_state.mode) {
		case AVR_UNINITIALISED:
			/* Because the system timer can call this update routine before
			 * the driver is initialized, we have this safe state. It does
			 * nothing and immediately returns.
			 *
			 * When the AVR driver initialization code runs, it will set the
			 * state to AVR_LINK_DOWN, which will kickstart the state machine.
			 */
			return;

		case AVR_LINK_DOWN:
			/* ARM-AVR link is not initialized. We need to send the hello
			 * string to tell the AVR that we are alive. This will (among
			 * other things) stop the "clicking brick" sound, and avoid having
			 * the brick powered down after a few minutes by an AVR that
			 * doesn't see us coming up.
			 */
			twi_write_async (AVR_ADDRESS, 
					(uint8_t *)avr_init_handshake,
					(sizeof (avr_init_handshake)) - 1);
			avr_state.failed_consecutive_checksums = 0;
			avr_state.mode = AVR_INIT;
			break;

		case AVR_INIT:
			/* Once the transmission of the handshake is complete, go into a 2
			 * millisecond wait, which is accomplished by the use of two
			 * intermediate state machine states.
			 */
			if (twi_ready ())
				avr_state.mode = AVR_WAIT_2MS;
			break;

		case AVR_WAIT_2MS:
			/* Wait another millisecond... */
			avr_state.mode = AVR_WAIT_1MS;
			break;

		case AVR_WAIT_1MS:
			/* Now switch the state to send mode, but also set the receive
			 * done flag. On the next refresh cycle, the communication will be
			 * in "production" mode, and will start by reading data back from
			 * the AVR.
			 */
			avr_state.mode = AVR_SEND;
			break;

		case AVR_SEND:
			/* If the transmission is complete, switch to receive mode and
			 * read the status structure from the AVR.
			 */
			if (twi_ready ()) {
				avr_state.mode = AVR_RECV;
				memset (raw_from_avr, 0, sizeof (raw_from_avr));
				twi_read_async (
					AVR_ADDRESS, 
					raw_from_avr,
					sizeof (raw_from_avr)
				);
			}

		case AVR_RECV:
			/* If the transmission is complete, unpack the read data into the
			 * from_avr struct, pack the data in the to_avr struct into a raw
			 * buffer, and shovel that over the i2c bus to the AVR.
			 */
			if (twi_ready ()) {
				avr_unpack_from_avr ();
				/* If the number of failed consecutive checksums is over the
				 * restart threshold, consider the link down and reboot the
				 * link. */
				if (avr_state.failed_consecutive_checksums >= AVR_MAX_FAILED_CHECKSUMS) {
					avr_state.mode = AVR_LINK_DOWN;
				} else {
					avr_state.mode = AVR_SEND;
					avr_pack_to_avr ();
					twi_write_async (
						AVR_ADDRESS, 
						raw_to_avr, 
						sizeof (raw_to_avr)
					);
				}
			}
			break;
	}
}
/*}}}*/

#if 0

uint32_t nx__avr_get_sensor_value(uint32_t n) {
  NX_ASSERT(n < NXT_N_SENSORS);

  return from_avr.adc_value[n];
}

void nx__avr_set_motor(uint32_t motor, int power_percent, bool brake) {
  NX_ASSERT(motor < NXT_N_MOTORS);

  to_avr.motor_speed[motor] = power_percent;
  if (brake)
    to_avr.motor_brake |= (1 << motor);
  else
    to_avr.motor_brake &= ~(1 << motor);
}

void nx__avr_power_down(void) {
  while (1)
    to_avr.power_mode = AVR_POWER_OFF;
}

void nx__avr_firmware_update_mode(void) {
  while (1)
    to_avr.power_mode = AVR_RESET_MODE;
}

nx_avr_button_t nx_avr_get_button(void) {
  return from_avr.buttons;
}

uint32_t nx_avr_get_battery_voltage(void) {
  return from_avr.battery.charge;
}

bool nx_avr_battery_is_aa(void) {
  return from_avr.battery.is_aa;
}

void nx_avr_get_version(uint8_t *major, uint8_t *minor) {
  if (major)
    *major = from_avr.version.major;
  if (minor)
    *minor = from_avr.version.minor;
}

#endif
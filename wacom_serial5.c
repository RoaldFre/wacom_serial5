/*
 * Wacom protocol 5 serial tablet driver
 *
 * This code is based on the Wacom protocol 4 serial tablet driver by 
 * Julian Squires <julian@cipht.net>.
 *
 * Protocol 5 is implemented by Roald Frederickx 
 * <roald.frederickx@gmail.com>.
 * As of now, I can only test it myself on an old Wacom Intuos1 with stylus 
 * and old 5-button mouse.
 *
 * Sections I have been unable to test personally due to lack of available
 * hardware are marked UNTESTED.  Much of what is marked UNTESTED comes from
 * old wcmSerial code in linuxwacom 0.9.0 or xf86-input-wacom.
 *  - The last tree of xf86-input-wacom that still has the serial code:
 *    f0c8aa9962e0238557d103baa4a5ba57484fd1c9
 *  - The commit that removed serial support:
 *    b3cba4e3543a98103282ba8fa55bf38012d23d9f
 *
 *
 * This driver was developed with reference to much code written by others,
 * particularly:
 *  - wacom protocol 4 serial tablet driver by Julian Squires 
 *    <julian@cipht.net>;
 *  - elo, gunze drivers by Vojtech Pavlik <vojtech@ucw.cz>;
 *  - wacom_w8001 driver by Jaya Kumar <jayakumar.lkml@gmail.com>;
 *  - the USB wacom input driver, credited to many people
 *    (see drivers/input/tablet/wacom.h);
 *  - new and old versions of linuxwacom / xf86-input-wacom credited to
 *    Frederic Lepied, France. <Lepied@XFree86.org> and
 *    Ping Cheng, Wacom. <pingc@wacom.com>;
 *  - and xf86wacom.c (a presumably ancient version of the linuxwacom 
 *    code), by Frederic Lepied and Raph Levien <raph@gtk.org>.
 */

/* XXX To be removed before (widespread) release. */
#define DEBUG

#include <linux/string.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/slab.h>
#include <linux/completion.h>

/* XXX To be removed before (widespread) release. */
#ifndef SERIO_WACOM_V
#define SERIO_WACOM_V 0x3e
#endif


/* TODO copied from the kernel's wacom.h for now */
#define USB_VENDOR_ID_WACOM	0x056a


#define DRIVER_AUTHOR	"Roald Frederickx <roald.frederickx@gmail.com>"
#define DEVICE_NAME	"Wacom protocol 5 serial tablet"
#define DRIVER_DESC	DEVICE_NAME " driver"
#define DRIVER_NAME	"wacom_serial5"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define REQUEST_MODEL_AND_ROM_VERSION	"~#\r"
#define REQUEST_MAX_COORDINATES		"~C\r"
#define REQUEST_CONFIGURATION_STRING	"~R\r"
#define REQUEST_RESET_TO_PROTOCOL_IV	"\r#\r"

#define COMMAND_START_SENDING_PACKETS		"ST\r"
#define COMMAND_STOP_SENDING_PACKETS		"SP\r"
#define COMMAND_SINGLE_MODE_INPUT		"MT0\r"
#define COMMAND_MULTI_MODE_INPUT		"MT1\r" /* MU1 for protocol4 */

#define COMMAND_ORIGIN_IN_UPPER_LEFT		"OC1\r"
#define COMMAND_ENABLE_ALL_MACRO_BUTTONS	"~M0\r"
#define COMMAND_DISABLE_GROUP_1_MACRO_BUTTONS	"~M1\r"
#define COMMAND_TRANSMIT_AT_MAX_RATE		"IT0\r"
#define COMMAND_DISABLE_INCREMENTAL_MODE	"IN0\r"
#define COMMAND_ENABLE_CONTINUOUS_MODE		"SR\r"
#define COMMAND_ENABLE_PRESSURE_MODE		"PH1\r"
#define COMMAND_Z_FILTER			"ZF1\r"

#define COMMAND_HEIGHT				"HT1\r"
#define COMMAND_ID				"ID1\r"

#define PACKET_LENGTH 9

#define MAX_Z ((1 << 10) - 1)

#define TILT_SIGN_BIT   0x40
#define TILT_BITS       0x3F
#define PROXIMITY_BIT   0x40

#if 0
/* device IDs from wacom_wac.h */
//TODO: properly include this header!
#define STYLUS_DEVICE_ID	0x02
#define TOUCH_DEVICE_ID         0x03
#define CURSOR_DEVICE_ID        0x06
#define ERASER_DEVICE_ID        0x0A
#define PAD_DEVICE_ID           0x0F
#endif

//TODO: find better/nicer way?
#define MOUSE_4D(id)     ((id & 0x07ff) == 0x0094)
#define MOUSE_2D(id)     ((id & 0x07ff) == 0x0007)
#define LENS_CURSOR(id)  ((id & 0x07ff) == 0x0096)


struct tool_state {
	int tool;		/* BTN_TOOL_XXX */
	int tool_id;		/* tool ID as received by hardware */
	int device_id;		/* device type *_DEVICE_ID */
	__u32 serial_num;	/* tool serial# as received by hardware */
	int proximity;
};

struct wacom {
	struct input_dev *dev;
	struct completion cmd_done;
	int idx;
	unsigned char data[32];
	char phys[32];
	struct tool_state tool_state[2]; /* state per channel */
};

enum {
	MODEL_INTUOS		= 0x4744, /* GD */
	MODEL_INTUOS2		= 0x5844, /* XD */
	MODEL_UNKNOWN           = 0
};

static void handle_model_response(struct wacom *wacom)
{
	int major_v, minor_v;
	char *p;

	dev_dbg(&wacom->dev->dev, "Model string: %s\n", wacom->data);

	major_v = minor_v = 0;
	p = strrchr(wacom->data, 'V');
	if (p)
		sscanf(p+1, "%u.%u", &major_v, &minor_v);

	switch (wacom->data[2] << 8 | wacom->data[3]) {
	case MODEL_INTUOS:
		p = "Intuos";
		wacom->dev->id.version = MODEL_INTUOS;
		input_abs_set_res(wacom->dev, ABS_X, 2540);
		input_abs_set_res(wacom->dev, ABS_Y, 2540);
		break;
	case MODEL_INTUOS2: /* Intuos 2 is UNTESTED */
		p = "Intuos2";
		wacom->dev->id.version = MODEL_INTUOS2;
		input_abs_set_res(wacom->dev, ABS_X, 2540);
		input_abs_set_res(wacom->dev, ABS_Y, 2540);
		input_set_abs_params(wacom->dev, ABS_THROTTLE, -1023, 1023, 0, 0);
			/* TODO: what other models have throttle? Does 
			 * intuos1 have this too? Dependent on which tool 
			 * you use? (mine doesn't... -- don't enable for 
			 * intuos1?) */
		break;
	default:		/* UNTESTED */
		dev_dbg(&wacom->dev->dev, "Didn't understand Wacom model "
				"string: \"%s\". Maybe you want the "
				"protocol IV driver instead of this one?\n",
				wacom->data);
		p = "Unknown Protocol V";
		wacom->dev->id.version = MODEL_UNKNOWN;
		break;
	}
	dev_info(&wacom->dev->dev, "Wacom tablet: %s, version %u.%u\n", p,
		 major_v, minor_v);
	input_set_abs_params(wacom->dev, ABS_PRESSURE, 0, MAX_Z, 0, 0);
	input_set_abs_params(wacom->dev, ABS_TILT_X,
					-(TILT_BITS + 1), TILT_BITS, 0, 0);
	input_set_abs_params(wacom->dev, ABS_TILT_Y,
					-(TILT_BITS + 1), TILT_BITS, 0, 0);
}


static void handle_configuration_response(struct wacom *wacom)
{
	int x, y, skip;

	dev_dbg(&wacom->dev->dev, "Configuration string: %s\n", wacom->data);
	sscanf(wacom->data, REQUEST_CONFIGURATION_STRING
			"%x,%u,%u,%u,%u", &skip, &skip, &skip, &x, &y);
	input_abs_set_res(wacom->dev, ABS_X, x);
	input_abs_set_res(wacom->dev, ABS_Y, y);
}

static void handle_coordinates_response(struct wacom *wacom)
{
	int x, y;

	dev_dbg(&wacom->dev->dev, "Coordinates string: %s\n", wacom->data);
	sscanf(wacom->data, REQUEST_MAX_COORDINATES"%u,%u", &x, &y);
	input_set_abs_params(wacom->dev, ABS_X, 0, x, 0, 0);
	input_set_abs_params(wacom->dev, ABS_Y, 0, y, 0, 0);
}

static void handle_response(struct wacom *wacom)
{
	if (wacom->data[0] != '~' || wacom->idx < 2) {
		dev_dbg(&wacom->dev->dev, "got a garbled response of length "
			                  "%d.\n", wacom->idx);
		return;
	}

	switch (wacom->data[1]) {
	case '#':
		handle_model_response(wacom);
		break;
	case 'R':
		handle_configuration_response(wacom);
		break;
	case 'C':
		handle_coordinates_response(wacom);
		break;
	default:
		dev_dbg(&wacom->dev->dev, "got an unexpected response: %s\n",
			wacom->data);
		break;
	}

	complete(&wacom->cmd_done);
}

static void send_position(struct input_dev *dev, char *data) {
	int x, y;
	x = ((data[1] & 0x7f) << 9) |
	    ((data[2] & 0x7f) << 2) |
	    ((data[3] & 0x60) >> 5);
	y = ((data[3] & 0x1f) << 11) |
	    ((data[4] & 0x7f) <<  4) |
	    ((data[5] & 0x78) >>  3);
	input_report_abs(dev, ABS_X, x);
	input_report_abs(dev, ABS_Y, y);
}

static void report_key(struct input_dev *dev, int buttons, int bit, int code) {
	input_report_key(dev, code, (buttons & (1 << bit)) >> bit);
}

static void send_buttons(struct input_dev *dev, int buttons, int is_stylus) {
	/* Reversed mappings of buttonmask to button codes.
	 * Found in wcmUSB.c of xf86-input-wacom, 
	 * tree: f0c8aa9962e0238557d103baa4a5ba57484fd1c9
	 * 
	 * commit 
	 * b3cba4e3543a98103282ba8fa55bf38012d23d9f
	 *
	 * bit	event code
	 * 0	BTN_LEFT
	 * 1	BTN_STYLUS or BTN_MIDDLE
	 * 2	BTN_STYLUS2 or BTN_RIGHT
	 * 3	BTN_SIDE
	 * 4	BTN_EXTRA
	 * not too sure of these:
	 * 5	BTN_FORWARD
	 * 6	BTN_BACK
	 * 7	BTN_TASK
	 *
	 * Note that we are doing "double" work here, as the wacom driver 
	 * will make the reverse transformation.
	 * We could probably in-line this so we only look at the relevant 
	 * bits that aren't masked anyway, but that leads to a bit of code 
	 * duplication. Let's hope the compiler is smart enough to do that 
	 * automagically.
	 */
	if (!is_stylus)
		report_key(dev, buttons, 0, BTN_LEFT);
		/* TODO: report BTN_TOUCH instead? -- however, bit 0 seems 
		 * to be 0 all the time */
	report_key(dev, buttons, 1, (is_stylus ?
					BTN_STYLUS : BTN_MIDDLE));
	report_key(dev, buttons, 2, (is_stylus ?
					BTN_STYLUS2 : BTN_RIGHT));
	report_key(dev, buttons, 3, BTN_SIDE);
	report_key(dev, buttons, 4, BTN_EXTRA);
	report_key(dev, buttons, 5, BTN_FORWARD);
	report_key(dev, buttons, 6, BTN_BACK);
	report_key(dev, buttons, 7, BTN_TASK);
}

static int tool_from_tool_id(int tool_id)
{
	/* The original old serial code masked the MSB from tool_id 
	 * (mask: 0x7ff). New code does not seem to do this. We don't 
	 * either.
	 * Code ripped from wacom_wac.c from the kernel and pruned for 
	 * Intuos and Intuos2 compatible tools only.  */
	switch (tool_id) {
	case 0x812: /* Inking pen */
	case 0x012:
		return BTN_TOOL_PENCIL;

	case 0x822: /* Pen */
	case 0x842:
	case 0x852:
	case 0x022:
		return BTN_TOOL_PEN;

	case 0x832: /* Stroke pen */
	case 0x032:
		return BTN_TOOL_BRUSH;

	case 0x007: /* Mouse 2D */
	case 0x094: /* Mouse 4D */
	case 0x09c: /* Not in old code -- not compatbile? */
		return BTN_TOOL_MOUSE;

	case 0x096: /* Lens cursor */
		return BTN_TOOL_LENS;

	case 0x82a: /* Eraser */
	case 0x85a:
	case 0x91a:
	case 0xd1a:
	case 0x0fa:
		return BTN_TOOL_RUBBER;

	case 0xd12:
	case 0x912:
	case 0x112:
		return BTN_TOOL_AIRBRUSH;

	default: /* Unknown tool */
		return BTN_TOOL_PEN;
	}
}

#if 0
static int device_type_from_tool(int tool)
{
	switch (tool) {
	case BTN_TOOL_RUBBER:
		return ERASER_DEVICE_ID;

	case BTN_TOOL_PENCIL:
	case BTN_TOOL_PEN:
	case BTN_TOOL_BRUSH:
	case BTN_TOOL_AIRBRUSH:
		return STYLUS_DEVICE_ID;

	case BTN_TOOL_MOUSE:
	case BTN_TOOL_LENS:
		return CURSOR_DEVICE_ID;

	default: /* Unknown tool */
		return STYLUS_DEVICE_ID;
	}
}
#endif

static void out_of_proximity_reset(struct input_dev *dev,
						struct tool_state *state)
{
	/* Don't reset state if we already did so (= we already are out of 
	 * prox). Otherwise we have problems with kernel event filtering 
	 * (BTN_TOOL events remain 0 and get filtered out) when we have two 
	 * tools on the tablet (the reset events will be assigned to the 
	 * other tool if that one is still in prox). */
	if (!state->proximity)
		return;

	state->proximity = 0;

	/* Reset everything, otherwise we lose the initial states
	 * when in-prox next time */
	input_report_abs(dev, ABS_X, 0);
	input_report_abs(dev, ABS_Y, 0);
	input_report_abs(dev, ABS_DISTANCE, 0);
	input_report_abs(dev, ABS_TILT_X, 0);
	input_report_abs(dev, ABS_TILT_Y, 0);
	if (state->tool >= BTN_TOOL_MOUSE) { //XXX not so nice...
		input_report_key(dev, BTN_LEFT, 0);
		input_report_key(dev, BTN_MIDDLE, 0);
		input_report_key(dev, BTN_RIGHT, 0);
		input_report_key(dev, BTN_SIDE, 0);
		input_report_key(dev, BTN_EXTRA, 0);
		input_report_abs(dev, ABS_THROTTLE, 0);
		//input_report_abs(dev, ABS_RZ, 0);
	} else {
		input_report_abs(dev, ABS_PRESSURE, 0);
		input_report_key(dev, BTN_STYLUS, 0);
		input_report_key(dev, BTN_STYLUS2, 0);
		//input_report_key(dev, BTN_TOUCH, 0);
		input_report_abs(dev, ABS_WHEEL, 0);
	}
}

static int handle_proximity_bit(struct input_dev *dev,
					char *data, struct tool_state *state)
{
	int proximity;

	proximity = (data[0] & PROXIMITY_BIT);
	if (!proximity) {
		out_of_proximity_reset(dev, state);
	} else {
		state->proximity = 1;
	}

	return proximity;
}


static void handle_general_stylus_packet(struct input_dev *dev,
					char *data, struct tool_state *state)
{
	int z, buttons, abswheel, tiltx, tilty;

	if (!handle_proximity_bit(dev, data, state))
		return;

	send_position(dev, data);

	if ((data[0] & 0xb8) == 0xa0) {
		z = (((data[5] & 0x07) << 7) | (data[6] & 0x7f));
		input_report_abs(dev, ABS_PRESSURE, z);

		buttons = (data[0] & 0x06);
		send_buttons(dev, buttons, 1);
	}
	else {
		abswheel = (((data[5] & 0x07) << 7) |
			(data[6] & 0x7f));
		//TODO: report wheel
	}

	tiltx = (data[7] & TILT_BITS);
	tilty = (data[8] & TILT_BITS);
	if (data[7] & TILT_SIGN_BIT)
		tiltx -= (TILT_BITS + 1);
	if (data[8] & TILT_SIGN_BIT)
		tilty -= (TILT_BITS + 1);

	/* TODO: xf86-wacom-input assumes a mintilt of 0 and only positive 
	 * tilt values -- fix there or here? (here for now) */
	input_report_abs(dev, ABS_TILT_X, tiltx + TILT_BITS + 1);
	input_report_abs(dev, ABS_TILT_Y, tilty + TILT_BITS + 1);
}

static void handle_device_id_packet(char *data, struct tool_state *state)
{
	int tool_id, tool;
	state->proximity = 0; /* Don't enable it here, yet. Let a packet 
				 with an actual valid position etc do it. */
	
	state->serial_num = ((data[2] & 0x03) << 30) |
			((data[3] & 0x7f) << 23) |
			((data[4] & 0x7f) << 16) |
			((data[5] & 0x7f) <<  9) |
			((data[6] & 0x7f) <<  2) |
			((data[7] & 0x60) >>  5);

	tool_id = ((data[1] & 0x7f) << 5) | 
			((data[2] & 0x7c) >> 2);
	state->tool_id = tool_id;

	tool = tool_from_tool_id(tool_id);
	state->tool = tool;
	
	//state->device_type = device_type_from_tool(tool);
}

static void handle_first_cursor_packet(struct input_dev *dev, 
					char *data, struct tool_state *state)
{
	int throttle, buttons, relwheel;

	if (!handle_proximity_bit(dev, data, state))
		return;

	send_position(dev, data);

	/* 4D mouse */ //UNTESTED
	if (MOUSE_4D(state->tool_id)) {
		throttle = (((data[5] & 0x07) << 7) |
			(data[6] & 0x7f));
		if (data[8] & 0x08)
			throttle = -throttle;
		input_report_abs(dev, ABS_THROTTLE, throttle);
	}

	/* Lens cursor */
	else if (LENS_CURSOR(state->tool_id)) {
		buttons = data[8];
		send_buttons(dev, buttons, 0);
	}

	/* 2D mouse */ //UNTESTED
	else if (MOUSE_2D(state->tool_id)) {
		buttons = (data[8] & 0x1C) >> 2;
		send_buttons(dev, buttons, 0);
		relwheel = - (data[8] & 1) +
				((data[8] & 2) >> 1);
		input_report_rel(dev, REL_WHEEL, relwheel);
	}
}

//UNTESTED
static void handle_second_cursor_packet(struct input_dev *dev, 
					char *data, struct tool_state *state)
{
	int rotation;
	
	if (!handle_proximity_bit(dev, data, state))
		return;

	send_position(dev, data);

	rotation = (((data[6] & 0x0f) << 7) |
		(data[7] & 0x7f));
	if (rotation < 900)
		rotation = -rotation;
	else 
		rotation = 1799 - rotation;

	//TODO: send rotation
}

static void handle_packet(struct wacom *wacom)
{
	struct input_dev *dev = wacom->dev;
	unsigned char *data = wacom->data;
	int channel = data[0] & 1;
	struct tool_state *state = &wacom->tool_state[channel];

	/* Device ID packet */
	if ((data[0] & 0xfc) == 0xc0) {
		handle_device_id_packet(data, state);
	}

	else if (state->tool_id == 0)
		return; /* Eek! We don't know the current tool yet! */

	/* Out of proximity packet */
	else if ((data[0] & 0xfe) == 0x80) {
		out_of_proximity_reset(dev, state);
		state->device_id = 0; // XXX?
	}
	/* General pen packet or eraser packet or airbrush first packet
	 * airbrush second packet */
	else if (((data[0] & 0xb8) == 0xa0) || ((data[0] & 0xbe) == 0xb4)) {
		handle_general_stylus_packet(dev, data, state);
	}
	/* 4D mouse 1st packet or Lens cursor packet or 2D mouse packet*/
	else if (((data[0] & 0xbe) == 0xa8) || ((data[0] & 0xbe) == 0xb0)) {
		handle_first_cursor_packet(dev, data, state);
	}
	/* 4D mouse 2nd packet */
	else if ((data[0] & 0xbe) == 0xaa) {
		handle_second_cursor_packet(dev, data, state);
	}
	else {
		dev_info(&dev->dev,
				"Received unknown protocol V packet type!\n");
		return;
	}

	//input_report_abs(dev, ABS_MISC, state->tool_id);
	input_report_key(dev, state->tool, state->proximity);
	input_event(dev, EV_MSC, MSC_SERIAL, state->serial_num);
	input_sync(dev);
}

static irqreturn_t wacom_interrupt(struct serio *serio, unsigned char data,
				   unsigned int flags)
{
	struct wacom *wacom = serio_get_drvdata(serio);

	if (wacom == NULL) {
		printk(KERN_ERR DRIVER_NAME ": Something went VERY WRONG!\n");
		return IRQ_HANDLED;
	}

	if (data & 0x80)
		wacom->idx = 0;
	if (wacom->idx >= sizeof(wacom->data)) {
		dev_dbg(&wacom->dev->dev, "throwing away %d bytes of garbage\n",
			wacom->idx);
		wacom->idx = 0;
	}

	wacom->data[wacom->idx++] = data;

	/* we're either expecting a carriage return-terminated ASCII
	 * response string, or a seven-byte packet with the MSB set on
	 * the first byte */
	if (wacom->idx == PACKET_LENGTH && (wacom->data[0] & 0x80)) {
		handle_packet(wacom);
		wacom->idx = 0;
	} else if (data == '\r' && !(wacom->data[0] & 0x80)) {
		wacom->data[wacom->idx-1] = 0;
		handle_response(wacom);
		wacom->idx = 0;
	}
	return IRQ_HANDLED;
}

static void wacom_disconnect(struct serio *serio)
{
	struct wacom *wacom = serio_get_drvdata(serio);

	serio_close(serio);
	serio_set_drvdata(serio, NULL);
	input_unregister_device(wacom->dev);
	kfree(wacom);
}

static int wacom_send(struct serio *serio, const char *command)
{
	int err = 0;
	for (; !err && *command; command++)
		err = serio_write(serio, *command);
	return err;
}

static int send_setup_string(struct wacom *wacom, struct serio *serio)
{
	const char *s;
	switch (wacom->dev->id.version) {
	case MODEL_INTUOS:
	case MODEL_INTUOS2: /* UNTESTED, but should be the same */
		s = COMMAND_MULTI_MODE_INPUT
			COMMAND_ID
			COMMAND_TRANSMIT_AT_MAX_RATE
			COMMAND_START_SENDING_PACKETS;
	default:
		/* TODO: remove this all together? All protocol5 tablets 
		 * seem to use the string above.*/
		s = COMMAND_MULTI_MODE_INPUT
			COMMAND_ID
			COMMAND_ORIGIN_IN_UPPER_LEFT
			COMMAND_ENABLE_ALL_MACRO_BUTTONS
			COMMAND_DISABLE_GROUP_1_MACRO_BUTTONS
			COMMAND_TRANSMIT_AT_MAX_RATE
			COMMAND_DISABLE_INCREMENTAL_MODE
			COMMAND_ENABLE_CONTINUOUS_MODE
			COMMAND_Z_FILTER
			COMMAND_START_SENDING_PACKETS;
		break;
	}
	return wacom_send(serio, s);
}

static int wacom_setup(struct wacom *wacom, struct serio *serio)
{
	int err;
	unsigned long u;

	/* Note that setting the link speed is the job of inputattach.
	 * We assume that reset negotiation has already happened,
	 * here. */
	err = wacom_send(serio, COMMAND_STOP_SENDING_PACKETS);
	if (err)
		return err;

	init_completion(&wacom->cmd_done);
	err = wacom_send(serio, REQUEST_MODEL_AND_ROM_VERSION);
	if (err)
		return err;

	u = wait_for_completion_timeout(&wacom->cmd_done, HZ);
	if (u == 0) {
		dev_info(&wacom->dev->dev, "Timed out waiting for tablet to "
			 "respond with model and version.\n");
		return -EIO;
	}

	init_completion(&wacom->cmd_done);

	/* My Intuos tablet won't respond to a configuration request, so it 
	 * seems
	 * TODO: Intuos2 does work? Or remove alltogether (seems to be 
	 * removed altogether in older code) */
	if (wacom->dev->id.version != MODEL_INTUOS) {
		err = wacom_send(serio, REQUEST_CONFIGURATION_STRING);
		if (err)
			return err;

		u = wait_for_completion_timeout(&wacom->cmd_done, HZ);
		if (u == 0) {
			dev_info(&wacom->dev->dev, "Timed out waiting for "
					"tablet to respond with "
					"configuration string.\n");
			return -EIO;
		}
	}

	init_completion(&wacom->cmd_done);
	err = wacom_send(serio, REQUEST_MAX_COORDINATES);
	if (err)
		return err;
	wait_for_completion_timeout(&wacom->cmd_done, HZ);

	return send_setup_string(wacom, serio);
}

static int wacom_connect(struct serio *serio, struct serio_driver *drv)
{
	struct wacom *wacom;
	struct input_dev *input_dev;
	int err = -ENOMEM;

	wacom = kzalloc(sizeof(struct wacom), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!wacom || !input_dev)
		goto fail0;

	wacom->dev = input_dev;
	snprintf(wacom->phys, sizeof(wacom->phys), "%s/input0", serio->phys);

	input_dev->name = DEVICE_NAME;
	input_dev->phys = wacom->phys;
	input_dev->id.bustype = BUS_RS232;
#if 0
	input_dev->id.vendor  = SERIO_WACOM_V;
	input_dev->id.product = serio->id.extra;
#else
	input_dev->id.vendor  = USB_VENDOR_ID_WACOM;
	input_dev->id.product = 0x24;
#endif
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &serio->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY)
			    | BIT_MASK(EV_ABS)
			    | BIT_MASK(EV_REL);
	__set_bit(BTN_LEFT,		input_dev->keybit);
	__set_bit(BTN_MIDDLE,		input_dev->keybit);
	__set_bit(BTN_RIGHT,		input_dev->keybit);
	__set_bit(BTN_SIDE,		input_dev->keybit);
	__set_bit(BTN_EXTRA,		input_dev->keybit);
	__set_bit(BTN_FORWARD,		input_dev->keybit);
	__set_bit(BTN_BACK,		input_dev->keybit);
	__set_bit(BTN_TASK,		input_dev->keybit);
	__set_bit(BTN_STYLUS,		input_dev->keybit);
	__set_bit(BTN_STYLUS2,		input_dev->keybit);
	__set_bit(BTN_TOOL_PEN,		input_dev->keybit);
	__set_bit(BTN_TOOL_RUBBER,	input_dev->keybit);
	__set_bit(BTN_TOOL_MOUSE,	input_dev->keybit);
	__set_bit(BTN_TOOL_LENS,	input_dev->keybit);

	__set_bit(ABS_MISC,		input_dev->absbit);

	input_set_capability(input_dev, EV_MSC, MSC_SERIAL);

	serio_set_drvdata(serio, wacom);

	err = serio_open(serio, drv);
	if (err)
		goto fail1;

	err = wacom_setup(wacom, serio);
	if (err)
		goto fail2;

	err = input_register_device(wacom->dev);
	if (err)
		goto fail2;

	return 0;

 fail2:	serio_close(serio);
 fail1:	serio_set_drvdata(serio, NULL);
 fail0:	input_free_device(input_dev);
	kfree(wacom);
	return err;
}

static struct serio_device_id wacom_serio_ids[] = {
	{
		.type	= SERIO_RS232,
		.proto	= SERIO_WACOM_V,
		.id	= SERIO_ANY,
		.extra	= SERIO_ANY,
	},
	{ 0 }
};

MODULE_DEVICE_TABLE(serio, wacom_serio_ids);

static struct serio_driver wacom_drv = {
	.driver		= {
		.name 	= DRIVER_NAME,
	},
	.description	= DRIVER_DESC,
	.id_table	= wacom_serio_ids,
	.interrupt	= wacom_interrupt,
	.connect	= wacom_connect,
	.disconnect	= wacom_disconnect,
};

static int __init wacom_init(void)
{
	return serio_register_driver(&wacom_drv);
}

static void __exit wacom_exit(void)
{
	serio_unregister_driver(&wacom_drv);
}

module_init(wacom_init);
module_exit(wacom_exit);

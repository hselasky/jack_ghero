/*-
 * Copyright (c) 2011 Hans Petter Selasky <hselasky@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * A few parts of this file has been copied from Edward Tomasz
 * Napierala's jack-keyboard sources.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <err.h>
#include <sysexits.h>

#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>

#include <usbhid.h>

#include <dev/usb/usbhid.h>

#define	PACKAGE_NAME		"jack_ghero"
#define	PACKAGE_VERSION		"1.0"
#define	BUFFER_SIZE		256	/* bytes */

#define	BUTTON_ORANGE 0x20
#define	BUTTON_BLUE 0x08
#define	BUTTON_YELLOW 0x10
#define	BUTTON_RED 0x04
#define	BUTTON_GREEN 0x02
#define	BUTTON_BACK 0x80
#define	BUTTON_START 0x100
#define	BUTTON_UP 0x10000
#define	BUTTON_DOWN 0x20000
#define	BUTTON_MAX 32

static jack_port_t *output_port;
static jack_client_t *jack_client;
static int read_fd = -1;
static int background;
static pthread_mutex_t ghero_mtx;
static const char *hid_name = "/dev/uhid0";
static report_desc_t hid_desc;
static struct hid_item hid_buttons[BUTTON_MAX];
static uint8_t hid_have_button[BUTTON_MAX];
static struct hid_item hid_volume[1];
static uint8_t hid_have_volume[1];
static int base_key = 72;
static int cmd_key = 36;
static int sustain;

#ifdef HAVE_DEBUG
#define	DPRINTF(fmt, ...) printf("%s:%d: " fmt, __FUNCTION__, __LINE__,## __VA_ARGS__)
#else
#define	DPRINTF(fmt, ...) do { } while (0)
#endif

static void
ghero_lock()
{
	pthread_mutex_lock(&ghero_mtx);
}

static void
ghero_unlock()
{
	pthread_mutex_unlock(&ghero_mtx);
}

static jack_nframes_t
ghero_write_data(jack_nframes_t t, jack_nframes_t nframes, void *buf, uint8_t *mbuf, int len)
{
	uint8_t *buffer;

	DPRINTF("Writing buffer %p, %d\n", mbuf, len);

#ifdef JACK_MIDI_NEEDS_NFRAMES
	buffer = jack_midi_event_reserve(buf, t, len, nframes);
#else
	buffer = jack_midi_event_reserve(buf, t, len);
#endif
	if (buffer == NULL) {
		DPRINTF("jack_midi_event_reserve() failed, "
		    "MIDI event lost\n");
		return (t);
	}
	memcpy(buffer, mbuf, len);

	DPRINTF("Buffer written\n");

	return (t + 1);
}

static void
ghero_read(jack_nframes_t nframes)
{
	void *buf;
	jack_nframes_t t;
	static uint8_t data[BUFFER_SIZE];
	static uint32_t old_value;
	uint8_t mbuf[8];
	uint32_t delta;
	int len;
	int i;
	int volume;
	uint32_t value;

	if (output_port == NULL)
		return;

	buf = jack_port_get_buffer(output_port, nframes);
	if (buf == NULL) {
		DPRINTF("jack_port_get_buffer() failed, cannot send anything.\n");
		return;
	}
#ifdef JACK_MIDI_NEEDS_NFRAMES
	jack_midi_clear_buffer(buf, nframes);
#else
	jack_midi_clear_buffer(buf);
#endif

	t = 0;
	ghero_lock();
	if (read_fd > -1) {
		while (1) {
			len = read(read_fd, data, sizeof(data));
			if (len > -1) {

				value = 0;
				for (i = 0; i != BUTTON_MAX; i++) {
					if (hid_have_button[i] && hid_get_data(data, &hid_buttons[i]))
						value |= (1 << i);
				}
				if (hid_have_volume[0])
					volume = hid_get_data(data, &hid_volume[0]);
				else
					volume = 0;

				if (volume < -32000)
					volume = -32000;
				else if (volume > 32000)
					volume = 32000;

				volume = 80 + (((127 - 80) * volume) / 32000);

				delta = old_value ^ value;
				old_value = value;

				if (value != 0)
					DPRINTF("value = 0x%08x, volume = %d\n", value, volume);

				if (delta & (BUTTON_DOWN | BUTTON_UP)) {
					mbuf[0] = 0x90;
					mbuf[1] = base_key + 0;
					mbuf[2] = (value & (BUTTON_DOWN | BUTTON_UP)) ? volume : 0;
					t = ghero_write_data(t, nframes, buf, mbuf, 3);
				}
				if (delta & BUTTON_ORANGE) {
					sustain = (value & BUTTON_ORANGE) ? 1 : 0;
					mbuf[0] = 0xB0;
					mbuf[1] = 0x40;
					mbuf[2] = sustain ? 127 : 0;
					t = ghero_write_data(t, nframes, buf, mbuf, 3);
				}
				if (delta & BUTTON_BLUE) {
					mbuf[0] = 0x90;
					mbuf[1] = cmd_key + 0;
					mbuf[2] = (value & BUTTON_BLUE) ? 127 : 0;
					t = ghero_write_data(t, nframes, buf, mbuf, 3);
				}
				if (delta & BUTTON_YELLOW) {
					mbuf[0] = 0x90;
					mbuf[1] = cmd_key + 1;
					mbuf[2] = (value & BUTTON_YELLOW) ? 127 : 0;
					t = ghero_write_data(t, nframes, buf, mbuf, 3);
				}
				if (delta & BUTTON_RED) {
					mbuf[0] = 0x90;
					mbuf[1] = cmd_key + 2;
					mbuf[2] = (value & BUTTON_RED) ? 127 : 0;
					t = ghero_write_data(t, nframes, buf, mbuf, 3);
				}
				if (delta & BUTTON_GREEN) {
					mbuf[0] = 0x90;
					mbuf[1] = cmd_key + 3;
					mbuf[2] = (value & BUTTON_GREEN) ? 127 : 0;
					t = ghero_write_data(t, nframes, buf, mbuf, 3);
				}
			} else {
				break;
			}
		}
	}
	ghero_unlock();
}

static int
ghero_process_callback(jack_nframes_t nframes, void *reserved)
{
	/*
	 * Check for impossible condition that actually happened to me,
	 * caused by some problem between jackd and OSS4.
	 */
	if (nframes <= 0) {
		DPRINTF("Process callback called with nframes = 0\n");
		return (0);
	}
	ghero_read(nframes);

	return (0);
}

static void
ghero_jack_shutdown(void *arg)
{
	exit(0);
}

static void *
ghero_watchdog(void *arg)
{
	struct hid_data *d;
	struct hid_item h;
	int fd;

	while (1) {

		if (hid_name == NULL) {
			/* do nothing */
		} else if (read_fd < 0) {
			fd = open(hid_name, O_RDWR | O_NONBLOCK);
			if (fd > -1) {
				hid_desc = hid_get_report_desc(fd);

				memset(hid_have_button, 0, sizeof(hid_have_button));
				memset(hid_have_volume, 0, sizeof(hid_have_volume));

				d = hid_start_parse(hid_desc, 1 << hid_input, -1);
				if (d != NULL) {

					while (hid_get_item(d, &h) != 0) {
						if (h.kind == hid_input) {

							if (HID_PAGE(h.usage) == HUP_BUTTON) {
								int value;

								value = HID_USAGE(h.usage);
								if ((value >= 0) && (value < 16)) {
									hid_have_button[value] = 1;
									hid_buttons[value] = h;
								}
							}
							if (HID_PAGE(h.usage) == HUP_GENERIC_DESKTOP) {
								int value;

								value = HID_USAGE(h.usage);

								DPRINTF("value = 0x%08x\n", value);

								switch (value) {
								case 0x90:
								case 0x91:
								case 0x92:
								case 0x93:
									value = 16 + (value & 3);
									hid_have_button[value] = 1;
									hid_buttons[value] = h;
									break;
								case 0x34:
									hid_volume[0] = h;
									hid_have_volume[0] = 1;
									break;
								default:
									break;
								}
							}
						}
					}
					hid_end_parse(d);
				}
				ghero_lock();
				read_fd = fd;
				ghero_unlock();
			}
		} else {
			ghero_lock();
			if (fcntl(read_fd, F_SETFL, (int)O_NONBLOCK) == -1) {
				DPRINTF("Close read\n");
				close(read_fd);
				read_fd = -1;
			}
			ghero_unlock();
		}

		usleep(1000000);
	}
}

static void
usage()
{
	fprintf(stderr,
	    "jack_ghero - GuitarHero to MIDI client v" PACKAGE_VERSION "\n"
	    "	-d /dev/uhid0 (set USB device)\n"
	    "	-B (run in background)\n"
	    "	-b 72 (base play key - C6)\n"
	    "	-c 36 (base command key - C3)\n"
	    "	-h (show help)\n");
	exit(0);
}

static void
ghero_pipe(int dummy)
{

}

int
main(int argc, char **argv)
{
	int error;
	int c;
	const char *pname;
	char devname[64];

	while ((c = getopt(argc, argv, "b:c:Bd:h")) != -1) {
		switch (c) {
		case 'b':
			base_key = atoi(optarg);
			if (base_key < 0 || base_key > (127 - 12)) {
				errx(EX_UNAVAILABLE, "Invalid base key value.");
			}
			break;
		case 'c':
			cmd_key = atoi(optarg);
			if (cmd_key < 0 || cmd_key > (127 - 12)) {
				errx(EX_UNAVAILABLE, "Invalid command key value.");
			}
			break;
		case 'B':
			background = 1;
			break;
		case 'd':
			hid_name = optarg;
			break;
		case 'h':
		default:
			usage();
		}
	}

	if (hid_name == NULL)
		usage();

	if (background) {
		if (daemon(0, 0))
			errx(EX_UNAVAILABLE, "Could not become daemon");
	}
	signal(SIGPIPE, ghero_pipe);

	pthread_mutex_init(&ghero_mtx, NULL);

	if (strncmp(hid_name, "/dev/", 5) == 0)
		pname = hid_name + 5;
	else
		pname = hid_name;

	snprintf(devname, sizeof(devname), PACKAGE_NAME "-%s", pname);

	jack_client = jack_client_open(devname,
	    JackNoStartServer, NULL);
	if (jack_client == NULL) {
		errx(EX_UNAVAILABLE, "Could not connect "
		    "to the JACK server. Run jackd first?");
	}
	error = jack_set_process_callback(jack_client,
	    ghero_process_callback, 0);
	if (error) {
		errx(EX_UNAVAILABLE, "Could not register "
		    "JACK process callback.");
	}
	jack_on_shutdown(jack_client, ghero_jack_shutdown, 0);

	output_port = jack_port_register(
	    jack_client, "TX", JACK_DEFAULT_MIDI_TYPE,
	    JackPortIsOutput, 0);

	if (output_port == NULL) {
		errx(EX_UNAVAILABLE, "Could not "
		    "register JACK output port.");
	}

	if (jack_activate(jack_client))
		errx(EX_UNAVAILABLE, "Cannot activate JACK client.");

	/* cleanup any stale connections */
	jack_port_disconnect(jack_client, output_port);

	ghero_watchdog(NULL);

	return (0);
}

/*
 * nvtispflash - a basic ISP programmer for Nuvoton N76E0003 chips
 * Copyright 2021 Frank Zago
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <err.h>
#include <libserialport.h>

#include "nvtispflash.h"

/* Default timeout for reading and writing commands */
#define SERIAL_TIMEOUT 5000

/* LDROM/APROM sizes, from LDSIZE config bits, for N76003 */
static const struct {
	int ldrom_size;
	int aprom_size;
} ldsize[8] = {
	{ 4, 14 }, { 4, 14 }, { 4, 14 }, { 4, 14 },
	{ 3, 15 }, { 2, 16 }, { 1, 17 }, { 0, 18 }
};

/* Compute the sum of all bytes of a command. The response checksum
 * must match it. */
static uint32_t calc_checksum(const struct pkt_cmd *cmd)
{
	unsigned char *p = (unsigned char *)cmd;
	uint32_t sum = 0;
	int i;

	for (i = 0; i < 64; i++)
		sum += *p++;

	return sum;
}

static int send_cmd(struct dev *dev, struct pkt_cmd *cmd)
{
	int rc;

	cmd->pkt_num = dev->pkt_num;
	dev->checksum = calc_checksum(cmd);

	rc = sp_blocking_write(dev->sp, cmd, sizeof(*cmd), 5000);
	if (rc != sizeof(*cmd))
		return -ETIMEDOUT;

	sp_drain(dev->sp);

	dev->pkt_num++;

	return 0;
}

static int read_response(struct dev *dev, int timeout_ms)
{
	int rc;
	void *p = &dev->ack;
	int len = sizeof(dev->ack);

	if (timeout_ms)
		rc = sp_blocking_read(dev->sp, p, len, timeout_ms);
	else
		rc = sp_nonblocking_read(dev->sp, p, len);
	if (rc == len) {
		if (dev->ack.pkt_num != dev->pkt_num) {
			printf("bad reply pkt_num: %u vs. %u\n", dev->ack.pkt_num, dev->pkt_num);
			return -EIO;
		}

		if (dev->ack.checksum != dev->checksum) {
			printf("bad checksum %x vs %x\n", dev->ack.checksum, dev->checksum);
			return -EIO;
		}

		return 0;
	}

	return -ETIMEDOUT;
}

/* Initiate connection to device. Issue the connect command every 40ms
 * until the device respond. */
static int dev_connect(struct dev *dev)
{
	struct pkt_cmd cmd = {};
	int rc;

	cmd.cmd = CMD_CONNECT;

	while (1)
	{
		rc = send_cmd(dev, &cmd);
		if (rc)
			return rc;

		/* NuMicro manual says 40ms between each tries */
		usleep(40000);

		rc = read_response(dev, 0);
		if (rc == 0)
			break;
	}

	return rc;
}

/* Several commands don't have parameters, so share some code. */
static int generic_command(struct dev *dev, uint32_t opcode)
{
	struct pkt_cmd cmd = {
		.cmd = opcode
	};
	int rc;

	rc = send_cmd(dev, &cmd);
	if (rc)
		return rc;

	rc = read_response(dev, SERIAL_TIMEOUT);
	if (rc)
		return rc;

	return 0;
}

static int dev_sync_packno(struct dev *dev)
{
	struct pkt_cmd cmd = {};
	int rc;

	cmd.cmd = CMD_SYNC_PACKNO;
	cmd.sync_packno.rn = cmd.pkt_num;

	rc = send_cmd(dev, &cmd);
	if (rc)
		return rc;

	rc = read_response(dev, SERIAL_TIMEOUT);
	if (rc)
		return rc;

	return 0;
}

static int dev_reset(struct dev *dev)
{
	struct pkt_cmd cmd = {};
	int rc;

	cmd.cmd = CMD_RESET;

	rc = send_cmd(dev, &cmd);
	if (rc)
		return rc;

	printf("Device reset\n");

	return 0;
}

static int dev_run_aprom(struct dev *dev)
{
	struct pkt_cmd cmd = {
		.cmd = CMD_RUN_APROM,
	};

	return send_cmd(dev, &cmd);
}

static void decode_config(const union config_bytes *config)
{
	printf("Config:\n");
	printf("  LOCK: %u\n", config->lock);
	printf("  RPD: %u\n", config->rpd);
	printf("  OCDEN: %u\n", config->ocden);
	printf("  OCDPWM: %u\n", config->ocdpwm);
	printf("  CBS: %u\n", config->cbs);
	printf("  LDSIZE: LDROM=%uK, APROM=%uK\n",
	       ldsize[config->ldsize].ldrom_size,
	       ldsize[config->ldsize].aprom_size);
	printf("  CBORST:%u\n", config->cborst);
	printf("  BOIAP:%u\n", config->boiap);
	printf("  CBOV:%u\n", config->cbov);
	printf("  CBODEN:%u\n", config->cboden);
	printf("  WDTEN:%u\n", config->wdten);
}

int set_new_config_options(struct dev *dev)
{
	union config_bytes config_update;
	struct pkt_cmd cmd = {};
	bool changes = false;
	int rc;
	int i;

	for (i = 0; i < sizeof(union config_bytes); i++) {
		config_update.raw[i] = dev->config_current.raw[i] & ~dev->config_mask.raw[i];
		config_update.raw[i] |= dev->config_new.raw[i];

		if (config_update.raw[i] != dev->config_current.raw[i])
			changes = true;
	}

	/* Avoid programming the config bits if nothing has
	 * changed. This is not an error. */
	if (!changes) {
		printf("No config changes\n");
		return 0;
	}

	/* Program the new config */
	cmd.cmd = CMD_UPDATE_CONFIG;
	cmd.update_config.new = config_update;

	rc = send_cmd(dev, &cmd);
	if (rc)
		return rc;

	rc = read_response(dev, SERIAL_TIMEOUT);
	if (rc)
		return rc;

	/* Read the new config */
	rc = generic_command(dev, CMD_READ_CONFIG);
	if (rc)
		return -EIO;

	dev->config_current = dev->ack.read_config;

	printf("New config options:\n");
	decode_config(&dev->config_current);

	return 0;
}

static int dev_update_aprom(struct dev *dev)
{
	int fd;
	struct stat statbuf;
	char buf[18 * 1024];
	bool first = true;
	int total_length;
	int to_copy;
	int rc;
	void *p;

	fd = open(dev->aprom_file, O_RDONLY);
	if (fd == -1)
		return -errno;

	rc = fstat(fd, &statbuf);
	if (rc == -1)
		return -errno;

	if (statbuf.st_size == 0)
		return -EBADF;

	if (statbuf.st_size > dev->aprom_size)
		return -E2BIG;

	/* TODO: loop until all read, or use mmap instead. */
	rc = read(fd, buf, statbuf.st_size);
	if (rc != statbuf.st_size)
		return -EIO;

	p = buf;
	total_length = statbuf.st_size;

	while (total_length) {
		struct pkt_cmd cmd = {};

		if (first) {
			first = false;

			cmd.cmd = CMD_UPDATE_APROM;
			cmd.update_aprom.start_addr = 0x0000;
			cmd.update_aprom.total_length = total_length;

			if (total_length > sizeof(cmd.update_aprom.data))
				to_copy = sizeof(cmd.update_aprom.data);
			else
				to_copy = total_length;

			memcpy(cmd.update_aprom.data, p, to_copy);
		} else {
			cmd.cmd = 0;

			if (total_length > sizeof(cmd.update_aprom2.data))
				to_copy = sizeof(cmd.update_aprom2.data);
			else
				to_copy = total_length;

			memcpy(cmd.update_aprom2.data, p, to_copy);
		}

		printf("sending block of %d bytes, from offset 0x%lx\n",
		       to_copy, (ptrdiff_t)p - (ptrdiff_t)buf);

		rc = send_cmd(dev, &cmd);
		if (rc)
			return rc;

		rc = read_response(dev, SERIAL_TIMEOUT);
		if (rc)
			return rc;

		total_length -= to_copy;
		p += to_copy;
	}

	return 0;
}

/* Open the serial device and configure it */
static void open_serial_device(struct dev *dev)
{
	int rc;

	rc = sp_get_port_by_name(dev->serial_device, &dev->sp);
	if (rc)
		err(EXIT_FAILURE, "Can't allocate serial port");

	rc = sp_open(dev->sp, SP_MODE_READ_WRITE);
	if (rc)
		err(EXIT_FAILURE, "Can't open serial port %s", dev->serial_device);

	if (sp_set_baudrate(dev->sp, 115200) ||
	    sp_set_bits(dev->sp, 8) ||
	    sp_set_parity(dev->sp, SP_PARITY_NONE) ||
	    sp_set_stopbits(dev->sp, 1) ||
	    sp_set_flowcontrol(dev->sp, SP_FLOWCONTROL_NONE))
		err(EXIT_FAILURE, "Can't set a serial port setting");
}

static const struct option long_options[] = {
	{ "serial-device", required_argument, 0,  'd' },
	{ "aprom-file", required_argument, 0,  'a' },
	{ "config-rpd", required_argument, 0,  'c' },
	{ "remain-isp", no_argument, 0,  'r' },
	{ "read-serial", no_argument, 0,  's' },
	{ "help", no_argument, 0,  'h' },
	{ 0, 0, 0, 0 }
};

static char * const opt_tokens[] = {
	[OPT_RPD] = "rpd",
	NULL
};

int process_config_options(struct dev *dev)
{
	char *subopts;
	char *value;
	int cfg_value;

	subopts = optarg;
	while (*subopts != '\0') {
		int opt;

		opt = getsubopt(&subopts, opt_tokens, &value);
		if (opt == -1) {
			fprintf(stderr, "Unrecognized config option '%s'\n",
				value);
			return -EINVAL;
		}

		if (value == NULL) {
			fprintf(stderr, "Missing config value\n");
			return -EINVAL;
		}

		if (strlen(value) != 1 || (value[0] != '0' && value[0] != '1')) {
			fprintf(stderr,
				"Invalid config value '%s'. Must be 0 or 1\n",
				value);
			return -EINVAL;
		}

		cfg_value = atoi(value);

		switch (opt) {
		case OPT_RPD:
			dev->config_new.rpd = cfg_value;
			dev->config_mask.rpd = 1;
			break;

		default:
			fprintf(stderr, "No known config bit %s\n", value);
			return -EINVAL;
		}
	}

	return 0;
}

void usage(void)
{
	printf("ISP programmer for Nuvoton N76E003\n");
	printf("Options:\n");
	printf("  --serial-device, -d    serial device to use. Defaults to /dev/ttyUSB0\n");
	printf("  --config, -c           enable or disable some config bits\n");
	printf("                         comma separated values of sub-options:\n");
	printf("                           rpd=0|1\n");
	printf("  --aprom, -a            binary APROM file to flash\n");
	printf("  --remain-isp, -r       remain in ISP mode when exiting\n");
	printf("  --read_serial, -s      read serial output after programming\n");
}

int main(int argc, char *argv[])
{
	struct dev dev = {
		.serial_device = "/dev/ttyUSB0",
	};
	int rc;
	int c;
	bool has_config_opts = false;

	while (1) {
		int option_index = 0;

		c = getopt_long(argc, argv, "a:c:d:hrs",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 0:
			printf("option %s", long_options[option_index].name);
			if (optarg)
				printf(" with arg %s", optarg);
			printf("\n");
			break;
		case 'a':
			dev.aprom_file = optarg;
			break;
		case 'c':
			if (process_config_options(&dev))
				return EXIT_FAILURE;
			has_config_opts = true;
			break;
		case 'd':
			dev.serial_device = optarg;
			break;
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'r':
			dev.remain_isp = true;
			break;
		case 's':
			dev.read_serial = true;
			break;
               default:
		       return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		errx(EXIT_FAILURE, "Extra argument: %s", argv[optind]);

	open_serial_device(&dev);

	dev.pkt_num = 0x17;		/* could be random */

	printf("Ready to connect\n");

	/* Try to automatically reset the device. Move DTR to low then
	 * high. This will not work if DTR is not connected or the RPD
	 * config bit is not set to 1. */
	sp_set_dtr(dev.sp, SP_DTR_ON);
	usleep(1000);
	sp_set_dtr(dev.sp, SP_DTR_OFF);

	rc = dev_connect(&dev);
	if (rc)
		errx(EXIT_FAILURE, "Can't connect to device");

	printf("Connected\n");

	rc = dev_sync_packno(&dev);
	if (rc)
		errx(EXIT_FAILURE, "Can't sync packet numbers");

	rc = generic_command(&dev, CMD_GET_FWVER);
	if (rc)
		errx(EXIT_FAILURE, "Can't get FW version");
	printf("FW version: 0x%x\n", dev.ack.get_fwver.version);

	rc = generic_command(&dev, CMD_GET_DEVICEID);
	if (rc)
		errx(EXIT_FAILURE, "Can't get device ID");
	switch (dev.ack.get_deviceid.deviceid) {
	case 0x3650: printf("Device is N76E003\n"); break;
	default:
		errx(EXIT_FAILURE, "Unknown device %x",
		     dev.ack.get_deviceid.deviceid);
		return -EOPNOTSUPP;
	}

	rc = generic_command(&dev, CMD_READ_CONFIG);
	if (rc)
		errx(EXIT_FAILURE, "Can't read config");
	dev.config_current = dev.ack.read_config;

	decode_config(&dev.config_current);
	dev.aprom_size = ldsize[dev.ack.read_config.ldsize].aprom_size * 1024;

	if (has_config_opts) {
		rc = set_new_config_options(&dev);
		if (rc)
			errx(EXIT_FAILURE, "Can't set new config bits");
	}

	if (0) {
		/* Implemented but not used. Avoid compilation warnings. */
		dev_reset(&dev);
	}

	if (dev.aprom_file) {
		printf("Flashing APROM with %s\n", dev.aprom_file);
		rc = dev_update_aprom(&dev);
		if (rc)
			errx(EXIT_FAILURE, "Can't program APROM");
		printf("Done\n");
	}

	if (!dev.remain_isp) {
		printf("Rebooting to APROM\n");
		dev_run_aprom(&dev);
	}

	if (dev.read_serial) {
		char buf[500];

		while (1) {
			rc = sp_blocking_read_next(dev.sp, buf, sizeof(buf), 1000);
			if (rc > 0)
				printf("%.*s", rc, buf);
		}
	}

	sp_close(dev.sp);
	sp_free_port(dev.sp);

	return 0;
}

/*
 * Local Variables:
 * mode: c
 * c-file-style: "linux"
 * indent-tabs-mode: t
 * tab-width: 8
 * End:
 */

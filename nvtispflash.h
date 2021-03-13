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

/* Supported commands */
enum {
	CMD_CONNECT          = 0xae,
	CMD_ERASE_ALL        = 0xa3,
	CMD_GET_DEVICEID     = 0xb1,
	CMD_GET_FLASHMODE    = 0xca, /* supported??? */
	CMD_GET_FWVER        = 0xa6,
	CMD_READ_CONFIG      = 0xa2,
	CMD_RESEND_PACKET    = 0xff,
	CMD_RESET            = 0xad,
	CMD_RUN_APROM        = 0xab,
	CMD_RUN_LDROM        = 0xac,
	CMD_SYNC_PACKNO      = 0xa4,
	CMD_UPDATE_APROM     = 0xa0,
	CMD_UPDATE_CONFIG    = 0xa1,
	CMD_UPDATE_DATAFLASH = 0xc3,
	CMD_WRITE_CHECKSUM   = 0xc9,
};

union config_bytes {
	struct {
		/* Config0 */
		uint8_t rsvd1:  1;
		uint8_t lock:   1;
		uint8_t rpd:    1;
		uint8_t rsvd2:  1;
		uint8_t ocden:  1;
		uint8_t ocdpwm: 1;
		uint8_t rsvd3:  1;
		uint8_t cbs:    1;

		/* Config1 */
		uint8_t ldsize: 3;
		uint8_t rsvd4:  5;

		/* Config2 */
		uint8_t rsvd5:  2;
		uint8_t cborst: 1;
		uint8_t boiap:  1;
		uint8_t cbov:   2;
		uint8_t rsvd6:  1;
		uint8_t cboden: 1;

		uint8_t rsvd_config3;

		/* Config4 */
		uint8_t rsvd7:  4;
		uint8_t wdten:  4;
	};
	uint8_t raw[5];
};

_Static_assert(sizeof(union config_bytes) == 5, "bad config size");

/* Command packet */
struct pkt_cmd {
	uint32_t cmd;
	uint32_t pkt_num;

	union {
		struct {
			uint32_t rn;
		} sync_packno;
		struct {
			uint32_t start_addr;
			uint32_t total_length;
			uint8_t data[48];
		} update_aprom;
		struct {
			uint8_t data[56];
		} update_aprom2;
		struct {
			union config_bytes new;
		} update_config;
		uint8_t pad[56];
	};
};

/* Ack, from the device */
struct pkt_ack {
    uint32_t checksum;
    uint32_t pkt_num;

    union {
	    struct {
		    uint8_t version;
	    } get_fwver;

	    struct {
		    uint32_t deviceid;
	    } get_deviceid;

	    union config_bytes read_config;

	    struct {
		    uint32_t mode;
	    } get_flashmode;

	    uint8_t pad[56];
    };
};

_Static_assert(sizeof(struct pkt_cmd) == 64, "bad packet size");
_Static_assert(sizeof(struct pkt_ack) == 64, "bad ack size");

/* Some chip config bits */
enum {
	OPT_RPD,
};

/* Device state */
struct dev {
	const char *serial_device;
	struct sp_port *sp;
	uint32_t pkt_num;	 /* next packet number, for command and ack */
	uint32_t checksum;	 /* checksum of last sent command */
	struct pkt_ack ack;	 /* last response */
	int aprom_size;		 /* APROM size, in bytes */
	const char *aprom_file;	 /* Binary file to program */
	bool remain_isp;	 /* Remain in ISP mode upon exiting */
	bool read_serial;	 /* Read from serial line after programming */

	/* Current config bits, and config bits set by the command
	 * line, if any. */
	union config_bytes config_current;
	union config_bytes config_new;
	union config_bytes config_mask;
};

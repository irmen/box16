// Commander X16 Emulator
// Copyright (c) 2019 Michael Steil
// Copyright (c) 2020 Frank van den Hoef
// All rights reserved. License: 2-clause BSD

#include "sdcard.h"

#include <SDL.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdbool.h>
#include <stdio.h>
#include <unordered_map>

#include "files.h"

#include "hypercalls.h"

// #define VERBOSE 1

// MMC/SD command (SPI mode)
enum {
	CMD0   = 0,         // GO_IDLE_STATE
	CMD1   = 1,         // SEND_OP_COND
	ACMD41 = 0x80 | 41, // SEND_OP_COND (SDC)
	CMD8   = 8,         // SEND_IF_COND
	CMD9   = 9,         // SEND_CSD
	CMD10  = 10,        // SEND_CID
	CMD12  = 12,        // STOP_TRANSMISSION
	CMD13  = 13,        // SEND_STATUS
	ACMD13 = 0x80 | 13, // SD_STATUS (SDC)
	CMD16  = 16,        // SET_BLOCKLEN
	CMD17  = 17,        // READ_SINGLE_BLOCK
	CMD18  = 18,        // READ_MULTIPLE_BLOCK
	CMD23  = 23,        // SET_BLOCK_COUNT
	ACMD23 = 0x80 | 23, // SET_WR_BLK_ERASE_COUNT (SDC)
	CMD24  = 24,        // WRITE_BLOCK
	CMD25  = 25,        // WRITE_MULTIPLE_BLOCK
	CMD32  = 32,        // ERASE_WR_BLK_START
	CMD33  = 33,        // ERASE_WR_BLK_END
	CMD38  = 38,        // ERASE
	CMD55  = 55,        // APP_CMD
	CMD58  = 58,        // READ_OCR
};

char            sdcard_path[PATH_MAX] = "";
static x16file *sdcard_file           = nullptr;
bool            sdcard_attached       = false;

static uint8_t  rxbuf[3 + 512];
static int      rxbuf_idx;
static uint32_t lba;
static uint8_t  last_cmd;
static bool     is_acmd        = false;
static bool     is_idle        = true;
static bool     is_initialized = false;

static const uint8_t *response         = nullptr;
static int            response_length  = 0;
static int            response_counter = 0;

static bool selected = false;

void sdcard_shutdown()
{
	if (sdcard_attached) {
		sdcard_detach();
	}
}

void sdcard_set_file(char const *path)
{
	sdcard_detach();

	strncpy(sdcard_path, path, PATH_MAX);
	sdcard_path[PATH_MAX - 1] = '\0';

	sdcard_attach();
}

bool sdcard_path_is_set()
{
	return strlen(sdcard_path) > 0;
}

void sdcard_attach()
{
	if (!sdcard_attached && sdcard_path_is_set()) {
		sdcard_file = x16open(sdcard_path, "r+b");
		if (sdcard_file == nullptr) {
			fmt::print("Cannot open SDCard file {}!\n", sdcard_path);
			return;
		}

		fmt::print("SD card attached.\n");
		sdcard_attached = true;
		is_initialized  = false;

		hypercalls_update();
	}
}

void sdcard_detach()
{
	if (sdcard_attached) {
		x16close(sdcard_file);
		sdcard_file = nullptr;

		fmt::print("SD card detached.\n");
		sdcard_attached = false;

		hypercalls_update();
	}
}

bool sdcard_is_attached()
{
	return sdcard_file != nullptr && sdcard_attached;
}

void sdcard_select(bool select)
{
	selected  = select;
	rxbuf_idx = 0;
#if defined(VERBOSE) && VERBOSE >= 2
	fmt::print("*** SD card select: %u\n", select);
#endif
}

static void set_response_r1(void)
{
	static uint8_t r1;
	r1              = is_idle ? 1 : 0;
	response        = &r1;
	response_length = 1;
}

static void set_response_r2(void)
{
	if (is_initialized) {
		static const uint8_t r2[] = { 0x00, 0x00 };
		response                  = r2;
		response_length           = sizeof(r2);
	} else {
		static const uint8_t r2[] = { 0x1F, 0xFF };
		response                  = r2;
		response_length           = sizeof(r2);
	}
}

static void set_response_r3(void)
{
	static const uint8_t r3[] = { 0xC0, 0xFF, 0x80, 0x00 };
	response                  = r3;
	response_length           = sizeof(r3);
}

static void set_response_r7(void)
{
	static const uint8_t r7[] = { 1, 0x00, 0x00, 0x01, 0xAA };
	response                  = r7;
	response_length           = sizeof(r7);
}

uint8_t sdcard_handle(uint8_t inbyte)
{
	if (!selected || sdcard_file == nullptr) {
		return 0xFF;
	}
	// fmt::print("sdcard_handle: {:02X}\n", inbyte);

	uint8_t outbyte = 0xFF;

	if (rxbuf_idx == 0 && inbyte == 0xFF) {
		// send response data
		if (response) {
			outbyte = response[response_counter++];
			if (response_counter == response_length) {
				response = nullptr;
			}
		}

	} else {
		rxbuf[rxbuf_idx++] = inbyte;

		if ((rxbuf[0] & 0xC0) == 0x40 && rxbuf_idx == 6) {
			rxbuf_idx = 0;

			// Check for start-bit + transmission bit
			if ((rxbuf[0] & 0xC0) != 0x40) {
				response = nullptr;
				return 0xFF;
			}
			rxbuf[0] &= 0x3F;

			// Use upper command bit to indicate this is an ACMD
			if (is_acmd) {
				rxbuf[0] |= 0x80;
				is_acmd = false;
			}

			last_cmd = rxbuf[0];

#if defined(VERBOSE) && VERBOSE >= 2
			fmt::print("*** SD {}CMD{:d} -> Response:", (rxbuf[0] & 0x80) ? "A" : "", rxbuf[0] & 0x3F);
#endif
			switch (rxbuf[0]) {
				case CMD0: {
					// GO_IDLE_STATE: Resets the SD Memory Card
					is_idle = true;
					set_response_r1();
					break;
				}

				case CMD8: {
					// SEND_IF_COND: Sends SD Memory Card interface condition that includes host supply voltage
					set_response_r7();
					break;
				}

				case ACMD41: {
					// SD_SEND_OP_COND: Sends host capacity support information and activated the card's initialization process
					is_idle        = false;
					is_initialized = true;
					set_response_r1();
					break;
				}

				case CMD13: {
					// SEND_STATUS: Asks the selected card to send its status register
					set_response_r2();
					break;
				}
				case CMD16: {
					// SET_BLOCKLEN: In case of non-SDHC card, this sets the block length. Block length of SDHC/SDXC cards are fixed to 512 bytes.
					set_response_r1();
					break;
				}
				case CMD17: {
					// READ_SINGLE_BLOCK
					uint32_t       lba = (rxbuf[1] << 24) | (rxbuf[2] << 16) | (rxbuf[3] << 8) | rxbuf[4];
					static uint8_t read_block_response[2 + 512 + 2];
					read_block_response[0] = 0;
					read_block_response[1] = 0xFE;
#ifdef VERBOSE
					fmt::print("*** SD Reading LBA {:d}\n", lba);
#endif
					if ((uint64_t)lba * 512 >= x16size(sdcard_file)) {
						read_block_response[1] = 0x08; // out of range
						response_length        = 2;
					} else {
						x16seek(sdcard_file, (Sint64)lba * 512, XSEEK_SET);
						size_t bytes_read = x16read(sdcard_file, &read_block_response[2], 1, 512);
						if (bytes_read != 512) {
							fmt::print("Warning: short read!\n");
						}

						response        = read_block_response;
						response_length = 2 + 512 + 2;
					}

					response        = read_block_response;
					response_length = 2 + 512 + 2;
					break;
				}

				case CMD24: {
					// WRITE_BLOCK
					lba = (rxbuf[1] << 24) | (rxbuf[2] << 16) | (rxbuf[3] << 8) | rxbuf[4];
					set_response_r1();
					break;
				}

				case CMD55: {
					// APP_CMD: Next command is an application specific command
					is_acmd = true;
					set_response_r1();
					break;
				}

				case CMD58: {
					// READ_OCR: Read the OCR register of the card
					set_response_r3();
					break;
				}

				default: {
					set_response_r1();
					break;
				}
			}
			response_counter = 0;

#if defined(VERBOSE) && VERBOSE >= 2
			for (int i = 0; i < (response_length < 16 ? response_length : 16); i++) {
				fmt::print(" {:02X}", response[i]);
			}
			fmt::print("\n");
#endif

		} else if (rxbuf_idx == 515) {
			rxbuf_idx = 0;
			// Check for 'start block' byte
			if (last_cmd == CMD24 && rxbuf[0] == 0xFE) {
#ifdef VERBOSE
				fmt::print("*** SD Writing LBA {:d}\n", lba);
#endif
				if ((uint64_t)lba * 512 >= x16size(sdcard_file)) {
					// do nothing?
				} else {
					x16seek(sdcard_file, (Sint64)lba * 512, XSEEK_SET);
					size_t bytes_written = x16write(sdcard_file, rxbuf + 1, 1, 512);
					if (bytes_written != 512) {
						fmt::print("Warning: short write!\n");
					}
				}
			}
		}
	}
	return outbyte;
}

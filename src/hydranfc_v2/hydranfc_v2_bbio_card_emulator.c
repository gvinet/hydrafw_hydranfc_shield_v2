/*
 * HydraBus/HydraNFC
 *
 * Copyright (C) 2020 Guillaume VINET
 * Copyright (C) 2014-2020 Benjamin VERNOUX
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common.h"
#include "tokenline.h"
#include "hydranfc_v2_nfc_mode.h"
#include "rfal_rf.h"
#include "hydrabus_bbio.h"
#include "bsp_gpio.h"
#include "hydranfc_v2_bbio_card_emulator.h"
#include "st25r3916.h"
#include "st25r3916_irq.h"
#include "st25r3916_com.h"
#include "rfal_analogConfig.h"
#include "rfal_rf.h"
#include "rfal_nfca.h"
#include "rfal_nfcb.h"
#include "rfal_nfcv.h"
#include "string.h"
#include "stdbool.h"
#include "rfal_isoDep.h"
#include "dispatcher.h"
#include "stdlib.h"
#include "utils.h"
#include "bsp_print_dbg.h"
#include <string.h>

enum cardEmulationCommand {
  CARDEMULATION_CMD_START = 0x01, /*!< start listen mode. */
  CARDEMULATION_CMD_STOP = 0x02, /*!< stop listen mode. */

  CARDEMULATION_CMD_GET_RX_A = 0x11,
  CARDEMULATION_CMD_SET_TX_A = 0x12,
  CARDEMULATION_CMD_GET_RX_B = 0x13,
  CARDEMULATION_CMD_SET_TX_B = 0x14,
  CARDEMULATION_CMD_GET_RX_F = 0x15,
  CARDEMULATION_CMD_SET_TX_F = 0x16,

  CARDEMULATION_CMD_GET_LISTEN_STATE = 0x21,
};

static volatile int irq_count;
static volatile int irq;
static volatile int irq_end_rx;
static void (*st25r3916_irq_fn)(void) = NULL;

#define TX_BUF_LENGTH       (256+3)
static uint8_t txBuf_ce[TX_BUF_LENGTH] __attribute__ ((section(".cmm")));

#define RX_BUF_LENGTH       (256+3)
static uint8_t rxBuf_ce[RX_BUF_LENGTH] __attribute__ ((section(".cmm")));
static uint16_t rxRcvdLen = 0;
static bool ceEnabled;
static rfalLmState state;
static rfalLmConfPA configA;
static rfalIsoDepAtsParam atsParam;
static uint8_t histChar[16];
static uint32_t configMask = 0;
static rfalTransceiveContext transceiveCtx;

static rfalIsoDepBufFormat *rxBufA;
static rfalIsoDepBufFormat *txBufA;
static bool bIsRxChaining;
static bool isActivatedA = false;
static bool isFirstA3_Frame = true;
static rfalIsoDepTxRxParam isoDepTxRxParam;

static bool rxReady = false;
static bool txReady = false;
static bool rawMode = false;
static uint16_t (*cardA_activated_ptr)(void) = NULL;

static uint8_t rxtxFrameBuf[512] __attribute__ ((section(".cmm")));

uint16_t bbio_processCmd(uint8_t *cmdData, uint16_t cmdDataLen, uint8_t *rspData);
static uint16_t (*current_processCmdPtr)(uint8_t *, uint16_t, uint8_t *) = bbio_processCmd;

static uint16_t dispatcherInterruptResults[32];

void hydranfc_ce_common(t_hydra_console *con, bool quiet);
static const uint8_t dispatcherInterruptResultRegs[32] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 1st byte */
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 2nd byte */
	ST25R3916_REG_CAPACITANCE_MEASURE_RESULT,
	ST25R3916_REG_PHASE_MEASURE_RESULT,
	ST25R3916_REG_AMPLITUDE_MEASURE_RESULT, 0xff, 0xff, 0xff, 0xff, 0xff, /* 3rd byte */
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 4th byte */
};

static void dispatcherInterruptHandler(void) {
	int i;
	uint8_t val;
	uint16_t isrs;

	for (i = 0; i < 32; i++) { /* !!!!!!!!!!!!!!!!! */
		if (dispatcherInterruptResultRegs[i] >= 0x40) continue;
		if (!st25r3916GetInterrupt(1UL << i)) continue;

		isrs = dispatcherInterruptResults[i] >> 8;
		if (isrs < 255) isrs++;

		st25r3916ReadRegister(dispatcherInterruptResultRegs[i], &val);

		dispatcherInterruptResults[i] = (isrs << 8)|val;
	}
}

static ReturnCode ceGetRx(const uint8_t cmd, uint8_t *txData, uint16_t *txSize) {
	ReturnCode err = ERR_NOTFOUND;
	switch (cmd) {
		case CARDEMULATION_CMD_GET_RX_A:
			if (isActivatedA) {

				if (rawMode) {
					err = rfalIsoDepGetTransceiveStatusRawMode();
				} else {
					err = rfalIsoDepGetTransceiveStatus();
				}

				switch (err) {
					case ERR_SLEEP_REQ:
						// TBC if ok going to idle state rather than to sleep state!
					case ERR_LINK_LOSS:
					default: rfalListenStop();
						rfalListenStart(configMask,
						                &configA,
						                NULL,
						                NULL,
						                rxBuf_ce,
						                rfalConvBytesToBits(RX_BUF_LENGTH),
						                &rxRcvdLen);
						rxReady = txReady = isActivatedA = false;
						isFirstA3_Frame = true;
						printf_dbg("LLoss3 %d\r\n", err);
						break;

					case ERR_BUSY:           /* Transceive ongoing                              */
					case ERR_AGAIN:          /* Chaining packet received - handling to be added */
						break;

					case ERR_NONE:

						if( rawMode){
							ST_MEMCPY(txData, isoDepTxRxParam.rxBuf, *isoDepTxRxParam.rxLen);
						}
						else{
							ST_MEMCPY(txData, isoDepTxRxParam.rxBuf->inf, *isoDepTxRxParam.rxLen);
						}

						*txSize = *isoDepTxRxParam.rxLen;

						rxReady = false;
						txReady = true;
						break;
				}

			} else {
				// handle layer3 comms here
				if (isFirstA3_Frame) {
					// IMPORTANT: We can not call rfalGetTransceiveStatus before we did the first
					// call rfalStartTransceive

					err = ERR_NONE;

					if (rxReady) {
						ST_MEMCPY(txData, transceiveCtx.rxBuf, *transceiveCtx.rxRcvdLen);
						*txSize = rfalConvBitsToBytes(*transceiveCtx.rxRcvdLen);

						rxReady = false;
						txReady = true;
					} else {
						*txSize = 0;
						err = ERR_BUSY; // no data available yet
					}

					// NOTE: isFirstA3_Frame is cleared after the first transmit is done.
					// For this mode this transmit is done within the rfalStartTransceive
					// function in ceSetTx(..)

				}
			}
			break;

		default: break;
	}

	return err;
}

static ReturnCode ceSetTx(const uint8_t cmd, const uint8_t *rxData, const uint16_t rxSize) {
	ReturnCode err = ERR_NOTFOUND;
	uint16_t rxSizeBytes = rfalConvBitsToBytes(rxSize);

	switch (cmd) {
		case CARDEMULATION_CMD_SET_TX_A:
			if (isActivatedA) {
				if (txReady) {
					ST_MEMCPY(isoDepTxRxParam.txBuf->inf, rxData, rxSizeBytes);
					isoDepTxRxParam.txBufLen = rxSizeBytes;
					*isoDepTxRxParam.rxLen = 0;
					err = rfalIsoDepStartTransceive(isoDepTxRxParam);

					if (err == ERR_NONE) {
						rxReady = txReady = false;
					}
				}
			}
			break;

		default: break;
	}
	return err;
}

static void ceHandler(void) {
	bool dataFlag = false;
	ReturnCode retCode;

	/* Check whether CE is enabled */
	if (!ceEnabled) {
		return;
	}

	state = rfalListenGetState(&dataFlag, NULL);

	switch (state) {
		// ------------------------------------------------------------------
		// NFC A
		// ------------------------------------------------------------------
		//
		case RFAL_LM_STATE_ACTIVE_A:
		case RFAL_LM_STATE_ACTIVE_Ax: {
			if (dataFlag == true) {
				if (rfalIsoDepIsRats(rxBuf_ce, rfalConvBitsToBytes(rxRcvdLen))) {
					printf_dbg("RATS!\r\n");

					// enter next already state
					rfalListenSetState(RFAL_LM_STATE_CARDEMU_4A);

					// prepare for RATS
					rfalIsoDepListenActvParam rxParam;
					rxParam.rxBuf = rxBufA;
					rxParam.rxLen = &rxRcvdLen;
					rxParam.isoDepDev = NULL;
					rxParam.isRxChaining = &bIsRxChaining;
					//
					isoDepTxRxParam.FSx = rfalIsoDepFSxI2FSx(atsParam.fsci);
					rfalIsoDepListenStartActivation(&atsParam, NULL, rxBuf_ce, rxRcvdLen, rxParam);
				} else if (true == rfalNfcaListenerIsSleepReq(rxBuf_ce, rfalConvBitsToBytes(rxRcvdLen))) {
					printf_dbg("HLTA\r\n");
					rfalListenSleepStart(RFAL_LM_STATE_SLEEP_A, rxBuf_ce, RX_BUF_LENGTH, &rxRcvdLen);
				} else {
					int i, rxSize;
					if (rxRcvdLen > 0) {
						printf_dbg("Hndler A3 rx: ");

						rxSize = rfalConvBitsToBytes(rxRcvdLen);
						for (i = 0; i < (rxSize > 16 ? 16 : rxSize); i++)
							printf_dbg(" %02X", rxBuf_ce[i]);
						printf_dbg("\r\n");
					}

					// not HLTA and not RATS - layer3 comms
					rxReady = true;
				}
			}
			break;
		}
			//
		case RFAL_LM_STATE_CARDEMU_4A : {
			if (!isActivatedA) {
				// finish card activation sequence
				retCode = rfalIsoDepListenGetActivationStatus();
				if (retCode != ERR_BUSY) {
					//
					switch (retCode) {
						case ERR_LINK_LOSS: rfalListenStop();
							rfalListenStart(configMask,
							                &configA,
							                NULL,
							                NULL,
							                rxBuf_ce,
							                rfalConvBytesToBits(RX_BUF_LENGTH),
							                &rxRcvdLen);
							isActivatedA = false;
							isFirstA3_Frame = true;
							printf_dbg("LLoss2\r\n");
							break;

						case ERR_NONE: isActivatedA = true;
							if (cardA_activated_ptr != NULL) {
								cardA_activated_ptr();
							}
							break;

							// all other error cases are simple ignored ..
						default: printf_dbg("GetActivationStatus err is %d\r\n", retCode);

							break;
					}
				}
			}

			break;
		}

		default: break;
	}
}

static void ceRun(void) {
	ReturnCode err = ERR_NONE;
	uint16_t dataSize;

	dispatcherInterruptHandler();

	rfalWorker();
	ceHandler();
	dataSize = sizeof(rxtxFrameBuf);

	err = ceGetRx(CARDEMULATION_CMD_GET_RX_A, rxtxFrameBuf, &dataSize);

	if (err == ERR_NONE) {

		dataSize = (*current_processCmdPtr)(rxtxFrameBuf, dataSize, rxtxFrameBuf);
		ceSetTx(CARDEMULATION_CMD_SET_TX_A, rxtxFrameBuf, dataSize);

	}
}

static void hydranfc_ce_set_processCmd_ptr(void *ptr) {
	current_processCmdPtr = ptr;
}

static void ceInit(void) {
	transceiveCtx.txBuf = txBuf_ce;
	transceiveCtx.txBufLen = 0;

	transceiveCtx.rxBuf = rxBuf_ce;
	transceiveCtx.rxBufLen = RX_BUF_LENGTH;
	transceiveCtx.rxRcvdLen = &rxRcvdLen;

	transceiveCtx.flags = RFAL_TXRX_FLAGS_DEFAULT;
	transceiveCtx.fwt = RFAL_FWT_NONE;

	rxBufA = (rfalIsoDepBufFormat *) rxBuf_ce;
	txBufA = (rfalIsoDepBufFormat *) txBuf_ce;

	isoDepTxRxParam.rxBuf = rxBufA;
	isoDepTxRxParam.rxLen = &rxRcvdLen;
	isoDepTxRxParam.txBuf = txBufA;
	isoDepTxRxParam.ourFSx = RFAL_ISODEP_FSX_KEEP;
	isoDepTxRxParam.isRxChaining = &bIsRxChaining;
	isoDepTxRxParam.isTxChaining = false;

	configMask = 0;
	isActivatedA = false;
	isFirstA3_Frame = true;
	rxReady = false;
	txReady = false;

	ceEnabled = false;
}

static ReturnCode ceStart(void) {

	// .. and go ..
	ceEnabled = true;
	rxRcvdLen = 0;
	configMask = RFAL_LM_MASK_NFCA;
	return rfalListenStart(configMask,
	                       &configA,
	                       NULL,
	                       NULL,
	                       rxBuf_ce,
	                       rfalConvBytesToBits(RX_BUF_LENGTH),
	                       &rxRcvdLen);
}

static ReturnCode ceStop(void) {
	rfalListenStop();
	ceInit();
	return ERR_NONE;
}

static void bbio_iso4443_raw_ce_common(void) {
	ReturnCode err = ERR_NONE;

	rfalFieldOff();
	ceInit();
	rfalIsoDepInitialize();

	err = ceStart();
	if (err == ERR_NONE) {
		while (!hydrabus_ubtn()) {
			ceRun();
			chThdYield();
		}

		ceStop();
	}
}

static void ce_set_cardA_activated_ptr(void *ptr) {
	cardA_activated_ptr = ptr;
}

/* Triggered when the Ext IRQ is pressed or released. */
static void extcb1(void *arg) {
	(void) arg;

	if (st25r3916_irq_fn != NULL)
		st25r3916_irq_fn();

	irq_count++;
	irq = 1;
}

extern t_mode_config mode_con1;
static t_hydra_console *g_con;

static ReturnCode hydranfc_v2_init_RFAL(t_hydra_console *con) {
	ReturnCode err;
	/* RFAL initalisation */
	rfalAnalogConfigInitialize();
	err = rfalInitialize();
	if (err != ERR_NONE) {
		cprintf(con, "hydranfc_v2_init_RFAL rfalInitialize() error=%d\r\n", err);
		return err;
	}

	/* DPO setup */
#ifdef DPO_ENABLE
	rfalDpoInitialize();
	rfalDpoSetMeasureCallback( rfalChipMeasureAmplitude );
	err = rfalDpoTableWrite(dpoSetup,sizeof(dpoSetup)/sizeof(rfalDpoEntry));
	if(err != ERR_NONE) {
		cprintf(con, "hydranfc_v2_init_RFAL rfalDpoTableWrite() error=%d\r\n", err);
		return err;
	}
	rfalDpoSetEnabled(true);
	rfalSetPreTxRxCallback(&rfalPreTransceiveCb);
#endif
	return err;
}

static bool init_gpio_spi_nfc(t_hydra_console *con) {
	/*
	 * Initializes the SPI driver 2. The SPI2 signals are routed as follow:
	 * ST25R3916 IO4_CS SPI mode / HydraBus PC1 - NSS
	 * ST25R3916 DATA_CLK SPI mode / HydraBus PB10 - SCK
	 * ST25R3916 IO6_MISO SPI mode / HydraBus PC2 - MISO
	 * ST25R3916 IO7_MOSI SPI mode / HydraBus PC3 - MOSI
	 * Used for communication with ST25R3916 in SPI mode with NSS.
	 */
	mode_con1.proto.config.spi.dev_gpio_pull = MODE_CONFIG_DEV_GPIO_NOPULL;
	//mode_con1.proto.config.spi.dev_speed = 5; /* 5 250 000 Hz */
	mode_con1.proto.config.spi.dev_speed = 6; /* 10 500 000 Hz */
	mode_con1.proto.config.spi.dev_phase = 1;
	mode_con1.proto.config.spi.dev_polarity = 0;
	mode_con1.proto.config.spi.dev_bit_lsb_msb = DEV_FIRSTBIT_MSB;
	mode_con1.proto.config.spi.dev_mode = DEV_MASTER;
	bsp_spi_init(BSP_DEV_SPI2, &mode_con1.proto);

	/*
	 * Initializes the SPI driver 1. The SPI1 signals are routed as follows:
	 * Shall be configured as SPI Slave for ST25R3916 NFC data sampling on MOD pin.
	 * NSS. (Not used use Software).
	 * ST25R3916 MCU_CLK pin28 output / HydraBus PA5 - SCK.(AF5) => SPI Slave CLK input (Sniffer mode/RX Transparent Mode)
	 * ST25R3916 ST25R3916 MOSI SPI pin31 (IN) / HydraBus PA6 - MISO.(AF5) (MCU TX Transparent Mode)
	 * ST25R3916 ST25R3916 MISO_SDA pin32 (OUT) / HydraBus PA7 - MOSI.(AF5) => SPI Slave MOSI input (Sniffer mode/RX Transparent Mode)
	 */
	/* spiStart() is done in sniffer see sniffer.c */
	/* HydraBus SPI1 Slave CLK input */
	bsp_gpio_init(BSP_GPIO_PORTA, 5, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL);
	/* HydraBus SPI1 Slave MISO. Not used/Not connected */
	bsp_gpio_init(BSP_GPIO_PORTA, 6, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL);
	/* HydraBus SPI1 Slave MOSI. connected to ST25R3916 MOD Pin */
	bsp_gpio_init(BSP_GPIO_PORTA, 7, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL);

	/* Configure K1/2 Buttons as Input */
	bsp_gpio_init(BSP_GPIO_PORTB, 8, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL); /* K1 Button */
	bsp_gpio_init(BSP_GPIO_PORTB, 9, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL); /* K2 Button */

	/* Configure D1/2/3/4 LEDs as Output */
	D1_OFF;
	D2_OFF;
	D3_OFF;
	D4_OFF;

	/* LED_D1 or TST_PÏN */
	bsp_gpio_init(BSP_GPIO_PORTB, 0, MODE_CONFIG_DEV_GPIO_OUT_PUSHPULL, MODE_CONFIG_DEV_GPIO_NOPULL);

#ifndef MAKE_DEBUG
	// can't use LED on PB3 if using SWO
	bsp_gpio_init(BSP_GPIO_PORTB, 3, MODE_CONFIG_DEV_GPIO_OUT_PUSHPULL, MODE_CONFIG_DEV_GPIO_NOPULL);
#endif

	bsp_gpio_init(BSP_GPIO_PORTB, 4, MODE_CONFIG_DEV_GPIO_OUT_PUSHPULL, MODE_CONFIG_DEV_GPIO_NOPULL);
	bsp_gpio_init(BSP_GPIO_PORTB, 5, MODE_CONFIG_DEV_GPIO_OUT_PUSHPULL, MODE_CONFIG_DEV_GPIO_NOPULL);

	palDisablePadEvent(GPIOA, 1);
	/* ST25R3916 IRQ output / HydraBus PA1 input */
	palClearPad(GPIOA, 1);
	palSetPadMode(GPIOA, 1, PAL_MODE_INPUT|PAL_STM32_OSPEED_MID1);
	/* Activates the PAL driver callback */
	//palDisablePadEvent(GPIOA, 1);
	palEnablePadEvent(GPIOA, 1, PAL_EVENT_MODE_RISING_EDGE);
	palSetPadCallback(GPIOA, 1, &extcb1, NULL);

	/* Init st25r3916 IRQ function callback */
	st25r3916_irq_fn = st25r3916Isr;
	hal_st25r3916_spiInit(ST25R391X_SPI_DEVICE);
	if (hydranfc_v2_init_RFAL(con) != ERR_NONE) {
		cprintf(con, "HydraNFC v2 not found.\r\n");
		return FALSE;
	}
	return TRUE;
}

static void deinit_gpio_spi_nfc(t_hydra_console *con) {
	(void) (con);
	palClearPad(GPIOA, 1);
	palSetPadMode(GPIOA, 1, PAL_MODE_INPUT);
	palDisablePadEvent(GPIOA, 1);

	bsp_spi_deinit(BSP_DEV_SPI2);

	bsp_gpio_init(BSP_GPIO_PORTA, 5, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL);
	bsp_gpio_init(BSP_GPIO_PORTA, 6, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL);
	bsp_gpio_init(BSP_GPIO_PORTA, 7, MODE_CONFIG_DEV_GPIO_IN, MODE_CONFIG_DEV_GPIO_NOPULL);

	st25r3916_irq_fn = NULL;
}

static void bbio_mode_id(t_hydra_console *con) {
	cprint(con, BBIO_HYDRANFC_CARD_EMULATOR, 4);
}

static void init(void) {
	configA.nfcidLen = 4;
	configA.nfcid[0] = 0xAA;
	configA.nfcid[1] = 0xBB;
	configA.nfcid[2] = 0xCC;
	configA.nfcid[3] = 0xDD;

	configA.SENS_RES[0] = 0x04;
	configA.SENS_RES[1] = 0x00;

	configA.SEL_RES = 0x20;

	atsParam.fsci = 0x08;
	atsParam.fwi = 0x0A;
	atsParam.sfgi = 0x00;
	atsParam.ta = 0x00;

	atsParam.didSupport = 0x00;

	histChar[0] = 0x73;
	histChar[1] = 0x74;
	atsParam.hb = histChar;
	atsParam.hbLen = 0x02;

}

/* Main command management function */
uint16_t bbio_processCmd(uint8_t *cmdData, uint16_t cmdDatalen, uint8_t *rspData) {
	uint16_t rspDataLen;
	char cmd_byte = BBIO_NFC_CE_CARD_CMD;

	cprint(g_con, &cmd_byte, 1);

	cprint(g_con, (char *) &cmdDatalen, 2);
	cprint(g_con, (char *) cmdData, cmdDatalen);

	chnRead(g_con->sdu, (uint8_t * )&rspDataLen, 2);
	chnRead(g_con->sdu, rspData, rspDataLen);

	return rspDataLen * 8;
}

static void cardA_activated(void) {
	char cmd_byte = BBIO_NFC_CE_CARD_ACTIVATION;

	cprint(g_con, &cmd_byte, 1);

}

void bbio_mode_hydranfc_v2_card_emulator(t_hydra_console *con) {
	uint8_t bbio_subcommand, clen;
	uint8_t *rx_data = (uint8_t *) g_sbuf + 4096;

	init_gpio_spi_nfc(con);

	bbio_mode_id(con);
	init();

	while (!hydrabus_ubtn()) {

		if (chnRead(con->sdu, &bbio_subcommand, 1) == 1) {
			switch (bbio_subcommand) {
				case BBIO_NFC_CE_START_EMULATION:
					/* Init st25r3916 IRQ function callback */
					st25r3916_irq_fn = st25r3916Isr;

					hydranfc_ce_set_processCmd_ptr(&bbio_processCmd);
					ce_set_cardA_activated_ptr(&cardA_activated);
					g_con = con;
					rawMode = FALSE;
					bbio_iso4443_raw_ce_common();

					bbio_subcommand = BBIO_NFC_CE_END_EMULATION;
					cprint(con, (char *) &bbio_subcommand, 1);

					irq_count = 0;
					st25r3916_irq_fn = NULL;
					break;

				case BBIO_NFC_CE_START_EMULATION_RAW:
					/* Init st25r3916 IRQ function callback */
					st25r3916_irq_fn = st25r3916Isr;

					hydranfc_ce_set_processCmd_ptr(&bbio_processCmd);
					ce_set_cardA_activated_ptr(&cardA_activated);
					g_con = con;
					rawMode = TRUE;
					bbio_iso4443_raw_ce_common();

					bbio_subcommand = BBIO_NFC_CE_END_EMULATION;
					cprint(con, (char *) &bbio_subcommand, 1);

					irq_count = 0;
					st25r3916_irq_fn = NULL;
					break;

				case BBIO_NFC_CE_SET_UID: {
					chnRead(con->sdu, &clen, 1);
					chnRead(con->sdu, rx_data, clen);

					switch (clen) {
						case 4:
						case 7: configA.nfcidLen = clen;
							memcpy(configA.nfcid, rx_data, clen);
							rx_data[0] = 0x01;
							break;
						default: rx_data[0] = 0x00;
					}

					cprint(con, (char *) rx_data, 1);
					break;
				}

				case BBIO_NFC_CE_SET_SAK: {
					chnRead(con->sdu, &clen, 1);
					chnRead(con->sdu, rx_data, clen);

					configA.SEL_RES = rx_data[0];
					rx_data[0] = 1;

					cprint(con, (char *) rx_data, 1);
					break;
				}

				case BBIO_NFC_CE_SET_ATS_HIST_BYTES:{
					chnRead(con->sdu, &clen, 1);
					chnRead(con->sdu, rx_data, clen);

					if( clen <= 16){
						memcpy(histChar, rx_data, clen);
						atsParam.hbLen = clen;
						rx_data[0] = 1;
					} else{
						rx_data[0] = 0;
					}

					cprint(con, (char *) rx_data, 1);
					break;
				}

				case BBIO_RESET: {
					deinit_gpio_spi_nfc(con);
					return;
				}
			}
		}
	}
}

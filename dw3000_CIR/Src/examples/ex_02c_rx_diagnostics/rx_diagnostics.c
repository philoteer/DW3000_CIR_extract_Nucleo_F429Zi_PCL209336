/*! ----------------------------------------------------------------------------
 *  @file    rx_diagnostics.c
 *  @brief   Simple RX with diagnostics example code
 *
 *           This application waits for reception of a frame. After each frame received with a good CRC it reads some data provided by DW IC:
 *               - Diagnostics data (e.g. first path index, first path amplitude, channel impulse response, etc.). See dwt_rxdiag_t structure for more
 *                 details on the data read.
 *               - Accumulator values around the first path.
 *           It also reads event counters (e.g. CRC good, CRC error, PHY header error, etc.) after any event, be it a good frame or an RX error. See
 *           dwt_deviceentcnts_t structure for more details on the counters read.
 *
 * @attention
 *
 * Copyright 2016-2020 (c) Decawave Ltd, Dublin, Ireland.
 *
 * All rights reserved.
 *
 * @author Decawave
 */

#include <deca_device_api.h>
#include <deca_regs.h>
#include <deca_spi.h>
#include <port.h>
#include <example_selection.h>
#include <shared_defines.h>
#include <time.h>
#include <math.h>

#if defined(TEST_RX_DIAG)


/* Global Vars */

extern void test_run_info(unsigned char *data);
extern UART_HandleTypeDef huart6;

/* Example application name */
#define APP_NAME "RX DIAG v1.0"
#define CIR_LEN 1016 //Max: 1016; Min: 1
#define PRINT_BUF_SIZE 16384
#define CIR_DUMP_START 720
#define CIR_DUMP_LEN 192

/* Default communication configuration. We use default non-STS DW mode. */
static dwt_config_t config = {
    9,               /* Channel number. */
    DWT_PLEN_128,    /* Preamble length. Used in TX only. */
    DWT_PAC8,        /* Preamble acquisition chunk size. Used in RX only. */
    9,               /* TX preamble code. Used in TX only. */
    9,               /* RX preamble code. Used in RX only. */
    1,               /* 0 to use standard 8 symbol SFD, 1 to use non-standard 8 symbol, 2 for non-standard 16 symbol SFD and 3 for 4z 8 symbol SDF type */
    DWT_BR_6M8,      /* Data rate. */
    DWT_PHRMODE_STD, /* PHY header mode. */
    DWT_PHRRATE_STD, /* PHY header rate. */
    (129 + 8 - 8),   /* SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. */
    DWT_STS_MODE_OFF, /* Cipher disabled */
    DWT_STS_LEN_64,/* Cipher length see allowed values in Enum dwt_sts_lengths_e */
    DWT_PDOA_M0      /* PDOA mode off */
};

/* Buffer to store received frame. See NOTE 1 below. */
static uint8_t rx_buffer[FRAME_LEN_MAX];

/* Hold copy of status register state here for reference, so reader can examine it at a breakpoint. */
static uint32_t status_reg = 0;

/* Hold copy of frame length of frame received (if good), so reader can examine it at a breakpoint. */
static uint16_t frame_len = 0;

/* Hold copy of event counters so that it can be examined at a debug breakpoint. */
static dwt_deviceentcnts_t event_cnt;

/* Hold copy of diagnostics data so that it can be examined at a debug breakpoint. */
static dwt_rxdiag_t rx_diag;

/* Hold copy of accumulator data so that it can be examined at a debug breakpoint. See NOTE 2. */
#define ACCUM_DATA_LEN (3 * 2 * (3 + 3) + 1)
static uint8_t accum_data[ACCUM_DATA_LEN];

static uint8_t CIR_data[6097];
char print_buf[PRINT_BUF_SIZE];

static int32_t CIR_real[CIR_LEN];
static int32_t CIR_imag[CIR_LEN];

//src: https://www.cnblogs.com/simpleGao/p/17253002.html
//modified to 32 bits due to... %llu not being a thing here.
//1 tick = 15.65ps * 256 = 4.0064 ns.

static uint32_t get_rx_timestamp_u32(void)
{
    uint8_t ts_tab[5];
    uint32_t ts = 0;
    int i;
    dwt_readrxtimestamp(ts_tab);

    for (i = 4; i >= 1; i--)
    {
        ts <<= 8;
        ts |= ts_tab[i];
    }
    return ts;
}

/**
 * Application entry point.
 */
int rx_diagnostics(void)
{
    /* Configure SPI rate, DW3000 supports up to 38 MHz */
    port_set_dw_ic_spi_fastrate();

    /* Reset DW IC */
    reset_DWIC(); /* Target specific drive of RSTn line into DW IC low for a period. */

    Sleep(2); // Time needed for DW3000 to start up (transition from INIT_RC to IDLE_RC, or could wait for SPIRDY event)

    while (!dwt_checkidlerc()) /* Need to make sure DW IC is in IDLE_RC before proceeding */
    { };

    if (dwt_initialise(DWT_DW_INIT) == DWT_ERROR)
    {
        test_run_info((unsigned char *)"INIT FAILED");
        while (1)
        { };
    }

    /* Configure DW IC. */
    int error = dwt_configure(&config);
    if(error)
    {
    	 char buf[200];
    	 snprintf(buf, 200,"CONFIG FAILED: %d \r\n",error);
        while (1)
        { test_run_info(buf); };
    }

    /* Activate event counters. */
    dwt_configeventcounters(1);

    /* Enable IC diagnostic calculation and logging */
    dwt_configciadiag(1);

    /* Loop forever receiving frames. */
    while (1)
    {
        int i;

        /* TESTING BREAKPOINT LOCATION #1 */

        /* Clear local RX buffer, rx_diag structure and accumulator values to avoid having leftovers from previous receptions  This is not necessary
         * but is included here to aid reading the data for each new frame.
         * This is a good place to put a breakpoint. Here (after first time through the loop) the local status register will be set for last event
         * and if a good receive has happened the data buffer will have the data in it, and frame_len will be set to the length of the RX frame. All
         * diagnostics data will also be available. */
        for (i = 0 ; i < FRAME_LEN_MAX; i++ )
        {
            rx_buffer[i] = 0;
        }
        for (i = 0 ; i < ACCUM_DATA_LEN; i++ )
        {
           accum_data[i] = 0;
        }

        memset(&rx_diag,0,sizeof(rx_diag));

        /* Activate reception immediately. See NOTE 4 below. */
        dwt_rxenable(DWT_START_RX_IMMEDIATE);

        /* Poll until a frame is properly received or an error/timeout occurs. See NOTE 5 below.
         * STATUS register is 5 bytes long but, as the event we are looking at is in the first byte of the register, we can use this simplest API
         * function to access it. */
        while (!((status_reg = dwt_read32bitreg(SYS_STATUS_ID)) & (SYS_STATUS_RXFCG_BIT_MASK | SYS_STATUS_ALL_RX_ERR)))
        { };

        if (status_reg & SYS_STATUS_RXFCG_BIT_MASK)
        {
            /* Clear good RX frame event in the DW IC status register. */
            dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_RXFCG_BIT_MASK);

            /* A frame has been received, copy it to our local buffer. */
            frame_len = dwt_read32bitreg(RX_FINFO_ID) & RX_FINFO_RXFLEN_BIT_MASK;
            if (frame_len <= FRAME_LEN_MAX)
            {
                dwt_readrxdata(rx_buffer, frame_len, 0);
            }


            /*if ranging is done, extract CIR and other data*/
		   dwt_readdiagnostics(&rx_diag);
		   uint16_t fp_int = rx_diag.ipatovFpIndex >> 6;
		   uint32_t fa_1 = rx_diag.ipatovF1;
		   uint32_t fa_2 = rx_diag.ipatovF2;
		   uint32_t fa_3 = rx_diag.ipatovF3;
		   uint16_t cir_power = rx_diag.ipatovPower;
		   uint16_t cir_peak1 = rx_diag.ipatovPeak >> 21;
		   uint32_t cir_peak2 = rx_diag.ipatovPeak & 0x1fffff;


		   dwt_readaccdata(CIR_data, 6097, 0);

		   int j = 0;
		   for(int i=1; i<(CIR_LEN*6+1); i+=6){
			     CIR_real[j] = ((CIR_data[i+2]) << 16 | (CIR_data[i+1]) << 8 | (CIR_data[i]));
			     CIR_imag[j] = ((CIR_data[i+5]) << 16 | (CIR_data[i+4]) << 8 | (CIR_data[i+3]));
			     if (CIR_real[j] & 0x00800000){
				  CIR_real[j] |= 0xff000000;
			     }

			     if (CIR_imag[j] & 0x00800000){
				  CIR_imag[j] |= 0xff000000;
			     }

			    //int32_t tempval = CIR_real[j];
				j++;
			}

		   int printf_len = 0;
		   printf_len += snprintf(print_buf+printf_len, PRINT_BUF_SIZE,"t: %u,",get_rx_timestamp_u32());
		   printf_len += snprintf(print_buf+printf_len, PRINT_BUF_SIZE,"----BEGIN CIR----");
		   for(j=CIR_DUMP_START; j<CIR_DUMP_START+CIR_DUMP_LEN; j+=1){
			   printf_len += snprintf(print_buf+printf_len, PRINT_BUF_SIZE,"%d%+dj,", CIR_real[j],CIR_imag[j]);
		   }
		   printf_len += snprintf(print_buf+printf_len, PRINT_BUF_SIZE,"----END CIR----\r\n");
			test_run_info((unsigned char *)print_buf);
        }
        else
        {
            /* Clear RX error events in the DW IC status register. */
            dwt_write32bitreg(SYS_STATUS_ID, SYS_STATUS_ALL_RX_ERR);
        }

        /* Read event counters. See NOTE 7. */
        dwt_readeventcounters(&event_cnt);
    }
}

#endif
/*****************************************************************************************************************************************************
 * NOTES:
 *
 * 1. In this example, maximum frame length is set to 127 bytes which is 802.15.4 UWB standard maximum frame length. DW IC supports an extended
 *    frame length (up to 1023 bytes long) mode which is not used in this example.
 * 2. Accumulator values are complex numbers: one 24-bit integer for real part and one 24-bit value for imaginary part, for each sample. In this
 *    example, we chose to read 3 values below the first path index and 3 values above. It must be noted that the first byte read when accessing the
 *    accumulator memory is always garbage and must be discarded, that is why the data length to read is increased by one byte here.
 * 3. In this example, the DW IC is put into IDLE state after calling dwt_initialise(). This means that a fast SPI rate of up to 20 MHz can be used
 *    thereafter.
 * 4. Manual reception activation is performed here but DW IC offers several features that can be used to handle more complex scenarios or to
 *    optimise system's overall performance (e.g. timeout after a given time, automatic re-enabling of reception in case of errors, etc.).
 * 5. We use polled mode of operation here to keep the example as simple as posrrupts".sible, but RXFCG and error/timeout status events can be used to generate
 *    interrupts. Please refer to DW IC User Manual for more details on "inte
 * 6. Here we chose to read only a few values around the first path index but it is possible and can be useful to get all accumulator values, using
 *    the relevant offset and length parameters. Reading the whole accumulator will require 4064 bytes of memory. First path value gotten from
 *    dwt_readdiagnostics is a 10.6 bits fixed point value calculated by the DW IC. By dividing this value by 64, we end up with the integer part of
 *    it. This value can be used to access the accumulator samples around the calculated first path index as it is done here.
 * 7. Event counters are never reset in this example but this can be done by re-enabling them (i.e. calling again dwt_configeventcounters with
 *    "enable" parameter set).
 * 8. The user is referred to DecaRanging ARM application (distributed with EVK1000 product) for additional practical example of usage, and to the
 *    DW IC API Guide for more details on the DW IC driver functions.
 ****************************************************************************************************************************************************/

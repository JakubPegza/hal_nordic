/* Copyright (c) 2017 - 2018, Nordic Semiconductor ASA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice, this
 *      list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright notice,
 *      this list of conditions and the following disclaimer in the documentation
 *      and/or other materials provided with the distribution.
 *
 *   3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * @file
 *   This file implements Finite State Machine of nRF 802.15.4 radio driver.
 *
 */

#include "nrf_802154_core.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf_802154.h"
#include "nrf_802154_config.h"
#include "nrf_802154_const.h"
#include "nrf_802154_critical_section.h"
#include "nrf_802154_debug.h"
#include "nrf_802154_notification.h"
#include "nrf_802154_peripherals.h"
#include "nrf_802154_pib.h"
#include "nrf_802154_procedures_duration.h"
#include "nrf_802154_rssi.h"
#include "nrf_802154_rx_buffer.h"
#include "nrf_802154_utils.h"
#include "nrf_802154_timer_coord.h"
#include "nrf_802154_types.h"
#include "nrf_802154_utils.h"
#include "hal/nrf_egu.h"
#include "nrf_error.h"
#include "hal/nrf_ppi.h"
#include "hal/nrf_radio.h"
#include "hal/nrf_timer.h"
#include "mac_features/nrf_802154_delayed_trx.h"
#include "mac_features/nrf_802154_filter.h"
#include "mac_features/nrf_802154_frame_parser.h"
#include "mac_features/ack_generator/nrf_802154_ack_data.h"
#include "mac_features/ack_generator/nrf_802154_ack_generator.h"
#include "rsch/nrf_802154_rsch.h"
#include "rsch/nrf_802154_rsch_crit_sect.h"
#include "platform/irq/nrf_802154_irq.h"

#include "nrf_802154_core_hooks.h"

#if ENABLE_FEM
#include "mpsl_fem_protocol_api.h"
#endif //ENABLE_FEM

#define EGU_EVENT                  NRF_EGU_EVENT_TRIGGERED15
#define EGU_TASK                   NRF_EGU_TASK_TRIGGER15
#define PPI_CHGRP0                 NRF_802154_PPI_CORE_GROUP                     ///< PPI group used to disable self-disabling PPIs
#define PPI_CHGRP0_DIS_TASK        NRF_PPI_TASK_CHG0_DIS                         ///< PPI task used to disable self-disabling PPIs

#define PPI_FEM_ABORT_GROUP        NRF_802154_PPI_FEM_ABORT_GROUP                ///< PPI group used to disable FEM

#define PPI_DISABLED_EGU           NRF_802154_PPI_RADIO_DISABLED_TO_EGU          ///< PPI that connects RADIO DISABLED event with EGU task
#define PPI_EGU_RAMP_UP            NRF_802154_PPI_EGU_TO_RADIO_RAMP_UP           ///< PPI that connects EGU event with RADIO TXEN or RXEN task
#define PPI_EGU_TIMER_START        NRF_802154_PPI_EGU_TO_TIMER_START             ///< PPI that connects EGU event with TIMER START task
#define PPI_CRCERROR_CLEAR         NRF_802154_PPI_RADIO_CRCERROR_TO_TIMER_CLEAR  ///< PPI that connects RADIO CRCERROR event with TIMER CLEAR task
#define PPI_CCAIDLE_FEM            NRF_802154_PPI_RADIO_CCAIDLE_TO_FEM_GPIOTE    ///< PPI that connects RADIO CCAIDLE event with GPIOTE tasks used by FEM
#define PPI_TIMER_TX_ACK           NRF_802154_PPI_TIMER_COMPARE_TO_RADIO_TXEN    ///< PPI that connects TIMER COMPARE event with RADIO TXEN task
#define PPI_CRCOK_DIS_PPI          NRF_802154_PPI_RADIO_CRCOK_TO_PPI_GRP_DISABLE ///< PPI that connects RADIO CRCOK event with task that disables PPI group

#if NRF_802154_DISABLE_BCC_MATCHING
#define PPI_ADDRESS_COUNTER_COUNT  NRF_802154_PPI_RADIO_ADDR_TO_COUNTER_COUNT    ///< PPI that connects RADIO ADDRESS event with TIMER COUNT task
#define PPI_CRCERROR_COUNTER_CLEAR NRF_802154_PPI_RADIO_CRCERROR_COUNTER_CLEAR   ///< PPI that connects RADIO CRCERROR event with TIMER CLEAR task
#endif  // NRF_802154_DISABLE_BCC_MATCHING

#if NRF_802154_DISABLE_BCC_MATCHING
#define SHORT_ADDRESS_BCSTART 0UL
#else // NRF_802154_DISABLE_BCC_MATCHING
#define SHORT_ADDRESS_BCSTART NRF_RADIO_SHORT_ADDRESS_BCSTART_MASK
#endif  // NRF_802154_DISABLE_BCC_MATCHING

/// Value set to SHORTS register when no shorts should be enabled.
#define SHORTS_IDLE             0

/// Value set to SHORTS register for RX operation.
#define SHORTS_RX               (NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK | \
                                 NRF_RADIO_SHORT_END_DISABLE_MASK |       \
                                 SHORT_ADDRESS_BCSTART)

#define SHORTS_RX_FREE_BUFFER   (NRF_RADIO_SHORT_RXREADY_START_MASK)

#define SHORTS_TX_ACK           (NRF_RADIO_SHORT_TXREADY_START_MASK | \
                                 NRF_RADIO_SHORT_PHYEND_DISABLE_MASK)

#define SHORTS_CCA_TX           (NRF_RADIO_SHORT_RXREADY_CCASTART_MASK | \
                                 NRF_RADIO_SHORT_CCABUSY_DISABLE_MASK |  \
                                 NRF_RADIO_SHORT_CCAIDLE_TXEN_MASK |     \
                                 NRF_RADIO_SHORT_TXREADY_START_MASK |    \
                                 NRF_RADIO_SHORT_PHYEND_DISABLE_MASK)

#define SHORTS_TX               (NRF_RADIO_SHORT_TXREADY_START_MASK | \
                                 NRF_RADIO_SHORT_PHYEND_DISABLE_MASK)

#define SHORTS_RX_ACK           (NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK | \
                                 NRF_RADIO_SHORT_END_DISABLE_MASK)

#define SHORTS_ED               (NRF_RADIO_SHORT_READY_EDSTART_MASK)

#define SHORTS_CCA              (NRF_RADIO_SHORT_RXREADY_CCASTART_MASK | \
                                 NRF_RADIO_SHORT_CCABUSY_DISABLE_MASK)

/// Delay before first check of received frame: 24 bits is PHY header and MAC Frame Control field.
#define BCC_INIT                (3 * 8)

/// Duration of single iteration of Energy Detection procedure
#define ED_ITER_DURATION        128U
/// Overhead of hardware preparation for ED procedure (aTurnaroundTime) [number of iterations]
#define ED_ITERS_OVERHEAD       2U

#define CRC_LENGTH              2               ///< Length of CRC in 802.15.4 frames [bytes]
#define CRC_POLYNOMIAL          0x011021        ///< Polynomial used for CRC calculation in 802.15.4 frames

#define MHMU_MASK               0xff000700      ///< Mask of known bytes in ACK packet
#define MHMU_PATTERN            0x00000200      ///< Values of known bytes in ACK packet
#define MHMU_PATTERN_DSN_OFFSET 24              ///< Offset of DSN in MHMU_PATTER [bits]

#define ACK_IFS                 TURNAROUND_TIME ///< Ack Inter Frame Spacing [us] - delay between last symbol of received frame and first symbol of transmitted Ack
#define TXRU_TIME               40              ///< Transmitter ramp up time [us]
#define EVENT_LAT               23              ///< END event latency [us]

#define MAX_CRIT_SECT_TIME      60              ///< Maximal time that the driver spends in single critical section.

#define LQI_VALUE_FACTOR        4               ///< Factor needed to calculate LQI value based on data from RADIO peripheral
#define LQI_MAX                 0xff            ///< Maximal LQI value

/** Get LQI of given received packet. If CRC is calculated by hardware LQI is included instead of CRC
 *  in the frame. Length is stored in byte with index 0; CRC is 2 last bytes.
 */
#define RX_FRAME_LQI(data)      ((data)[(data)[0] - 1])

#if NRF_802154_RX_BUFFERS > 1
/// Pointer to currently used receive buffer.
static rx_buffer_t * mp_current_rx_buffer;

#else
/// If there is only one buffer use const pointer to the receive buffer.
static rx_buffer_t * const mp_current_rx_buffer = &nrf_802154_rx_buffers[0];

#endif

/** Prototype for the Radio IRQ handling routine. */
static void irq_handler(void);

static const uint8_t * mp_ack;         ///< Pointer to Ack frame buffer.
static const uint8_t * mp_tx_data;     ///< Pointer to the data to transmit.
static uint32_t        m_ed_time_left; ///< Remaining time of the current energy detection procedure [us].
static uint8_t         m_ed_result;    ///< Result of the current energy detection procedure.

static volatile radio_state_t m_state; ///< State of the radio driver.

#if ENABLE_FEM
/// Common parameters for the FEM handling.

static const mpsl_fem_event_t m_activate_rx_cc0 =
{
    .type         = MPSL_FEM_EVENT_TYPE_TIMER,
    .override_ppi = false,
    .event.timer  =
    {
        .p_timer_instance     = NRF_802154_TIMER_INSTANCE,
        .counter_period.start = 0,
        .counter_period.end   = RX_RAMP_UP_TIME,
        .compare_channel_mask = ((1 << NRF_TIMER_CC_CHANNEL0) | (1 << NRF_TIMER_CC_CHANNEL2)),
    },
};

static const mpsl_fem_event_t m_activate_tx_cc0 =
{
    .type         = MPSL_FEM_EVENT_TYPE_TIMER,
    .override_ppi = false,
    .event.timer  =
    {
        .p_timer_instance     = NRF_802154_TIMER_INSTANCE,
        .counter_period.start = 0,
        .counter_period.end   = TX_RAMP_UP_TIME,
        .compare_channel_mask = ((1 << NRF_TIMER_CC_CHANNEL0) | (1 << NRF_TIMER_CC_CHANNEL2)),
    },
};

static const mpsl_fem_event_t m_ccaidle =
{
    .type                           = MPSL_FEM_EVENT_TYPE_GENERIC,
    .override_ppi                   = true,
    .ppi_ch_id                      = PPI_CCAIDLE_FEM,
    .event.generic.register_address = ((uint32_t)NRF_RADIO_BASE + (uint32_t)NRF_RADIO_EVENT_CCAIDLE)
};
#endif // ENABLE_FEM

typedef struct
{
    bool frame_filtered        : 1; ///< If frame being received passed filtering operation.
    bool rx_timeslot_requested : 1; ///< If timeslot for the frame being received is already requested.

#if !NRF_802154_DISABLE_BCC_MATCHING
    bool psdu_being_received   : 1; ///< If PSDU is currently being received.

#endif  // !NRF_802154_DISABLE_BCC_MATCHING
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    bool tx_started   : 1; ///< If the requested transmission has started.

#endif  // NRF_802154_TX_STARTED_NOTIFY_ENABLED
    bool rssi_started : 1;
} nrf_802154_flags_t;
static nrf_802154_flags_t m_flags;               ///< Flags used to store the current driver state.

static volatile bool m_rsch_timeslot_is_granted; ///< State of the RSCH timeslot.

/***************************************************************************************************
 * @section Common core operations
 **************************************************************************************************/

/** Set driver state.
 *
 * @param[in]  state  Driver state to set.
 */
static void state_set(radio_state_t state)
{
    m_state = state;

    nrf_802154_log(EVENT_SET_STATE, (uint32_t)state);
}

/** Clear flags describing frame being received. */
static void rx_flags_clear(void)
{
    m_flags.frame_filtered        = false;
    m_flags.rx_timeslot_requested = false;
#if !NRF_802154_DISABLE_BCC_MATCHING
    m_flags.psdu_being_received = false;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
}

/** Request the RSSI measurement. */
static void rssi_measure(void)
{
    m_flags.rssi_started = true;
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
}

/** Wait for the RSSI measurement. */
static void rssi_measurement_wait(void)
{
    while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RSSIEND))
    {
        // Intentionally empty: This function is called from a critical section.
        // WFE would not be waken up by a RADIO event.
    }
}

/** Get the result of the last RSSI measurement.
 *
 * @returns  Result of the last RSSI measurement in dBm.
 */
static int8_t rssi_last_measurement_get(void)
{
    uint8_t rssi_sample = nrf_radio_rssi_sample_get(NRF_RADIO);

    rssi_sample = nrf_802154_rssi_sample_corrected_get(rssi_sample);

    return -((int8_t)rssi_sample);
}

/** Get LQI of a received frame.
 *
 * @param[in]  p_data  Pointer to buffer containing PHR and PSDU of received frame
 *
 * @returns  LQI of given frame.
 */
static uint8_t lqi_get(const uint8_t * p_data)
{
    uint32_t lqi = RX_FRAME_LQI(p_data);

    lqi  = nrf_802154_rssi_lqi_corrected_get(lqi);
    lqi *= LQI_VALUE_FACTOR;

    if (lqi > LQI_MAX)
    {
        lqi = LQI_MAX;
    }

    return (uint8_t)lqi;
}

static void received_frame_notify(uint8_t * p_data)
{
    nrf_802154_notify_received(p_data,                      // data
                               rssi_last_measurement_get(), // rssi
                               lqi_get(p_data));            // lqi
}

/** Allow nesting critical sections and notify MAC layer that a frame was received. */
static void received_frame_notify_and_nesting_allow(uint8_t * p_data)
{
    nrf_802154_critical_section_nesting_allow();

    received_frame_notify(p_data);

    nrf_802154_critical_section_nesting_deny();
}

/** Notify MAC layer that receive procedure failed. */
static void receive_failed_notify(nrf_802154_rx_error_t error)
{
    nrf_802154_critical_section_nesting_allow();

    nrf_802154_notify_receive_failed(error);

    nrf_802154_critical_section_nesting_deny();
}

/** Notify MAC layer that transmission of requested frame has started. */
static void transmit_started_notify(void)
{
    const uint8_t * p_frame = mp_tx_data;

    if (nrf_802154_core_hooks_tx_started(p_frame))
    {
        nrf_802154_tx_started(p_frame);
    }

}

#if !NRF_802154_DISABLE_BCC_MATCHING
/** Notify that reception of a frame has started. */
static void receive_started_notify(void)
{
    const uint8_t * p_frame = mp_current_rx_buffer->data;

    nrf_802154_core_hooks_rx_started(p_frame);
}

#endif

/** Notify MAC layer that a frame was transmitted. */
static void transmitted_frame_notify(uint8_t * p_ack, int8_t power, uint8_t lqi)
{
    const uint8_t * p_frame = mp_tx_data;

    nrf_802154_critical_section_nesting_allow();

    nrf_802154_core_hooks_transmitted(p_frame);
    nrf_802154_notify_transmitted(p_frame, p_ack, power, lqi);

    nrf_802154_critical_section_nesting_deny();
}

/** Notify MAC layer that transmission procedure failed. */
static void transmit_failed_notify(nrf_802154_tx_error_t error)
{
    const uint8_t * p_frame = mp_tx_data;

    if (nrf_802154_core_hooks_tx_failed(p_frame, error))
    {
        nrf_802154_notify_transmit_failed(p_frame, error);
    }
}

/** Allow nesting critical sections and notify MAC layer that transmission procedure failed. */
static void transmit_failed_notify_and_nesting_allow(nrf_802154_tx_error_t error)
{
    nrf_802154_critical_section_nesting_allow();

    transmit_failed_notify(error);

    nrf_802154_critical_section_nesting_deny();
}

/** Notify MAC layer that energy detection procedure ended. */
static void energy_detected_notify(uint8_t result)
{
    nrf_802154_critical_section_nesting_allow();

    nrf_802154_notify_energy_detected(result);

    nrf_802154_critical_section_nesting_deny();
}

/** Notify MAC layer that CCA procedure ended. */
static void cca_notify(bool result)
{
    nrf_802154_critical_section_nesting_allow();

    nrf_802154_notify_cca(result);

    nrf_802154_critical_section_nesting_deny();
}

/** Update CCA configuration in RADIO registers. */
static void cca_configuration_update(void)
{
    nrf_802154_cca_cfg_t cca_cfg;

    nrf_802154_pib_cca_cfg_get(&cca_cfg);
    nrf_radio_cca_configure(NRF_RADIO,
                            cca_cfg.mode,
                            nrf_802154_rssi_cca_ed_threshold_corrected_get(cca_cfg.ed_threshold),
                            cca_cfg.corr_threshold,
                            cca_cfg.corr_limit);
}

/** Check if PSDU is currently being received.
 *
 * @returns True if radio is receiving PSDU, false otherwise.
 */
static bool psdu_is_being_received(void)
{
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE,
                           nrf_timer_capture_task_get(NRF_TIMER_CC_CHANNEL0));
    uint32_t counter = nrf_timer_cc_get(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL0);

    assert(counter <= 1);

    return counter > 0;
#else // NRF_802154_DISABLE_BCC_MATCHING
    return m_flags.psdu_being_received;
#endif  // NRF_802154_DISABLE_BCC_MATCHING
}

/** Check if requested transmission has already started.
 *
 * @retval true   The transmission has started.
 * @retval false  The transmission has not started.
 */
static bool transmission_has_started(void)
{
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    return m_flags.tx_started;
#else // NRF_802154_TX_STARTED_NOTIFY_ENABLED
    return nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
#endif  // NRF_802154_TX_STARTED_NOTIFY_ENABLED
}

/** Check if timeslot is currently granted.
 *
 * @retval true   The timeslot is granted.
 * @retval false  The timeslot is not granted.
 */
static bool timeslot_is_granted(void)
{
    return m_rsch_timeslot_is_granted;
}

/***************************************************************************************************
 * @section RX buffer management
 **************************************************************************************************/

/** Set currently used rx buffer to given address.
 *
 * @param[in]  p_rx_buffer  Pointer to receive buffer that should be used now.
 */
static void rx_buffer_in_use_set(rx_buffer_t * p_rx_buffer)
{
#if NRF_802154_RX_BUFFERS > 1
    mp_current_rx_buffer = p_rx_buffer;
#else
    (void)p_rx_buffer;
#endif
}

/** Check if currently there is available rx buffer.
 *
 * @retval true   There is available rx buffer.
 * @retval false  Currently there is no available rx buffer.
 */
static bool rx_buffer_is_available(void)
{
    return (mp_current_rx_buffer != NULL) && (mp_current_rx_buffer->free);
}

/** Get pointer to available rx buffer.
 *
 * @returns Pointer to available rx buffer or NULL if rx buffer is not available.
 */
static uint8_t * rx_buffer_get(void)
{
    return rx_buffer_is_available() ? mp_current_rx_buffer->data : NULL;
}

/***************************************************************************************************
 * @section Radio parameters calculators
 **************************************************************************************************/

/** Set radio channel
 *
 *  @param[in]  channel  Channel number to set (11-26).
 */
static void channel_set(uint8_t channel)
{
    assert(channel >= 11 && channel <= 26);

    nrf_radio_frequency_set(NRF_RADIO, 2405 + 5 * (channel - 11));
}

/***************************************************************************************************
 * @section ACK transmission management
 **************************************************************************************************/

/** Check if ACK is requested in given frame.
 *
 * @param[in]  p_frame  Pointer to a frame to check.
 *
 * @retval  true   ACK is requested in given frame.
 * @retval  false  ACK is not requested in given frame.
 */
static bool ack_is_requested(const uint8_t * p_frame)
{
    return nrf_802154_frame_parser_ar_bit_is_set(p_frame);
}

/***************************************************************************************************
 * @section ACK receiving management
 **************************************************************************************************/

/** Enable hardware ACK matching accelerator. */
static void ack_matching_enable(void)
{
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_MHRMATCH);
    nrf_radio_mhmu_search_pattern_set(NRF_RADIO,
                                      MHMU_PATTERN |
                                      ((uint32_t)mp_tx_data[DSN_OFFSET] <<
                                       MHMU_PATTERN_DSN_OFFSET));
}

/** Disable hardware ACK matching accelerator. */
static void ack_matching_disable(void)
{
    nrf_radio_mhmu_search_pattern_set(NRF_RADIO, 0);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_MHRMATCH);
}

/** Check if hardware ACK matching accelerator matched ACK pattern in received frame.
 *
 * @retval  true   ACK matching accelerator matched ACK pattern.
 * @retval  false  ACK matching accelerator did not match ACK pattern.
 */
static bool ack_is_matched(void)
{
    return (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_MHRMATCH)) &&
           (nrf_radio_crc_status_check(NRF_RADIO));
}

/***************************************************************************************************
 * @section RADIO peripheral management
 **************************************************************************************************/

/** Initialize radio peripheral. */
static void nrf_radio_init(void)
{
    nrf_radio_packet_conf_t packet_conf;

    nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_IEEE802154_250KBIT);

    memset(&packet_conf, 0, sizeof(packet_conf));
    packet_conf.lflen  = 8;
    packet_conf.plen   = NRF_RADIO_PREAMBLE_LENGTH_32BIT_ZERO;
    packet_conf.crcinc = true;
    packet_conf.maxlen = MAX_PACKET_SIZE;
    nrf_radio_packet_configure(NRF_RADIO, &packet_conf);

    nrf_radio_modecnf0_set(NRF_RADIO, true, 0);

    // Configure CRC
    nrf_radio_crc_configure(NRF_RADIO, CRC_LENGTH, NRF_RADIO_CRC_ADDR_IEEE802154, CRC_POLYNOMIAL);

    // Configure CCA
    cca_configuration_update();

    // Configure MAC Header Match Unit
    nrf_radio_mhmu_search_pattern_set(NRF_RADIO, 0);
    nrf_radio_mhmu_pattern_mask_set(NRF_RADIO, MHMU_MASK);

    // Set channel
    channel_set(nrf_802154_pib_channel_get());
}

/** Reset radio peripheral. */
static void nrf_radio_reset(void)
{
    nrf_radio_power_set(NRF_RADIO, false);
    nrf_radio_power_set(NRF_RADIO, true);

    nrf_802154_log(EVENT_RADIO_RESET, 0);
}

/** Initialize interrupts for radio peripheral. */
static void irq_init(void)
{
#if !NRF_802154_IRQ_PRIORITY_ALLOWED(NRF_802154_IRQ_PRIORITY)
#error NRF_802154_IRQ_PRIORITY value out of the allowed range.
#endif
#if NRF_802154_INTERNAL_RADIO_IRQ_HANDLING
    nrf_802154_irq_init(RADIO_IRQn, NRF_802154_IRQ_PRIORITY, irq_handler);
#endif
}

/** Deinitialize interrupts for radio peripheral. */
static void irq_deinit(void)
{
    nrf_802154_irq_disable(RADIO_IRQn);
    nrf_802154_irq_clear_pending(RADIO_IRQn);
}

/***************************************************************************************************
 * @section TIMER peripheral management
 **************************************************************************************************/

/** Initialize TIMER peripheral used by the driver. */
static void nrf_timer_init(void)
{
    nrf_timer_mode_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_MODE_TIMER);
    nrf_timer_bit_width_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_BIT_WIDTH_32);
    nrf_timer_frequency_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_FREQ_1MHz);

#if NRF_802154_DISABLE_BCC_MATCHING
    // Setup timer for detecting PSDU reception.
    nrf_timer_mode_set(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_MODE_COUNTER);
    nrf_timer_bit_width_set(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_BIT_WIDTH_8);
#endif
}

/***************************************************************************************************
 * @section Energy detection management
 **************************************************************************************************/

/** Get ED result value.
 *
 * @returns ED result based on data collected during Energy Detection procedure.
 */
static uint8_t ed_result_get(void)
{
    uint32_t result = m_ed_result;

    result  = nrf_802154_rssi_ed_corrected_get(result);
    result *= ED_RESULT_FACTOR;

    if (result > ED_RESULT_MAX)
    {
        result = ED_RESULT_MAX;
    }

    return (uint8_t)result;
}

/** Setup next iteration of energy detection procedure.
 *
 *  Energy detection procedure is performed in iterations to make sure it is performed for requested
 *  time regardless radio arbitration.
 *
 *  @param[in]  Remaining time of energy detection procedure [us].
 *
 *  @retval  true   Next iteration of energy detection procedure will be performed now.
 *  @retval  false  Next iteration of energy detection procedure will not be performed now due to
 *                  ending timeslot.
 */
static bool ed_iter_setup(uint32_t time_us)
{
    uint32_t us_left_in_timeslot = nrf_802154_rsch_timeslot_us_left_get();
    uint32_t next_ed_iters       = us_left_in_timeslot / ED_ITER_DURATION;

    if (next_ed_iters > ED_ITERS_OVERHEAD)
    {
        next_ed_iters -= ED_ITERS_OVERHEAD;

        if ((time_us / ED_ITER_DURATION) < next_ed_iters)
        {
            m_ed_time_left = 0;
            next_ed_iters  = time_us / ED_ITER_DURATION;
        }
        else
        {
            m_ed_time_left = time_us - (next_ed_iters * ED_ITER_DURATION);
            next_ed_iters--; // Time of ED procedure is (next_ed_iters + 1) * 128us
        }

        nrf_radio_ed_loop_count_set(NRF_RADIO, next_ed_iters);

        return true;
    }
    else
    {
        // Silently wait for a new timeslot

        m_ed_time_left = time_us;

        return false;
    }
}

/***************************************************************************************************
 * @section FSM transition request sub-procedures
 **************************************************************************************************/

/** Wait time needed to propagate event through PPI to EGU.
 *
 * During detection if trigger of DISABLED event caused start of hardware procedure, detecting
 * function needs to wait until event is propagated from RADIO through PPI to EGU. This delay is
 * required to make sure EGU event is set if hardware was prepared before DISABLED event was
 * triggered.
 */
static inline void ppi_and_egu_delay_wait(void)
{
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
    __ASM("nop");
}

/** Detect if PPI starting EGU for current operation worked.
 *
 * @retval  true   PPI worked.
 * @retval  false  PPI did not work. DISABLED task should be triggered.
 */
static bool ppi_egu_worked(void)
{
    // Detect if PPIs were set before DISABLED event was notified. If not trigger DISABLE
    if (nrf_radio_state_get(NRF_RADIO) != NRF_RADIO_STATE_DISABLED)
    {
        // If RADIO state is not DISABLED, it means that RADIO is still ramping down or already
        // started ramping up.
        return true;
    }

    // Wait for PPIs
    ppi_and_egu_delay_wait();

    if (nrf_egu_event_check(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT))
    {
        // If EGU event is set, procedure is running.
        return true;
    }
    else
    {
        return false;
    }
}

/** Set PPIs to connect DISABLED->EGU->RAMP_UP
 *
 * @param[in]  ramp_up_task    Task triggered to start ramp up procedure.
 * @param[in]  self_disabling  If PPI should disable itself.
 */
static void ppis_for_egu_and_ramp_up_set(nrf_radio_task_t ramp_up_task, bool self_disabling)
{
    if (self_disabling)
    {
        nrf_ppi_channel_and_fork_endpoint_setup(NRF_PPI,
                                                PPI_EGU_RAMP_UP,
                                                (uint32_t)nrf_egu_event_address_get(
                                                    NRF_802154_SWI_EGU_INSTANCE,
                                                    EGU_EVENT),
                                                (uint32_t)nrf_radio_task_address_get(
                                                    NRF_RADIO,
                                                    ramp_up_task),
                                                (uint32_t)nrf_ppi_task_address_get(
                                                    NRF_PPI,
                                                    PPI_CHGRP0_DIS_TASK));
    }
    else
    {
        nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                       PPI_EGU_RAMP_UP,
                                       (uint32_t)nrf_egu_event_address_get(
                                           NRF_802154_SWI_EGU_INSTANCE,
                                           EGU_EVENT),
                                       (uint32_t)nrf_radio_task_address_get(
                                           NRF_RADIO,
                                           ramp_up_task));
    }

    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_DISABLED_EGU,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_DISABLED),
                                   (uint32_t)nrf_egu_task_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_TASK));

    if (self_disabling)
    {
        nrf_ppi_channel_include_in_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);
    }

    nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_RAMP_UP);
    nrf_ppi_channel_enable(NRF_PPI, PPI_DISABLED_EGU);
}

/** Configure FEM to set LNA at appropriate time. */
static void fem_for_lna_set(void)
{
#if ENABLE_FEM
    if (mpsl_fem_lna_configuration_set(&m_activate_rx_cc0, NULL) == 0)
    {
        uint32_t event_addr = (uint32_t)nrf_egu_event_address_get(NRF_802154_SWI_EGU_INSTANCE,
                                                                  EGU_EVENT);
        uint32_t task_addr = (uint32_t)nrf_timer_task_address_get(NRF_802154_TIMER_INSTANCE,
                                                                  NRF_TIMER_TASK_START);

        nrf_timer_shorts_enable(m_activate_rx_cc0.event.timer.p_timer_instance,
                                NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
        nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_EGU_TIMER_START, event_addr, task_addr);
        nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_TIMER_START);
    }
#endif // ENABLE_FEM
}

/** Reset FEM configuration for LNA.
 *
 * @param[in]  timer_short_mask  Mask of shorts that should be disabled on FEM timer.
 */
static void fem_for_lna_reset(void)
{
#if ENABLE_FEM
    mpsl_fem_lna_configuration_clear();
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
#endif // ENABLE_FEM
}

/** Configure FEM to set PA at appropriate time. */
static void fem_for_pa_set(void)
{
#if ENABLE_FEM
    if (mpsl_fem_pa_configuration_set(&m_activate_tx_cc0, NULL) == 0)
    {
        uint32_t event_addr = (uint32_t)nrf_egu_event_address_get(NRF_802154_SWI_EGU_INSTANCE,
                                                                  EGU_EVENT);
        uint32_t task_addr = (uint32_t)nrf_timer_task_address_get(NRF_802154_TIMER_INSTANCE,
                                                                  NRF_TIMER_TASK_START);

        nrf_timer_shorts_enable(m_activate_tx_cc0.event.timer.p_timer_instance,
                                NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
        nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_EGU_TIMER_START, event_addr, task_addr);
        nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_TIMER_START);
    }
#endif // ENABLE_FEM
}

/** Reset FEM configuration for PA. */
static void fem_for_pa_reset(void)
{
#if ENABLE_FEM
    mpsl_fem_pa_configuration_clear();
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
    mpsl_fem_deactivate_now(MPSL_FEM_PA);
#endif // ENABLE_FEM
}

/** Configure FEM for TX procedure. */
static void fem_for_tx_set(bool cca)
{
#if ENABLE_FEM
    bool success;

    if (cca)
    {
        bool pa_set  = false;
        bool lna_set = false;

        if (mpsl_fem_lna_configuration_set(&m_activate_rx_cc0, &m_ccaidle) == 0)
        {
            lna_set = true;
        }

        if (mpsl_fem_pa_configuration_set(&m_ccaidle, NULL) == 0)
        {
            pa_set = true;
        }

        success = pa_set || lna_set;

    }
    else
    {
        success = (mpsl_fem_pa_configuration_set(&m_activate_tx_cc0, NULL) == 0);
    }

    if (success)
    {
        nrf_timer_shorts_enable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

        uint32_t egu_event_addr = (uint32_t)nrf_egu_event_address_get(NRF_802154_SWI_EGU_INSTANCE,
                                                                      EGU_EVENT);
        uint32_t timer_task_addr = (uint32_t)nrf_timer_task_address_get(NRF_802154_TIMER_INSTANCE,
                                                                        NRF_TIMER_TASK_START);

        nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_EGU_TIMER_START, egu_event_addr, timer_task_addr);

        mpsl_fem_abort_extend(PPI_EGU_RAMP_UP, PPI_FEM_ABORT_GROUP);
        mpsl_fem_abort_extend(PPI_EGU_TIMER_START, PPI_FEM_ABORT_GROUP);

        nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_TIMER_START);
    }
#else
    (void)cca;
#endif // ENABLE_FEM
}

/** Reset FEM for TX procedure. */
static void fem_for_tx_reset(bool disable_ppi_egu_timer_start)
{
#if ENABLE_FEM
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK |
                             NRF_TIMER_SHORT_COMPARE1_STOP_MASK);

    switch (m_state)
    {
        case RADIO_STATE_CCA_TX:
        case RADIO_STATE_TX:
            mpsl_fem_pa_configuration_clear();
            break;

        default:
            assert(false);
            break;
    }

    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    if (disable_ppi_egu_timer_start)
    {
        nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
    }
#else
 (void)disable_ppi_egu_timer_start;
#endif // ENABLE_FEM
}

static void fem_for_rx_set(void)
{
#if ENABLE_FEM
    uint32_t delta_time;
    nrf_timer_shorts_enable(NRF_802154_TIMER_INSTANCE,
                            NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

    if (mpsl_fem_lna_configuration_set(&m_activate_rx_cc0, NULL) == 0)
    {
        delta_time = nrf_timer_cc_get(NRF_802154_TIMER_INSTANCE,
                                      NRF_TIMER_CC_CHANNEL0);
    }
    else
    {
        delta_time = 1;
        nrf_timer_cc_set(NRF_802154_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL0, delta_time);
    }

    nrf_timer_cc_set(NRF_802154_TIMER_INSTANCE,
                     NRF_TIMER_CC_CHANNEL1,
                     delta_time + ACK_IFS - TXRU_TIME - EVENT_LAT);
#endif // ENABLE_FEM
}

static bool fem_prepare_powerdown(uint32_t event_addr)
{
#if ENABLE_FEM
    return mpsl_fem_prepare_powerdown(NRF_802154_TIMER_INSTANCE,
                                      NRF_TIMER_CC_CHANNEL0,
                                      PPI_EGU_TIMER_START,
                                      event_addr);
#else
    (void)event_addr;
    return false;
#endif // ENABLE_FEM
}

static void fem_deactivate_now(void)
{
#if ENABLE_FEM
    mpsl_fem_deactivate_now(MPSL_FEM_ALL);
#endif // ENABLE_FEM
}

static void fem_cleanup(void)
{
#if ENABLE_FEM
    mpsl_fem_cleanup();
#endif // ENABLE_FEM
}
/** Restart RX procedure after frame was received (and no ACK transmitted). */
static void rx_restart(bool set_shorts)
{
    // Disable PPIs on DISABLED event to control TIMER.
    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);

    if (set_shorts)
    {
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX);
    }

    // Restart TIMER.
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

#if NRF_802154_DISABLE_BCC_MATCHING
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    // Enable self-disabled PPI or PPI disabled by CRCOK
    nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_RAMP_UP);

    rx_flags_clear();
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);
    nrf_radio_bcc_set(NRF_RADIO, BCC_INIT);
#endif // !NRF_802154_DISABLE_BCC_MATCHING

    // Enable PPIs on DISABLED event and clear event to detect if PPI worked
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);
    nrf_ppi_channel_enable(NRF_PPI, PPI_DISABLED_EGU);

    // Prepare the timer coordinator to get a precise timestamp of the CRCOK event.
    nrf_802154_timer_coord_timestamp_prepare(
        (uint32_t)nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_CRCOK));

    if (!ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }
}

/** Check if time remaining in the timeslot is long enough to process whole critical section. */
static bool remaining_timeslot_time_is_enough_for_crit_sect(void)
{
    return nrf_802154_rsch_timeslot_us_left_get() >= MAX_CRIT_SECT_TIME;
}

/** Check if critical section can be processed at the moment.
 *
 * @note This function returns valid result only inside critical section.
 *
 * @retval true   There is enough time in current timeslot or timeslot is denied at the moment.
 * @retval false  Current timeslot ends too shortly to process critical section inside.
 */
static bool critical_section_can_be_processed_now(void)
{
    return !timeslot_is_granted() || remaining_timeslot_time_is_enough_for_crit_sect();
}

/** Enter critical section and verify if there is enough time to complete operations within. */
static bool critical_section_enter_and_verify_timeslot_length(void)
{
    bool result = nrf_802154_critical_section_enter();

    if (result)
    {
        if (!critical_section_can_be_processed_now())
        {
            result = false;

            nrf_802154_critical_section_exit();
        }
    }

    return result;
}

/** Terminate Falling Asleep procedure. */
static void falling_asleep_terminate(void)
{
    if (timeslot_is_granted())
    {
        nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);
    }
}

/** Terminate Sleep procedure. */
static void sleep_terminate(void)
{
    nrf_802154_rsch_crit_sect_prio_request(RSCH_PRIO_MAX);
}

/** Terminate RX procedure. */
static void rx_terminate(void)
{
    uint32_t ints_to_disable = 0;

    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_disable(NRF_PPI, PPI_CRCERROR_CLEAR);
    nrf_ppi_channel_disable(NRF_PPI, PPI_CRCOK_DIS_PPI);
    nrf_ppi_channel_disable(NRF_PPI, PPI_ADDRESS_COUNTER_COUNT);
    nrf_ppi_channel_disable(NRF_PPI, PPI_CRCERROR_COUNTER_CLEAR);
    nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_CRCERROR_CLEAR, 0, 0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_CRCERROR_CLEAR, 0);
#endif // NRF_802154_DISABLE_BCC_MATCHING

#if ENABLE_FEM
    // Disable LNA
    mpsl_fem_lna_configuration_clear();
#endif // ENABLE_FEM
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_ppi_channel_remove_from_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);

#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_TIMER_START, 0);
#else // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_RAMP_UP, 0);
#endif  // NRF_802154_DISABLE_BCC_MATCHING

    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

#if NRF_802154_DISABLE_BCC_MATCHING
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    if (timeslot_is_granted())
    {
#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
        ints_to_disable |= NRF_RADIO_INT_CRCERROR_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
#if !NRF_802154_DISABLE_BCC_MATCHING
        ints_to_disable |= NRF_RADIO_INT_BCMATCH_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
        ints_to_disable |= NRF_RADIO_INT_CRCOK_MASK;
        nrf_radio_int_disable(NRF_RADIO, ints_to_disable);
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_IDLE);
        bool shutdown = fem_prepare_powerdown(nrf_radio_event_address_get
                                                (NRF_RADIO, NRF_RADIO_EVENT_DISABLED));

        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        if (shutdown)
        {
            while (!nrf_timer_event_check(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0))
            {
                // Wait until the event is set.
            }
            nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
        }
    }
}

/** Terminate TX ACK procedure. */
static void tx_ack_terminate(void)
{
    uint32_t ints_to_disable = 0;

    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_disable(NRF_PPI, PPI_CRCERROR_CLEAR);
    nrf_ppi_channel_disable(NRF_PPI, PPI_CRCOK_DIS_PPI);
    nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_CRCERROR_CLEAR, 0, 0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_CRCERROR_CLEAR, 0);
#endif // NRF_802154_DISABLE_BCC_MATCHING

#if ENABLE_FEM
    // Disable PA
    mpsl_fem_pa_configuration_clear();
#endif // ENABLE_FEM

    nrf_ppi_channel_remove_from_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_RAMP_UP, 0);
#endif // !NRF_802154_DISABLE_BCC_MATCHING

    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE,
                             NRF_TIMER_SHORT_COMPARE0_STOP_MASK);

#if NRF_802154_DISABLE_BCC_MATCHING
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    nrf_timer_shorts_disable(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    if (timeslot_is_granted())
    {
        ints_to_disable = NRF_RADIO_INT_PHYEND_MASK;

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        ints_to_disable |= NRF_RADIO_INT_ADDRESS_MASK;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

        nrf_radio_int_disable(NRF_RADIO, ints_to_disable);
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_IDLE);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }
}

/** Terminate TX procedure. */
static void tx_terminate(void)
{
    uint32_t ints_to_disable;

    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);

    fem_for_tx_reset(true);

    nrf_ppi_channel_remove_from_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_RAMP_UP, 0);

    if (timeslot_is_granted())
    {
        ints_to_disable = NRF_RADIO_INT_PHYEND_MASK | NRF_RADIO_INT_CCABUSY_MASK;

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        ints_to_disable |= NRF_RADIO_INT_ADDRESS_MASK;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

        nrf_radio_int_disable(NRF_RADIO, ints_to_disable);
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_IDLE);
        bool shutdown = fem_prepare_powerdown(nrf_radio_event_address_get
                                                (NRF_RADIO, NRF_RADIO_EVENT_DISABLED));

        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_CCASTOP);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        if (shutdown)
        {
            while (!nrf_timer_event_check(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0))
            {
                // Wait until the event is set.
            }
            nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
        }
    }
}

/** Terminate RX ACK procedure. */
static void rx_ack_terminate(void)
{
    uint32_t ints_to_disable;

    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);

    fem_for_lna_reset();

    nrf_ppi_channel_remove_from_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_RAMP_UP, 0);

    if (timeslot_is_granted())
    {
        ints_to_disable  = NRF_RADIO_INT_END_MASK;
        ints_to_disable |= NRF_RADIO_INT_ADDRESS_MASK;

        nrf_radio_int_disable(NRF_RADIO, ints_to_disable);
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_IDLE);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

        ack_matching_disable();
    }
}

/** Terminate ED procedure. */
static void ed_terminate(void)
{
    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);

    fem_for_lna_reset();

    nrf_ppi_channel_remove_from_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_RAMP_UP, 0);

    if (timeslot_is_granted())
    {
        bool shutdown = fem_prepare_powerdown(nrf_radio_event_address_get
                                                (NRF_RADIO, NRF_RADIO_EVENT_DISABLED));

        nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_EDEND_MASK);
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_IDLE);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_EDSTOP);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

        if (shutdown)
        {
            while (!nrf_timer_event_check(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0))
            {
                // Wait until the event is set.
            }
            nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
        }
    }
}

/** Terminate CCA procedure. */
static void cca_terminate(void)
{
    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);

    fem_for_lna_reset();

    nrf_ppi_channel_remove_from_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_RAMP_UP, 0);

    if (timeslot_is_granted())
    {
        bool shutdown = fem_prepare_powerdown(nrf_radio_event_address_get
                                                (NRF_RADIO, NRF_RADIO_EVENT_DISABLED));

        nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_CCABUSY_MASK | NRF_RADIO_INT_CCAIDLE_MASK);
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_IDLE);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_CCASTOP);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

        if (shutdown)
        {
            while (!nrf_timer_event_check(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0))
            {
                // Wait until the event is set.
            }
            nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
        }
    }
}

/** Terminate Continuous Carrier procedure. */
static void continuous_carrier_terminate(void)
{
    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);
    nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_RAMP_UP);

    fem_for_pa_reset();

    if (timeslot_is_granted())
    {
        bool shutdown = fem_prepare_powerdown(nrf_radio_event_address_get
                                                (NRF_RADIO, NRF_RADIO_EVENT_DISABLED));

        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        if (shutdown)
        {
            while (!nrf_timer_event_check(NRF_802154_TIMER_INSTANCE, NRF_TIMER_EVENT_COMPARE0))
            {
                // Wait until the event is set.
            }
            nrf_timer_shorts_disable(NRF_802154_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
            nrf_ppi_channel_disable(NRF_PPI, PPI_EGU_TIMER_START);
        }
    }
}

/** Terminate ongoing operation.
 *
 * This function is called when MAC layer requests transition to another operation.
 *
 * After calling this function RADIO should enter DISABLED state and Radio Scheduler
 * should be in continuous mode.
 *
 * @param[in]  term_lvl      Termination level of this request. Selects procedures to abort.
 * @param[in]  req_orig      Module that originates termination request.
 * @param[in]  notify_abort  If Termination of current operation shall be notified.
 *
 * @retval true   Terminated ongoing operation.
 * @retval false  Ongoing operation was not terminated.
 */
static bool current_operation_terminate(nrf_802154_term_t term_lvl,
                                        req_originator_t  req_orig,
                                        bool              notify)
{
    bool result = nrf_802154_core_hooks_terminate(term_lvl, req_orig);

    if (result)
    {
        switch (m_state)
        {
            case RADIO_STATE_SLEEP:
                if (req_orig != REQ_ORIG_RSCH)
                {
                    // Terminate sleep state unless it is requested by Radio Scheduler
                    // during timeslot end.
                    sleep_terminate();
                }

                break;

            case RADIO_STATE_FALLING_ASLEEP:
                falling_asleep_terminate();
                break;

            case RADIO_STATE_RX:
                if (psdu_is_being_received())
                {
                    if (term_lvl >= NRF_802154_TERM_802154)
                    {
                        rx_terminate();

                        if (notify)
                        {
                            nrf_802154_notify_receive_failed(NRF_802154_RX_ERROR_ABORTED);
                        }
                    }
                    else
                    {
                        result = false;
                    }
                }
                else
                {
                    rx_terminate();
                }

                break;

            case RADIO_STATE_TX_ACK:
                if (term_lvl >= NRF_802154_TERM_802154)
                {
                    tx_ack_terminate();

                    if (notify)
                    {
                        mp_current_rx_buffer->free = false;
                        received_frame_notify(mp_current_rx_buffer->data);
                    }
                }
                else
                {
                    result = false;
                }

                break;

            case RADIO_STATE_CCA_TX:
            case RADIO_STATE_TX:
                if (term_lvl >= NRF_802154_TERM_802154)
                {
                    tx_terminate();

                    if (notify)
                    {
                        transmit_failed_notify(NRF_802154_TX_ERROR_ABORTED);
                    }
                }
                else
                {
                    result = false;
                }

                break;

            case RADIO_STATE_RX_ACK:
                if (term_lvl >= NRF_802154_TERM_802154)
                {
                    rx_ack_terminate();

                    if (notify)
                    {
                        transmit_failed_notify(NRF_802154_TX_ERROR_ABORTED);
                    }
                }
                else
                {
                    result = false;
                }

                break;

            case RADIO_STATE_ED:
                if (term_lvl >= NRF_802154_TERM_802154)
                {
                    ed_terminate();

                    if (notify)
                    {
                        nrf_802154_notify_energy_detection_failed(NRF_802154_ED_ERROR_ABORTED);
                    }
                }
                else
                {
                    result = false;
                }

                break;

            case RADIO_STATE_CCA:
                if (term_lvl >= NRF_802154_TERM_802154)
                {
                    cca_terminate();

                    if (notify)
                    {
                        nrf_802154_notify_cca_failed(NRF_802154_CCA_ERROR_ABORTED);
                    }
                }
                else
                {
                    result = false;
                }
                break;

            case RADIO_STATE_CONTINUOUS_CARRIER:
                continuous_carrier_terminate();
                break;

            default:
                assert(false);
        }
    }

    return result;
}

/** Enter Sleep state. */
static void sleep_init(void)
{
    nrf_802154_timer_coord_stop();
    nrf_802154_rsch_crit_sect_prio_request(RSCH_PRIO_IDLE);
}

/** Initialize Falling Asleep operation. */
static void falling_asleep_init(void)
{
    if (!timeslot_is_granted())
    {
        state_set(RADIO_STATE_SLEEP);
        sleep_init();
        return;
    }

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);

    if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_DISABLED)
    {
        // Radio is already disabled. Enter sleep state directly.
        falling_asleep_terminate();
        state_set(RADIO_STATE_SLEEP);
        sleep_init();
    }
}

/** Initialize RX operation. */
static void rx_init(bool disabled_was_triggered)
{
    bool    free_buffer;
    int32_t ints_to_enable = 0;

    if (!timeslot_is_granted())
    {
        return;
    }

    // Clear filtering flag
    rx_flags_clear();
    // Clear the RSSI measurement flag.
    m_flags.rssi_started = false;

    nrf_radio_txpower_set(NRF_RADIO, nrf_802154_pib_tx_power_get());

    // Find available RX buffer
    free_buffer = rx_buffer_is_available();

    if (free_buffer)
    {
        nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
    }

    // Set shorts
    nrf_radio_shorts_set(NRF_RADIO, free_buffer ? (SHORTS_RX | SHORTS_RX_FREE_BUFFER) : (SHORTS_RX));

    // Set BCC
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_bcc_set(NRF_RADIO, BCC_INIT);
#endif // !NRF_802154_DISABLE_BCC_MATCHING

    // Enable IRQs
#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
    ints_to_enable |= NRF_RADIO_INT_CRCERROR_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING ||NRF_802154_NOTIFY_CRCERROR
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);
    ints_to_enable |= NRF_RADIO_INT_BCMATCH_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
    ints_to_enable |= NRF_RADIO_INT_CRCOK_MASK;
    nrf_radio_int_enable(NRF_RADIO, ints_to_enable);

    // Set FEM
    fem_for_rx_set();

#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_timer_shorts_enable(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_SHORT_COMPARE1_STOP_MASK);
    nrf_timer_cc_set(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_CC_CHANNEL1, 1);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_EGU_RAMP_UP,
                                   (uint32_t)nrf_egu_event_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_EVENT),
                                   (uint32_t)nrf_radio_task_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_TASK_RXEN));
    nrf_ppi_channel_and_fork_endpoint_setup(NRF_PPI,
                                            PPI_EGU_TIMER_START,
                                            (uint32_t)nrf_egu_event_address_get(
                                                NRF_802154_SWI_EGU_INSTANCE,
                                                EGU_EVENT),
                                            (uint32_t)nrf_timer_task_address_get(
                                                NRF_802154_TIMER_INSTANCE,
                                                NRF_TIMER_TASK_START),
                                            (uint32_t)nrf_timer_task_address_get(
                                                NRF_802154_COUNTER_TIMER_INSTANCE,
                                                NRF_TIMER_TASK_START));
    // Anomaly 78: use SHUTDOWN instead of CLEAR.
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_CRCERROR_CLEAR,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_CRCERROR),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_SHUTDOWN));
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_CRCOK_DIS_PPI,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_CRCOK),
                                   (uint32_t)nrf_ppi_task_address_get(NRF_PPI, PPI_CHGRP0_DIS_TASK));
#else // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_and_fork_endpoint_setup(NRF_PPI,
                                            PPI_EGU_RAMP_UP,
                                            (uint32_t)nrf_egu_event_address_get(
                                                NRF_802154_SWI_EGU_INSTANCE,
                                                EGU_EVENT),
                                            (uint32_t)nrf_radio_task_address_get(
                                                NRF_RADIO,
                                                NRF_RADIO_TASK_RXEN),
                                            (uint32_t)nrf_ppi_task_address_get(
                                                NRF_PPI,
                                                PPI_CHGRP0_DIS_TASK));
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_EGU_TIMER_START,
                                   (uint32_t)nrf_egu_event_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_EVENT),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_START));
#endif // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_include_in_group(NRF_PPI, PPI_EGU_RAMP_UP, PPI_CHGRP0);

    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_DISABLED_EGU,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_DISABLED),
                                   (uint32_t)nrf_egu_task_address_get(
                                       NRF_802154_SWI_EGU_INSTANCE,
                                       EGU_TASK));
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_ADDRESS_COUNTER_COUNT,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_ADDRESS),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_COUNTER_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_COUNT));
    // Anomaly 78: use SHUTDOWN instead of CLEAR.
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_CRCERROR_COUNTER_CLEAR,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_CRCERROR),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_COUNTER_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_SHUTDOWN));
#endif // NRF_802154_DISABLE_BCC_MATCHING

    nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_RAMP_UP);
    nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_TIMER_START);
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_enable(NRF_PPI, PPI_CRCERROR_CLEAR);
    nrf_ppi_channel_enable(NRF_PPI, PPI_CRCOK_DIS_PPI);
    nrf_ppi_channel_enable(NRF_PPI, PPI_ADDRESS_COUNTER_COUNT);
    nrf_ppi_channel_enable(NRF_PPI, PPI_CRCERROR_COUNTER_CLEAR);
#endif // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_enable(NRF_PPI, PPI_DISABLED_EGU);

    // Configure the timer coordinator to get a timestamp of the CRCOK event.
    nrf_802154_timer_coord_timestamp_prepare(
        (uint32_t)nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_CRCOK));

    // Start procedure if necessary
    if (!disabled_was_triggered || !ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }

    // Find RX buffer if none available
    if (!free_buffer)
    {
        rx_buffer_in_use_set(nrf_802154_rx_buffer_free_find());

        if (rx_buffer_is_available())
        {
            nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
            nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX | SHORTS_RX_FREE_BUFFER);

            if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
            {
                nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
            }
        }
    }
}

/** Initialize TX operation. */
static bool tx_init(const uint8_t * p_data, bool cca, bool disabled_was_triggered)
{
    uint32_t ints_to_enable = 0;

    if (!timeslot_is_granted() || !nrf_802154_rsch_timeslot_request(
            nrf_802154_tx_duration_get(p_data[0], cca, ack_is_requested(p_data))))
    {
        return false;
    }

    nrf_radio_txpower_set(NRF_RADIO, nrf_802154_pib_tx_power_get());
    nrf_radio_packetptr_set(NRF_RADIO, p_data);

    // Set shorts
    nrf_radio_shorts_set(NRF_RADIO, cca ? SHORTS_CCA_TX : SHORTS_TX);

    // Enable IRQs
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
    ints_to_enable |= NRF_RADIO_INT_PHYEND_MASK;

    if (cca)
    {
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CCABUSY);
        ints_to_enable |= NRF_RADIO_INT_CCABUSY_MASK;
    }

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    ints_to_enable    |= NRF_RADIO_INT_ADDRESS_MASK;
    m_flags.tx_started = false;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

    nrf_radio_int_enable(NRF_RADIO, ints_to_enable);

    // Set FEM
    fem_for_tx_set(cca);

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(cca ? NRF_RADIO_TASK_RXEN : NRF_RADIO_TASK_TXEN, true);

    if (!disabled_was_triggered || !ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }

    return true;
}

/** Initialize ED operation */
static void ed_init(bool disabled_was_triggered)
{
    if (!timeslot_is_granted() || !ed_iter_setup(m_ed_time_left))
    {
        // Just wait for next timeslot if there is not enough time in this one.
        return;
    }

    // Set shorts
    nrf_radio_shorts_set(NRF_RADIO, SHORTS_ED);

    // Enable IRQs
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_EDEND);
    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_EDEND_MASK);

    // Set FEM
    fem_for_lna_set();

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(NRF_RADIO_TASK_RXEN, true);

    if (!disabled_was_triggered || !ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }
}

/** Initialize CCA operation. */
static void cca_init(bool disabled_was_triggered)
{
    if (!timeslot_is_granted() || !nrf_802154_rsch_timeslot_request(nrf_802154_cca_duration_get()))
    {
        return;
    }

    // Set shorts
    nrf_radio_shorts_set(NRF_RADIO, SHORTS_CCA);

    // Enable IRQs
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CCABUSY);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CCAIDLE);
    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_CCABUSY_MASK | NRF_RADIO_INT_CCAIDLE_MASK);

    // Set FEM
    fem_for_lna_set();

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(NRF_RADIO_TASK_RXEN, true);

    if (!disabled_was_triggered || !ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }
}

/** Initialize Continuous Carrier operation. */
static void continuous_carrier_init(bool disabled_was_triggered)
{
    if (!timeslot_is_granted())
    {
        return;
    }

    // Set Tx Power
    nrf_radio_txpower_set(NRF_RADIO, nrf_802154_pib_tx_power_get());

    // Set FEM
    fem_for_pa_set();

    // Clr event EGU
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

    // Set PPIs
    ppis_for_egu_and_ramp_up_set(NRF_RADIO_TASK_TXEN, false);

    if (!disabled_was_triggered || !ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }
}

/***************************************************************************************************
 * @section Radio Scheduler notification handlers
 **************************************************************************************************/

static void cont_prec_approved(void)
{
    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_TIMESLOT_STARTED);

    if (remaining_timeslot_time_is_enough_for_crit_sect() && !timeslot_is_granted())
    {
        nrf_radio_reset();
        nrf_radio_init();
        irq_init();

        assert(nrf_radio_shorts_get(NRF_RADIO) == SHORTS_IDLE);

        m_rsch_timeslot_is_granted = true;
        nrf_802154_timer_coord_start();

#if ENABLE_FEM
        mpsl_fem_abort_set(nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_DISABLED), PPI_FEM_ABORT_GROUP);
#endif // ENABLE_FEM
        switch (m_state)
        {
            case RADIO_STATE_SLEEP:
                // Intentionally empty. Appropriate action will be performed on state change.
                break;

            case RADIO_STATE_RX:
                rx_init(false);
                break;

            case RADIO_STATE_CCA_TX:
                (void)tx_init(mp_tx_data, true, false);
                break;

            case RADIO_STATE_TX:
                (void)tx_init(mp_tx_data, false, false);
                break;

            case RADIO_STATE_ED:
                ed_init(false);
                break;

            case RADIO_STATE_CCA:
                cca_init(false);
                break;

            case RADIO_STATE_CONTINUOUS_CARRIER:
                continuous_carrier_init(false);
                break;

            default:
                assert(false);
        }
    }

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_TIMESLOT_STARTED);
}

static void cont_prec_denied(void)
{
    bool result;

    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_TIMESLOT_ENDED);

    if (timeslot_is_granted())
    {
        m_rsch_timeslot_is_granted = false;

        if (nrf_802154_rsch_timeslot_is_requested())
        {
            irq_deinit();
            nrf_radio_reset();
        }

#if ENABLE_FEM
        mpsl_fem_abort_clear();
        mpsl_fem_deactivate_now(MPSL_FEM_ALL);
#endif // ENABLE_FEM
        nrf_802154_timer_coord_stop();

        result = current_operation_terminate(NRF_802154_TERM_802154, REQ_ORIG_RSCH, false);
        assert(result);
        (void)result;

        switch (m_state)
        {
            case RADIO_STATE_FALLING_ASLEEP:
                state_set(RADIO_STATE_SLEEP);
                sleep_init();
                break;

            case RADIO_STATE_RX:
                if (psdu_is_being_received())
                {
                    receive_failed_notify(NRF_802154_RX_ERROR_TIMESLOT_ENDED);
                }

                break;

            case RADIO_STATE_TX_ACK:
                state_set(RADIO_STATE_RX);
                mp_current_rx_buffer->free = false;
                received_frame_notify_and_nesting_allow(mp_current_rx_buffer->data);
                break;

            case RADIO_STATE_CCA_TX:
            case RADIO_STATE_TX:
            case RADIO_STATE_RX_ACK:
                state_set(RADIO_STATE_RX);
                transmit_failed_notify_and_nesting_allow(NRF_802154_TX_ERROR_TIMESLOT_ENDED);
                break;

            case RADIO_STATE_ED:
            case RADIO_STATE_CCA:
            case RADIO_STATE_CONTINUOUS_CARRIER:
            case RADIO_STATE_SLEEP:
                // Intentionally empty.
                break;

            default:
                assert(false);
        }
    }

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_TIMESLOT_ENDED);
}

void nrf_802154_rsch_crit_sect_prio_changed(rsch_prio_t prio)
{
    if (prio > RSCH_PRIO_IDLE)
    {
        cont_prec_approved();
    }
    else
    {
        cont_prec_denied();
        nrf_802154_rsch_continuous_ended();
    }
}

/***************************************************************************************************
 * @section RADIO interrupt handler
 **************************************************************************************************/

static void irq_address_state_tx_frame(void)
{
    transmit_started_notify();

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    m_flags.tx_started = true;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED
}

static void irq_address_state_tx_ack(void)
{
    nrf_802154_tx_ack_started(mp_ack);
}

static void irq_address_state_rx_ack(void)
{
    nrf_802154_core_hooks_rx_ack_started();
}

#if !NRF_802154_DISABLE_BCC_MATCHING
// This event is generated during frame reception to request Radio Scheduler timeslot
// and to filter frame
static void irq_bcmatch_state_rx(void)
{
    uint8_t               prev_num_data_bytes;
    uint8_t               num_data_bytes;
    nrf_802154_rx_error_t filter_result;
    bool                  frame_accepted = true;

    num_data_bytes      = nrf_radio_bcc_get(NRF_RADIO) / 8;
    prev_num_data_bytes = num_data_bytes;

    assert(num_data_bytes >= PHR_SIZE + FCF_SIZE);

    // If CRCERROR event is set, it means that events are handled out of order due to software
    // latency. Just skip this handler in this case - frame will be dropped.
    if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR))
    {
        return;
    }

    if (!m_flags.frame_filtered)
    {
        m_flags.psdu_being_received = true;
        filter_result               = nrf_802154_filter_frame_part(mp_current_rx_buffer->data,
                                                                   &num_data_bytes);

        if (filter_result == NRF_802154_RX_ERROR_NONE)
        {
            if (num_data_bytes != prev_num_data_bytes)
            {
                nrf_radio_bcc_set(NRF_RADIO, num_data_bytes * 8);
            }
            else
            {
                m_flags.frame_filtered = true;
            }
        }
        else if ((filter_result == NRF_802154_RX_ERROR_INVALID_LENGTH) ||
                 (!nrf_802154_pib_promiscuous_get()))
        {
            rx_terminate();
            rx_init(true);

            frame_accepted = false;

            if ((mp_current_rx_buffer->data[FRAME_TYPE_OFFSET] & FRAME_TYPE_MASK) !=
                FRAME_TYPE_ACK)
            {
                receive_failed_notify(filter_result);
            }
        }
        else
        {
            // Promiscuous mode, allow incorrect frames. Nothing to do here.
        }
    }

    if ((!m_flags.rx_timeslot_requested) && (frame_accepted))
    {
        if (nrf_802154_rsch_timeslot_request(nrf_802154_rx_duration_get(
                                                 mp_current_rx_buffer->data[0],
                                                 ack_is_requested(mp_current_rx_buffer->data))))
        {
            m_flags.rx_timeslot_requested = true;

            receive_started_notify();
        }
        else
        {
            // Disable receiver and wait for a new timeslot.
            rx_terminate();

            nrf_802154_notify_receive_failed(NRF_802154_RX_ERROR_TIMESLOT_ENDED);
        }
    }
}

#endif // !NRF_802154_DISABLE_BCC_MATCHING

#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
static void irq_crcerror_state_rx(void)
{
#if !NRF_802154_DISABLE_BCC_MATCHING
    rx_restart(false);
#endif // !NRF_802154_DISABLE_BCC_MATCHING
#if NRF_802154_NOTIFY_CRCERROR
    receive_failed_notify(NRF_802154_RX_ERROR_INVALID_FCS);
#endif // NRF_802154_NOTIFY_CRCERROR
}

#endif // !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR

static void irq_crcok_state_rx(void)
{
    uint8_t * p_received_data = mp_current_rx_buffer->data;
    uint32_t  ints_to_disable = 0;
    uint32_t  ints_to_enable  = 0;

    m_flags.rssi_started = true;

#if NRF_802154_DISABLE_BCC_MATCHING
    uint8_t               num_data_bytes      = PHR_SIZE + FCF_SIZE;
    uint8_t               prev_num_data_bytes = 0;
    nrf_802154_rx_error_t filter_result;

    // Frame filtering
    while (num_data_bytes != prev_num_data_bytes)
    {
        prev_num_data_bytes = num_data_bytes;

        // Keep checking consecutive parts of the frame header.
        filter_result = nrf_802154_filter_frame_part(mp_current_rx_buffer->data, &num_data_bytes);

        if (filter_result == NRF_802154_RX_ERROR_NONE)
        {
            if (num_data_bytes == prev_num_data_bytes)
            {
                m_flags.frame_filtered = true;
            }
        }
        else
        {
            break;
        }
    }

    // Timeslot request
    if (m_flags.frame_filtered &&
        ack_is_requested(p_received_data) &&
        !nrf_802154_rsch_timeslot_request(nrf_802154_rx_duration_get(0, true)))
    {
        // Frame is destined to this node but there is no timeslot to transmit ACK.
        // Just disable receiver and wait for a new timeslot.
        rx_terminate();

        rx_flags_clear();

        // Filter out received ACK frame if promiscuous mode is disabled.
        if (((p_received_data[FRAME_TYPE_OFFSET] & FRAME_TYPE_MASK) != FRAME_TYPE_ACK) ||
            nrf_802154_pib_promiscuous_get())
        {
            mp_current_rx_buffer->free = false;
            received_frame_notify_and_nesting_allow(p_received_data);
        }

        return;
    }
#endif // NRF_802154_DISABLE_BCC_MATCHING

    if (m_flags.frame_filtered || nrf_802154_pib_promiscuous_get())
    {
        bool send_ack = false;

        if (m_flags.frame_filtered &&
            ack_is_requested(mp_current_rx_buffer->data) &&
            nrf_802154_pib_auto_ack_get())
        {
            mp_ack = nrf_802154_ack_generator_create(mp_current_rx_buffer->data);
            if (NULL != mp_ack)
            {
                send_ack = true;
            }
        }

        if (send_ack)
        {
            bool wait_for_phyend;

            nrf_radio_packetptr_set(NRF_RADIO, mp_ack);

            // Set shorts
            nrf_radio_shorts_set(NRF_RADIO, SHORTS_TX_ACK);

            // Clear TXREADY event to detect if PPI worked
            nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_TXREADY);

#if NRF_802154_DISABLE_BCC_MATCHING
            // Disable PPIs for PSDU detection
            nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_TIMER_START, 0);
            nrf_ppi_channel_disable(NRF_PPI, PPI_ADDRESS_COUNTER_COUNT);
            nrf_ppi_channel_disable(NRF_PPI, PPI_CRCERROR_COUNTER_CLEAR);
#endif // NRF_802154_DISABLE_BCC_MATCHING

            // Set PPIs
            nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                           PPI_TIMER_TX_ACK,
                                           (uint32_t)nrf_timer_event_address_get(
                                               NRF_802154_TIMER_INSTANCE,
                                               NRF_TIMER_EVENT_COMPARE1),
                                           (uint32_t)nrf_radio_task_address_get(
                                               NRF_RADIO,
                                               NRF_RADIO_TASK_TXEN));

#if !NRF_802154_DISABLE_BCC_MATCHING
            nrf_ppi_channel_enable(NRF_PPI, PPI_TIMER_TX_ACK);
#endif // !NRF_802154_DISABLE_BCC_MATCHING

            // Set FEM PPIs
            uint32_t time_to_rampup = nrf_timer_cc_get(NRF_802154_TIMER_INSTANCE,
                                                       NRF_TIMER_CC_CHANNEL1);

#if ENABLE_FEM
            mpsl_fem_event_t timer = m_activate_tx_cc0;

            timer.event.timer.counter_period.end += time_to_rampup;

            mpsl_fem_pa_configuration_set(&timer, NULL);
            uint32_t time_to_fem = nrf_timer_cc_get(NRF_802154_TIMER_INSTANCE,
                                                    NRF_TIMER_CC_CHANNEL0);
#else //ENABLE_FEM
            uint32_t time_to_fem = time_to_rampup;
#endif // ENABLE_FEM

            // Detect if PPI worked (timer is counting or TIMER event is marked)
            nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_CAPTURE3);
            uint32_t current_timer_value = nrf_timer_cc_get(NRF_802154_TIMER_INSTANCE,
                                                            NRF_TIMER_CC_CHANNEL3);

            // When external PA uses timer, it should be configured to a time later than ramp up
            // time. In such case, the timer stops with shorts on PA timer.
            // But if external PA does not use timer, FEM time is set to a value in the pased
            // used by LNA. After timer overflow, the timer stops with short on the past value
            // used by LNA. We have to detect if the timer is after the overflow.
            if ((current_timer_value < time_to_rampup) &&
                ((time_to_fem >= time_to_rampup) || (current_timer_value > time_to_fem)))
            {
                wait_for_phyend = true;
            }
            else
            {
                ppi_and_egu_delay_wait();

                if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_TXRU)
                {
                    wait_for_phyend = true;
                }
                else if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_TXREADY))
                {
                    wait_for_phyend = true;
                }
                else
                {
                    wait_for_phyend = false;
                }
            }

            if (wait_for_phyend)
            {
                state_set(RADIO_STATE_TX_ACK);

                // Set event handlers
#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
                ints_to_disable |= NRF_RADIO_INT_CRCERROR_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
#if !NRF_802154_DISABLE_BCC_MATCHING
                ints_to_disable |= NRF_RADIO_INT_BCMATCH_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
                ints_to_disable |= NRF_RADIO_INT_CRCOK_MASK;
                nrf_radio_int_disable(NRF_RADIO, ints_to_disable);

                nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
                ints_to_enable = NRF_RADIO_INT_PHYEND_MASK;

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
                nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
                ints_to_enable |= NRF_RADIO_INT_ADDRESS_MASK;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

                nrf_radio_int_enable(NRF_RADIO, ints_to_enable);
            }
            else
            {
                mp_current_rx_buffer->free = false;

#if !NRF_802154_DISABLE_BCC_MATCHING
                nrf_ppi_channel_disable(NRF_PPI, PPI_TIMER_TX_ACK);
                nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_TIMER_TX_ACK, 0, 0);
                nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_TIMER_TX_ACK, 0);
#endif // !NRF_802154_DISABLE_BCC_MATCHING

                // RX uses the same peripherals as TX_ACK until RADIO ints are updated.
                rx_terminate();
                rx_init(true);

                received_frame_notify_and_nesting_allow(p_received_data);
            }
        }
        else
        {
            rx_restart(true);

            // Filter out received ACK frame if promiscuous mode is disabled.
            if (((p_received_data[FRAME_TYPE_OFFSET] & FRAME_TYPE_MASK) != FRAME_TYPE_ACK) ||
                nrf_802154_pib_promiscuous_get())
            {
                // Find new RX buffer
                mp_current_rx_buffer->free = false;
                rx_buffer_in_use_set(nrf_802154_rx_buffer_free_find());

                if (rx_buffer_is_available())
                {
                    nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
                    nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX | SHORTS_RX_FREE_BUFFER);

                    if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
                    {
                        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
                    }
                }

                received_frame_notify_and_nesting_allow(p_received_data);
            }
            else
            {
                nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX | SHORTS_RX_FREE_BUFFER);

                if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
                {
                    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
                }
            }
        }
    }
    else
    {
        // CRC is OK, but filtering operation did not end - it is invalid frame with valid CRC
        // or problem due to software latency (i.e. handled BCMATCH, CRCERROR, CRCOK from two
        // consecutively received frames).
        rx_terminate();
        rx_init(true);

#if NRF_802154_DISABLE_BCC_MATCHING
        if ((p_received_data[FRAME_TYPE_OFFSET] & FRAME_TYPE_MASK) != FRAME_TYPE_ACK)
        {
            receive_failed_notify(filter_result);
        }
#else // NRF_802154_DISABLE_BCC_MATCHING
        receive_failed_notify(NRF_802154_RX_ERROR_RUNTIME);
#endif  // NRF_802154_DISABLE_BCC_MATCHING
    }
}

static void irq_phyend_state_tx_ack(void)
{
    uint8_t * p_received_data = mp_current_rx_buffer->data;
    uint32_t  ints_to_enable  = 0;
    uint32_t  ints_to_disable = 0;

    // Disable PPIs on DISABLED event to control TIMER.
    nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);

    // Set FEM PPIs
#if ENABLE_FEM
    mpsl_fem_pa_configuration_clear();
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);
    mpsl_fem_lna_configuration_set(&m_activate_rx_cc0, NULL);
#endif // ENABLE_FEM

    nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX);

    // Set BCC for next reception
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_bcc_set(NRF_RADIO, BCC_INIT);
#endif // !NRF_802154_DISABLE_BCC_MATCHING

    ints_to_disable = NRF_RADIO_INT_PHYEND_MASK;

#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
    ints_to_disable |= NRF_RADIO_INT_ADDRESS_MASK;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

    nrf_radio_int_disable(NRF_RADIO, ints_to_disable);

#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
    ints_to_enable |= NRF_RADIO_INT_CRCERROR_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING ||NRF_802154_NOTIFY_CRCERROR
#if !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);
    ints_to_enable |= NRF_RADIO_INT_BCMATCH_MASK;
#endif // !NRF_802154_DISABLE_BCC_MATCHING
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
    ints_to_enable |= NRF_RADIO_INT_CRCOK_MASK;
    nrf_radio_int_enable(NRF_RADIO, ints_to_enable);

    // Restart TIMER.
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

#if NRF_802154_DISABLE_BCC_MATCHING
    // Anomaly 78: use SHUTDOWN instead of STOP and CLEAR.
    nrf_timer_task_trigger(NRF_802154_COUNTER_TIMER_INSTANCE, NRF_TIMER_TASK_SHUTDOWN);

    // Reset PPI for RX mode
#if PPI_TIMER_TX_ACK != PPI_CRCERROR_CLEAR
#error Invalid PPI configuration
#endif
    // Anomaly 78: use SHUTDOWN instead of CLEAR.
    nrf_ppi_channel_endpoint_setup(NRF_PPI,
                                   PPI_CRCERROR_CLEAR,
                                   (uint32_t)nrf_radio_event_address_get(
                                       NRF_RADIO,
                                       NRF_RADIO_EVENT_CRCERROR),
                                   (uint32_t)nrf_timer_task_address_get(
                                       NRF_802154_TIMER_INSTANCE,
                                       NRF_TIMER_TASK_SHUTDOWN));

    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_EGU_TIMER_START,
                                (uint32_t)nrf_timer_task_address_get(
                                    NRF_802154_COUNTER_TIMER_INSTANCE,
                                    NRF_TIMER_TASK_START));
#else // NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_disable(NRF_PPI, PPI_TIMER_TX_ACK);
    nrf_ppi_channel_endpoint_setup(NRF_PPI, PPI_TIMER_TX_ACK, 0, 0);
    nrf_ppi_fork_endpoint_setup(NRF_PPI, PPI_TIMER_TX_ACK, 0);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    // Enable PPI disabled by CRCOK
    nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_RAMP_UP);
#if NRF_802154_DISABLE_BCC_MATCHING
    nrf_ppi_channel_enable(NRF_PPI, PPI_ADDRESS_COUNTER_COUNT);
    nrf_ppi_channel_enable(NRF_PPI, PPI_CRCERROR_COUNTER_CLEAR);
#endif // NRF_802154_DISABLE_BCC_MATCHING

    // Enable PPIs on DISABLED event and clear event to detect if PPI worked
    nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);
    nrf_ppi_channel_enable(NRF_PPI, PPI_DISABLED_EGU);

    // Prepare the timer coordinator to get a precise timestamp of the CRCOK event.
    nrf_802154_timer_coord_timestamp_prepare(
        (uint32_t)nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_CRCOK));

    if (!ppi_egu_worked())
    {
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    }

    // Find new RX buffer
    mp_current_rx_buffer->free = false;
    rx_buffer_in_use_set(nrf_802154_rx_buffer_free_find());

    if (rx_buffer_is_available())
    {
        nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
        nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX | SHORTS_RX_FREE_BUFFER);

        if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
        {
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
        }
    }

    state_set(RADIO_STATE_RX);

    rx_flags_clear();

    received_frame_notify_and_nesting_allow(p_received_data);
}

static void irq_phyend_state_tx_frame(void)
{
    uint32_t ints_to_disable = 0;
    uint32_t ints_to_enable  = 0;

    // Ignore PHYEND event if transmission has not started. This event may be triggered by
    // previously terminated transmission.
    if (!transmission_has_started())
    {
        return;
    }

    if (ack_is_requested(mp_tx_data))
    {
        bool     rx_buffer_free = rx_buffer_is_available();
        uint32_t shorts         = rx_buffer_free ?
                                  (SHORTS_RX_ACK | SHORTS_RX_FREE_BUFFER) : SHORTS_RX_ACK;

        // Disable EGU PPI to prevent unsynchronized PPIs
        nrf_ppi_channel_disable(NRF_PPI, PPI_DISABLED_EGU);

        nrf_radio_shorts_set(NRF_RADIO, shorts);

        if (rx_buffer_free)
        {
            nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
        }

        ints_to_disable = NRF_RADIO_INT_CCABUSY_MASK;
#if NRF_802154_TX_STARTED_NOTIFY_ENABLED
        ints_to_disable |= NRF_RADIO_INT_ADDRESS_MASK;
#endif // NRF_802154_TX_STARTED_NOTIFY_ENABLED

        ints_to_disable |= NRF_RADIO_INT_PHYEND_MASK;
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
        ints_to_enable |= NRF_RADIO_INT_END_MASK;

        nrf_radio_int_disable(NRF_RADIO, ints_to_disable);

        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
        ints_to_enable |= NRF_RADIO_INT_ADDRESS_MASK;

        nrf_radio_int_enable(NRF_RADIO, ints_to_enable);

        // Clear FEM configuration set at the beginning of the transmission
        fem_for_tx_reset(false);
        // Set PPIs necessary in rx_ack state
        fem_for_lna_set();

        nrf_ppi_channel_and_fork_endpoint_setup(NRF_PPI,
                                                PPI_EGU_RAMP_UP,
                                                (uint32_t)nrf_egu_event_address_get(
                                                    NRF_802154_SWI_EGU_INSTANCE,
                                                    EGU_EVENT),
                                                (uint32_t)nrf_radio_task_address_get(
                                                    NRF_RADIO,
                                                    NRF_RADIO_TASK_RXEN),
                                                (uint32_t)nrf_ppi_task_address_get(
                                                    NRF_PPI,
                                                    PPI_CHGRP0_DIS_TASK));

        nrf_egu_event_clear(NRF_802154_SWI_EGU_INSTANCE, EGU_EVENT);

        // Enable PPI disabled by DISABLED event
        nrf_ppi_channel_enable(NRF_PPI, PPI_EGU_RAMP_UP);

        // Enable EGU PPI to start all PPIs synchronously
        nrf_ppi_channel_enable(NRF_PPI, PPI_DISABLED_EGU);

        state_set(RADIO_STATE_RX_ACK);

        if (!ppi_egu_worked())
        {
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        }

        if (!rx_buffer_free)
        {
            rx_buffer_in_use_set(nrf_802154_rx_buffer_free_find());

            if (rx_buffer_is_available())
            {
                nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
                nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX_ACK | SHORTS_RX_FREE_BUFFER);

                if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
                {
                    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
                }
            }
        }

        if ((mp_tx_data[FRAME_VERSION_OFFSET] & FRAME_VERSION_MASK) != FRAME_VERSION_2)
        {
            ack_matching_enable();
        }
    }
    else
    {
        tx_terminate();
        state_set(RADIO_STATE_RX);
        rx_init(true);

        transmitted_frame_notify(NULL, 0, 0);
    }
}

static void irq_end_state_rx_ack(void)
{
    bool          ack_match    = ack_is_matched();
    rx_buffer_t * p_ack_buffer = NULL;
    uint8_t     * p_ack_data   = mp_current_rx_buffer->data;

    if (!ack_match &&
        ((mp_tx_data[FRAME_VERSION_OFFSET] & FRAME_VERSION_MASK) == FRAME_VERSION_2) &&
        ((p_ack_data[FRAME_VERSION_OFFSET] & FRAME_VERSION_MASK) == FRAME_VERSION_2) &&
        ((p_ack_data[FRAME_TYPE_OFFSET] & FRAME_TYPE_MASK) == FRAME_TYPE_ACK) &&
        (nrf_radio_crc_status_check(NRF_RADIO)))
    {
        // For frame version 2 sequence number bit may be suppressed and its check fails.
        // Verify ACK frame using its destination address.
        nrf_802154_frame_parser_mhr_data_t tx_mhr_data;
        nrf_802154_frame_parser_mhr_data_t ack_mhr_data;
        bool                               parse_result;

        parse_result = nrf_802154_frame_parser_mhr_parse(mp_tx_data, &tx_mhr_data);
        assert(parse_result);
        parse_result = nrf_802154_frame_parser_mhr_parse(p_ack_data, &ack_mhr_data);

        if (parse_result &&
            (tx_mhr_data.p_src_addr != NULL) &&
            (ack_mhr_data.p_dst_addr != NULL) &&
            (tx_mhr_data.src_addr_size == ack_mhr_data.dst_addr_size) &&
            (0 == memcmp(tx_mhr_data.p_src_addr,
                         ack_mhr_data.p_dst_addr,
                         tx_mhr_data.src_addr_size)))
        {
            ack_match = true;
        }
    }

    if (ack_match)
    {
        p_ack_buffer               = mp_current_rx_buffer;
        mp_current_rx_buffer->free = false;
    }

    rx_ack_terminate();
    state_set(RADIO_STATE_RX);
    rx_init(true);

    if (ack_match)
    {
        transmitted_frame_notify(p_ack_buffer->data,           // phr + psdu
                                 rssi_last_measurement_get(),  // rssi
                                 lqi_get(p_ack_buffer->data)); // lqi;
    }
    else
    {
        transmit_failed_notify_and_nesting_allow(NRF_802154_TX_ERROR_INVALID_ACK);
    }
}

static void irq_disabled_state_falling_asleep(void)
{
    falling_asleep_terminate();
    state_set(RADIO_STATE_SLEEP);
    sleep_init();
}

/// This event is generated when CCA reports idle channel during stand-alone procedure.
static void irq_ccaidle_state_cca(void)
{
    cca_terminate();
    state_set(RADIO_STATE_RX);
    rx_init(true);

    cca_notify(true);
}

static void irq_ccabusy_state_tx_frame(void)
{
    tx_terminate();
    state_set(RADIO_STATE_RX);
    rx_init(true);

    transmit_failed_notify_and_nesting_allow(NRF_802154_TX_ERROR_BUSY_CHANNEL);
}

static void irq_ccabusy_state_cca(void)
{
    cca_terminate();
    state_set(RADIO_STATE_RX);
    rx_init(true);

    cca_notify(false);
}

/// This event is generated when energy detection procedure ends.
static void irq_edend_state_ed(void)
{
    uint32_t result = nrf_radio_ed_sample_get(NRF_RADIO);

    m_ed_result = result > m_ed_result ? result : m_ed_result;

    if (m_ed_time_left)
    {
        if (ed_iter_setup(m_ed_time_left))
        {
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_EDSTART);
        }
        else
        {
            fem_for_lna_reset();
        }
    }
    else
    {
        // In case channel change was requested during energy detection procedure.
        channel_set(nrf_802154_pib_channel_get());

        ed_terminate();
        state_set(RADIO_STATE_RX);
        rx_init(true);

        energy_detected_notify(ed_result_get());
    }
}

/// Handler of radio interrupts.
static void irq_handler(void)
{
    nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_IRQ_HANDLER);

    // Prevent interrupting of this handler by requests from higher priority code.
    nrf_802154_critical_section_forcefully_enter();

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_ADDRESS_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_FRAMESTART);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

        switch (m_state)
        {
            case RADIO_STATE_CCA_TX:
            case RADIO_STATE_TX:
                irq_address_state_tx_frame();
                break;

            case RADIO_STATE_TX_ACK:
                irq_address_state_tx_ack();
                break;

            case RADIO_STATE_RX_ACK:
                irq_address_state_rx_ack();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_FRAMESTART);
    }

#if !NRF_802154_DISABLE_BCC_MATCHING
    // Check MAC frame header.
    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_BCMATCH_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_BCMATCH);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_BCMATCH);

        switch (m_state)
        {
            case RADIO_STATE_RX:
                irq_bcmatch_state_rx();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_BCMATCH);
    }

#endif // !NRF_802154_DISABLE_BCC_MATCHING

#if !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR
    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_CRCERROR_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CRCERROR);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);

        switch (m_state)
        {
            case RADIO_STATE_RX:
                irq_crcerror_state_rx();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CRCERROR);
    }
#endif // !NRF_802154_DISABLE_BCC_MATCHING || NRF_802154_NOTIFY_CRCERROR

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_CRCOK_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCOK))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CRCOK);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);

        switch (m_state)
        {
            case RADIO_STATE_RX:
                irq_crcok_state_rx();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CRCOK);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_PHYEND_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_PHYEND);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

        switch (m_state)
        {
            case RADIO_STATE_TX_ACK:
                irq_phyend_state_tx_ack();
                break;

            case RADIO_STATE_CCA_TX:
            case RADIO_STATE_TX:
                irq_phyend_state_tx_frame();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_PHYEND);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_END_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_END);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

        switch (m_state)
        {
            case RADIO_STATE_RX_ACK: // Ended receiving of ACK.
                irq_end_state_rx_ack();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_END);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_DISABLED);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

        switch (m_state)
        {
            case RADIO_STATE_FALLING_ASLEEP:
                irq_disabled_state_falling_asleep();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_DISABLED);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_CCAIDLE_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CCAIDLE))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CCAIDLE);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CCAIDLE);

        switch (m_state)
        {
            case RADIO_STATE_CCA:
                irq_ccaidle_state_cca();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CCAIDLE);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_CCABUSY_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CCABUSY))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_CCABUSY);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CCABUSY);

        switch (m_state)
        {
            case RADIO_STATE_CCA_TX:
            case RADIO_STATE_TX:
                irq_ccabusy_state_tx_frame();
                break;

            case RADIO_STATE_CCA:
                irq_ccabusy_state_cca();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_CCABUSY);
    }

    if (nrf_radio_int_enable_check(NRF_RADIO, NRF_RADIO_INT_EDEND_MASK) &&
        nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_EDEND))
    {
        nrf_802154_log(EVENT_TRACE_ENTER, FUNCTION_EVENT_EDEND);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_EDEND);

        switch (m_state)
        {
            case RADIO_STATE_ED:
                irq_edend_state_ed();
                break;

            default:
                assert(false);
        }

        nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_EVENT_EDEND);
    }

    nrf_802154_critical_section_exit();

    nrf_802154_log(EVENT_TRACE_EXIT, FUNCTION_IRQ_HANDLER);
}

/***************************************************************************************************
 * @section API functions
 **************************************************************************************************/

void nrf_802154_core_init(void)
{
    m_state                    = RADIO_STATE_SLEEP;
    m_rsch_timeslot_is_granted = false;

    nrf_timer_init();
    nrf_802154_ack_generator_init();
}

void nrf_802154_core_deinit(void)
{
    if (timeslot_is_granted())
    {
        nrf_radio_reset();

        fem_deactivate_now();
    }

    fem_cleanup();

    irq_deinit();
}

radio_state_t nrf_802154_core_state_get(void)
{
    return m_state;
}

bool nrf_802154_core_sleep(nrf_802154_term_t term_lvl)
{
    bool result = nrf_802154_critical_section_enter();

    if (result)
    {
        if ((m_state != RADIO_STATE_SLEEP) && (m_state != RADIO_STATE_FALLING_ASLEEP))
        {
            result = current_operation_terminate(term_lvl, REQ_ORIG_CORE, true);

            if (result)
            {
                state_set(RADIO_STATE_FALLING_ASLEEP);
                falling_asleep_init();
            }
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_receive(nrf_802154_term_t              term_lvl,
                             req_originator_t               req_orig,
                             nrf_802154_notification_func_t notify_function,
                             bool                           notify_abort)
{
    bool result = nrf_802154_critical_section_enter();

    if (result)
    {
        if ((m_state != RADIO_STATE_RX) && (m_state != RADIO_STATE_TX_ACK))
        {
            if (critical_section_can_be_processed_now())
            {
                result = current_operation_terminate(term_lvl, req_orig, notify_abort);

                if (result)
                {
                    state_set(RADIO_STATE_RX);
                    rx_init(true);
                }
            }
            else
            {
                result = false;
            }
        }

        if (notify_function != NULL)
        {
            notify_function(result);
        }

        nrf_802154_critical_section_exit();
    }
    else
    {
        if (notify_function != NULL)
        {
            notify_function(false);
        }
    }

    return result;
}

bool nrf_802154_core_transmit(nrf_802154_term_t              term_lvl,
                              req_originator_t               req_orig,
                              const uint8_t                * p_data,
                              bool                           cca,
                              bool                           immediate,
                              nrf_802154_notification_func_t notify_function)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        result = current_operation_terminate(term_lvl, req_orig, true);

        if (result)
        {
            // Set state to RX in case sleep terminate succeeded, but transmit_begin fails.
            state_set(RADIO_STATE_RX);

            mp_tx_data = p_data;
            result     = tx_init(p_data, cca, true);

            if (!immediate)
            {
                result = true;
            }
        }

        if (result)
        {
            state_set(cca ? RADIO_STATE_CCA_TX : RADIO_STATE_TX);
        }

        if (notify_function != NULL)
        {
            notify_function(result);
        }

        nrf_802154_critical_section_exit();
    }
    else
    {
        if (notify_function != NULL)
        {
            notify_function(false);
        }
    }

    return result;
}

bool nrf_802154_core_energy_detection(nrf_802154_term_t term_lvl, uint32_t time_us)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        result = current_operation_terminate(term_lvl, REQ_ORIG_CORE, true);

        if (result)
        {
            state_set(RADIO_STATE_ED);
            m_ed_time_left = time_us;
            m_ed_result    = 0;
            ed_init(true);
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_cca(nrf_802154_term_t term_lvl)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        result = current_operation_terminate(term_lvl, REQ_ORIG_CORE, true);

        if (result)
        {
            state_set(RADIO_STATE_CCA);
            cca_init(true);
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_continuous_carrier(nrf_802154_term_t term_lvl)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        result = current_operation_terminate(term_lvl, REQ_ORIG_CORE, true);

        if (result)
        {
            state_set(RADIO_STATE_CONTINUOUS_CARRIER);
            continuous_carrier_init(true);
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_notify_buffer_free(uint8_t * p_data)
{
    rx_buffer_t * p_buffer     = (rx_buffer_t *)p_data;
    bool          in_crit_sect = critical_section_enter_and_verify_timeslot_length();

    p_buffer->free = true;

    if (in_crit_sect)
    {
        if (timeslot_is_granted())
        {
            switch (m_state)
            {
                case RADIO_STATE_RX:
                    if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
                    {
                        assert(nrf_radio_shorts_get(NRF_RADIO) == SHORTS_RX);

                        rx_buffer_in_use_set(p_buffer);

                        nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
                        nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX | SHORTS_RX_FREE_BUFFER);

                        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
                    }

                    break;

                case RADIO_STATE_RX_ACK:
                    if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE)
                    {
                        assert(nrf_radio_shorts_get(NRF_RADIO) == SHORTS_RX_ACK);

                        rx_buffer_in_use_set(p_buffer);

                        nrf_radio_packetptr_set(NRF_RADIO, rx_buffer_get());
                        nrf_radio_shorts_set(NRF_RADIO, SHORTS_RX_ACK | SHORTS_RX_FREE_BUFFER);

                        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
                    }

                    break;

                default:
                    // Don't perform any action in any other state (receiver should not be started).
                    break;
            }
        }

        nrf_802154_critical_section_exit();
    }

    return true;
}

bool nrf_802154_core_channel_update(void)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        if (timeslot_is_granted())
        {
            channel_set(nrf_802154_pib_channel_get());
        }

        switch (m_state)
        {
            case RADIO_STATE_RX:
                if (current_operation_terminate(NRF_802154_TERM_NONE, REQ_ORIG_CORE, true))
                {
                    rx_init(true);
                }

                break;

            case RADIO_STATE_CONTINUOUS_CARRIER:
                if (timeslot_is_granted())
                {
                    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
                }

                break;

            default:
                // Don't perform any additional action in any other state.
                break;
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_cca_cfg_update(void)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        if (timeslot_is_granted())
        {
            cca_configuration_update();
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_rssi_measure(void)
{
    bool result = critical_section_enter_and_verify_timeslot_length();

    if (result)
    {
        if (timeslot_is_granted() && (m_state == RADIO_STATE_RX))
        {
            rssi_measure();
        }
        else
        {
            result = false;
        }

        nrf_802154_critical_section_exit();
    }

    return result;
}

bool nrf_802154_core_last_rssi_measurement_get(int8_t * p_rssi)
{
    bool result       = false;
    bool rssi_started = m_flags.rssi_started;
    bool in_crit_sect = false;

    if (rssi_started)
    {
        in_crit_sect = critical_section_enter_and_verify_timeslot_length();
    }

    if (rssi_started && in_crit_sect)
    {
        // Checking if a timeslot is granted is valid only in a critical section
        if (timeslot_is_granted())
        {
            rssi_measurement_wait();
            *p_rssi = rssi_last_measurement_get();
            result  = true;
        }
    }

    if (in_crit_sect)
    {
        nrf_802154_critical_section_exit();
    }

    return result;
}

#if NRF_802154_INTERNAL_RADIO_IRQ_HANDLING
void RADIO_IRQHandler(void)
#else // NRF_802154_INTERNAL_RADIO_IRQ_HANDLING
void nrf_802154_core_irq_handler(void)
#endif  // NRF_802154_INTERNAL_RADIO_IRQ_HANDLING
{
    irq_handler();
}

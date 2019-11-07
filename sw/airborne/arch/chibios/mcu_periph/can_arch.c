/*
 * Copyright (C) 2019 Gautier Hattenberger <gautier.hattenberger@enac.fr>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

/**
 * @file arch/chibios/mcu_periph/can_arch.h
 * @ingroup chibios_arch
 *
 * Handling of CAN hardware for STM32 with ChibiOS.
 */

#include "mcu_periph/can_arch.h"
#include <ch.h>
#include <hal.h>
#include BOARD_CONFIG
#include <string.h>

// Default stack size
#ifndef CAN_THREAD_STACK_SIZE
#define CAN_THREAD_STACK_SIZE 2048 // TODO check
#endif

struct CanInit {
  CANDriver *dev;
  CANConfig *cfg;
  event_source_t *tx_request;
  mutex_t *mutex;
  CANRxFrame rx_msg;
  CANTxFrame tx_msg;
};

void _can_run_rx_callback(uint32_t id, uint8_t *buf, uint8_t len); // FIXME change generic API

/**
 * RX handler
 */
static handle_can_rx(struct CanInit *conf, CANRxFrame *rx_msg) {
  chMtxLock(conf->mutex);
  while (canReceive(conf->dev, CAN_ANY_MAILBOX, rx_msg, TIME_IMMEDIATE) == MSG_OK) {
    // Process message.
    uint8_t len = rx_msg->DLC;
    uint8_t buf[len];
    memcpy(buf, rx_msg->data8, len);
    uint32_t id;
    if (rx_msg->IDE) {
      id = rx_msg->EID;
    } else {
      id = rx_msg->SID;
    }
    _can_run_rx_callback(id, buf, len); // TODO check if we run callback here ?
  }
  chMtxUnlock(conf->mutex);
}

static THD_FUNCTION(can_rx, p) {
  struct CanInit *cfg = (struct CanInit *)p;
  event_listener_t el;

  chRegSetThreadName("can_rx");
  chEvtRegister(&cfg->dev->rxfull_event, &el, EVENT_MASK(0));

  while (true) {
    if (chEvtWaitAnyTimeout(ALL_EVENTS, TIME_MS2I(100)) == 0)
      continue;
    handle_can_rx(cfg, &cfg->rx_msg);
  }
  chEvtUnregister(&cfg->dev->rxfull_event, &el);
}

/*
 * TX handler
 */
static handle_can_tx(struct CanInit *conf, CANTxFrame *tx_msg) {
  chMtxLock(conf->mutex);
  if (canTransmit(conf->dev, CAN_ANY_MAILBOX, tx_msg, TIME_IMMEDIATE) != MSG_OK) {
    // handle error here ?
  }
  chMtxUnlock(conf->mutex);
}

static THD_FUNCTION(can_tx, p) {
  struct CanInit *cfg = (struct CanInit *)p;
  event_listener_t txc, txe, txr;

  chRegSetThreadName("can_tx");
  chEvtRegister(&cfg->dev->txempty_event, &txc, EVENT_MASK(0));
  chEvtRegister(&cfg->dev->error_event, &txe, EVENT_MASK(1));
  chEvtRegister(cfg->tx_request, &txr, EVENT_MASK(2));

  while (true) {
    eventmask_t evts = chEvtWaitAnyTimeout(ALL_EVENTS, TIME_MS2I(100));
    if (evts == 0) {
      continue;
    }
    if (evts & EVENT_MASK(1)) {
      chEvtGetAndClearFlags(&txe);
      continue;
    }
    handle_can_tx(cfg, &cfg->tx_msg);
  }
}

#if USE_CAN1

static CANConfig can1_cfg = {
  CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_TXFP,
  CAN_BTR_SJW(0) | CAN_BTR_TS2(1) |
  CAN_BTR_TS1(14) | CAN_BTR_BRP((STM32_PCLK1/18)/125000 - 1)
}; // TODO check
static MUTEX_DECL(can1_mtx);
static EVENTSOURCE_SECL(can1_tx_ev);
static THD_WORKING_AREA(wa_thd_can1_rx, CAN_THREAD_STACK_SIZE);
static THD_WORKING_AREA(wa_thd_can1_tx, CAN_THREAD_STACK_SIZE);

static struct CanInit can1_init = {
  .cfg = &can1_cfg,
  .dev = &CAND1,
  .mutex = &can1_mtx,
  .tx_request = &can1_tx_ev
};

#endif

// Init all CAN periph
void cam_hw_init(void)
{
#if USE_CAN1
  canStart(can1_init.dev, can1_init.cfg);
  chThdCreateStatic(wa_thd_can1_rx, sizeof(wa_thd_can1_rx), NORMALPRIO + 8, can_rx, (void *)(&can1_init));
#endif
}

// Transmit message
int can_hw_transmit(uint32_t id, const uint8_t *buf, uint8_t len)
{
#if USE_CAN1
  if (len > 8) {
    return -1; // frame is too long
  }

  // FIXME only work with CAN1 for now, arch indep API should be changed to have a can_periph structure
  chMtxLock(can1_init.mutex);
  can1_init.tx_msg.DLC = len;
  memcpy(can1_init.tx_msg.data8, buf, len);
  can1_init.tx_msg.EID = id;
  can1_init.tx_msg.IDE = CAN_IDE_EXT;
  can1_init.tx_msg.RTR = CAN_RTR_DATA;
  chMtxUnlock(can1_init.mutex);
  chEvtBroadcast(can1_init.tx_request);
#endif

  return 0;
}

/// UAVCAN PART TODO

#if 0

#include <canard.h>
#include <string.h>

struct uavcan_iface_t {
  CANDriver *can_driver;
  CANConfig can_cfg;

  event_source_t tx_request;
  mutex_t mutex;
  void *thread_rx_wa;
  void *thread_tx_wa;
  void *thread_uavcan_wa;
  size_t thread_rx_wa_size;
  size_t thread_tx_wa_size;
  size_t thread_uavcan_wa_size;

  uint8_t node_id;
  CanardInstance canard;
  uint8_t canard_memory_pool[1024*2];

  // Dynamic node id allocation
  uint32_t send_next_node_id_allocation_request_at_ms;
  uint8_t node_id_allocation_unique_id_offset;

  uint8_t transfer_id;
};

#if STM32_CAN_USE_CAN1
static THD_WORKING_AREA(can1_rx_wa, 1024*2);
static THD_WORKING_AREA(can1_tx_wa, 1024*2);

static struct uavcan_iface_t can1_iface = {
  .can_driver = &CAND1,
  .can_cfg = {
    CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_TXFP,
    CAN_BTR_SJW(0) | CAN_BTR_TS2(1) |
    CAN_BTR_TS1(14) | CAN_BTR_BRP((STM32_PCLK1/18)/125000 - 1)
  },
  .thread_rx_wa = can1_rx_wa,
  .thread_rx_wa_size = sizeof(can1_rx_wa),
  .thread_tx_wa = can1_tx_wa,
  .thread_tx_wa_size = sizeof(can1_tx_wa),
  .node_id = 100,
  .transfer_id = 0
};
#endif

#if STM32_CAN_USE_CAN2
static THD_WORKING_AREA(can2_rx_wa, 1024*2);
static THD_WORKING_AREA(can2_tx_wa, 1024*2);

static struct uavcan_iface_t can2_iface = {
  .can_driver = &CAND2,
  .can_cfg = {
    CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_TXFP,
    CAN_BTR_SJW(0) | CAN_BTR_TS2(1) |
    CAN_BTR_TS1(14) | CAN_BTR_BRP((STM32_PCLK1/18)/125000 - 1)
  },
  .thread_rx_wa = can2_rx_wa,
  .thread_rx_wa_size = sizeof(can2_rx_wa),
  .thread_tx_wa = can2_tx_wa,
  .thread_tx_wa_size = sizeof(can2_tx_wa),
  .node_id = 100,
  .transfer_id = 0
};
#endif

int16_t actuators_uavcan_values[ACTUATORS_UAVCAN_NB];

CANConfig can_cfg = {
  CAN_MCR_ABOM | CAN_MCR_AWUM | CAN_MCR_TXFP,
  CAN_BTR_SJW(0) | CAN_BTR_TS2(1) |
  CAN_BTR_TS1(14) | CAN_BTR_BRP((STM32_PCLK1/18)/125000 - 1)
};

/*
 * Receiver thread.
 */
static THD_FUNCTION(can_rx, p) {
  event_listener_t el;
  CANRxFrame rx_msg;
  CanardCANFrame rx_frame;
  struct uavcan_iface_t *iface = (struct uavcan_iface_t *)p;

  chRegSetThreadName("can_rx");
  chEvtRegister(&iface->can_driver->rxfull_event, &el, EVENT_MASK(0));
  while (true) {
    if (chEvtWaitAnyTimeout(ALL_EVENTS, TIME_MS2I(100)) == 0)
      continue;
    chMtxLock(&iface->mutex);
    while (canReceive(iface->can_driver, CAN_ANY_MAILBOX, &rx_msg, TIME_IMMEDIATE) == MSG_OK) {
      // Process message.
      const uint32_t timestamp = TIME_I2US(chVTGetSystemTimeX());
      memcpy(rx_frame.data, rx_msg.data8, 8);
      rx_frame.data_len = rx_msg.DLC;
      if(rx_msg.IDE) {
        rx_frame.id = CANARD_CAN_FRAME_EFF | rx_msg.EID;
      } else {
        rx_frame.id = rx_msg.SID;
      }
 
      canardHandleRxFrame(&iface->canard, &rx_frame, timestamp);
    }
    chMtxUnlock(&iface->mutex);
  }
  chEvtUnregister(&iface->can_driver->rxfull_event, &el);
}

/*
 * Transmitter thread.
 */
static THD_FUNCTION(can_tx, p) {
  event_listener_t txc, txe, txr;
  struct uavcan_iface_t *iface = (struct uavcan_iface_t *)p;
  uint8_t err_cnt = 0;

  chRegSetThreadName("can_tx");
  chEvtRegister(&iface->can_driver->txempty_event, &txc, EVENT_MASK(0));
  chEvtRegister(&iface->can_driver->error_event, &txe, EVENT_MASK(1));
  chEvtRegister(&iface->tx_request, &txr, EVENT_MASK(2));

  while (true) {
    eventmask_t evts = chEvtWaitAnyTimeout(ALL_EVENTS, TIME_MS2I(100));
    if (evts == 0)
      continue;

    if(evts & EVENT_MASK(1))
    {
      chEvtGetAndClearFlags(&txe);
      continue;
    }

    chMtxLock(&iface->mutex);
    for (const CanardCANFrame* txf = NULL; (txf = canardPeekTxQueue(&iface->canard)) != NULL;) {
      CANTxFrame tx_msg;
      tx_msg.DLC = txf->data_len;
      memcpy(tx_msg.data8, txf->data, 8);
      tx_msg.EID = txf->id & CANARD_CAN_EXT_ID_MASK;
      tx_msg.IDE = CAN_IDE_EXT;
      tx_msg.RTR = CAN_RTR_DATA;
      if (canTransmit(iface->can_driver, CAN_ANY_MAILBOX, &tx_msg, TIME_IMMEDIATE) == MSG_OK) {
        err_cnt = 0;
        canardPopTxQueue(&iface->canard);
      } else {
        // After 5 retries giveup
        if(err_cnt >= 5) {
          err_cnt = 0;
          canardPopTxQueue(&iface->canard);
          continue;
        }

        // Timeout - just exit and try again later
        chMtxUnlock(&iface->mutex);
        err_cnt++;
        canardPopTxQueue(&iface->canard);
        chThdSleepMilliseconds(err_cnt * 5);
        chMtxLock(&iface->mutex);
        continue;
      }
    }
    chMtxUnlock(&iface->mutex);
  }
}

static void onTransferReceived(CanardInstance* ins, CanardRxTransfer* transfer) {

}

static bool shouldAcceptTransfer(const CanardInstance* ins,
                                 uint64_t* out_data_type_signature,
                                 uint16_t data_type_id,
                                 CanardTransferType transfer_type,
                                 uint8_t source_node_id) {
  return false;
}

static void uavcanInitIface(struct uavcan_iface_t *iface) {
  chMtxObjectInit(&iface->mutex);
  chEvtObjectInit(&iface->tx_request);
  canardInit(&iface->canard, iface->canard_memory_pool, sizeof(iface->canard_memory_pool),
    onTransferReceived, shouldAcceptTransfer, iface);
  canardSetLocalNodeID(&iface->canard, iface->node_id);

  canStart(iface->can_driver, &iface->can_cfg);
  chThdCreateStatic(iface->thread_rx_wa, iface->thread_rx_wa_size, NORMALPRIO + 8, can_rx, (void*)iface);
  chThdCreateStatic(iface->thread_tx_wa, iface->thread_tx_wa_size, NORMALPRIO + 7, can_tx, (void*)iface);
}

void actuators_uavcan_init(void)
{
  /*----------------
   * Configure CAN busses
   *----------------*/
  uavcanInitIface(&can1_iface);
  uavcanInitIface(&can2_iface);
}

#define UAVCAN_EQUIPMENT_ESC_RAWCOMMAND_ID                 1030
#define UAVCAN_EQUIPMENT_ESC_RAWCOMMAND_SIGNATURE          (0x217F5C87D7EC951DULL)
#define UAVCAN_EQUIPMENT_ESC_RAWCOMMAND_MAX_SIZE           ((285 + 7)/8)

static void transmit_esc_raw(struct uavcan_iface_t *iface, uint8_t start_idx, uint8_t cnt) {
  uint8_t buffer[UAVCAN_EQUIPMENT_ESC_RAWCOMMAND_MAX_SIZE];
  uint32_t offset = 0;

  for(uint8_t i = start_idx; i < (start_idx+cnt); i++) {
    canardEncodeScalar(buffer, offset, 14, (void *)&actuators_uavcan_values[i]);
    offset += 14;
  }

  chMtxLock(&iface->mutex);
  canardBroadcast(&iface->canard,
      UAVCAN_EQUIPMENT_ESC_RAWCOMMAND_SIGNATURE,
      UAVCAN_EQUIPMENT_ESC_RAWCOMMAND_ID, &iface->transfer_id,
      CANARD_TRANSFER_PRIORITY_HIGH, buffer, (offset+7)/8);
  chMtxUnlock(&iface->mutex);
  chEvtBroadcast(&iface->tx_request);
}


void actuators_uavcan_commit(void)
{ 
  RunOnceEvery(5, {
  transmit_esc_raw(&can1_iface, 0, 10);
  transmit_esc_raw(&can2_iface, 10, 10);
  });
}

#endif


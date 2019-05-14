/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include <stdlib.h>
#include "unity.h"
#include "tusb_option.h"
#include "tusb_errors.h"
#include "binary.h"
#include "type_helper.h"

#include "hal.h"
#include "mock_osal.h"
#include "hcd.h"
#include "mock_usbh_hcd.h"
#include "ehci.h"
#include "ehci_controller_fake.h"
#include "host_helper.h"

usbh_device_t _usbh_devices[CFG_TUSB_HOST_DEVICE_MAX+1];

static uint8_t const hub_addr = 2;
static uint8_t const hub_port = 2;
static uint8_t dev_addr;
static uint8_t hostid;

static ehci_qhd_t *async_head;

//--------------------------------------------------------------------+
// Setup/Teardown + helper declare
//--------------------------------------------------------------------+
void setUp(void)
{
  TEST_ASSERT_STATUS( hcd_init() );

  dev_addr = 1;
  hostid = RANDOM(CONTROLLER_HOST_NUMBER) + TEST_CONTROLLER_HOST_START_INDEX;

  tu_memclr(_usbh_devices, sizeof(usbh_device_t)*(CFG_TUSB_HOST_DEVICE_MAX+1));
  helper_usbh_device_emulate(dev_addr, hub_addr, hub_port, hostid, TUSB_SPEED_HIGH);

  async_head =  get_async_head( hostid );
}

void tearDown(void)
{
}

void verify_open_qhd(ehci_qhd_t *p_qhd, uint8_t endpoint_addr, uint16_t max_packet_size)
{
  TEST_ASSERT_EQUAL(dev_addr, p_qhd->device_address);
  TEST_ASSERT_FALSE(p_qhd->non_hs_period_inactive_next_xact);
  TEST_ASSERT_EQUAL(endpoint_addr & 0x0F, p_qhd->endpoint_number);
  TEST_ASSERT_EQUAL(_usbh_devices[dev_addr].speed, p_qhd->endpoint_speed);
  TEST_ASSERT_EQUAL(max_packet_size, p_qhd->max_package_size);
  TEST_ASSERT_EQUAL(0, p_qhd->nak_count_reload); // TDD NAK Reload disable

  TEST_ASSERT_EQUAL(hub_addr, p_qhd->hub_address);
  TEST_ASSERT_EQUAL(hub_port, p_qhd->hub_port);
  TEST_ASSERT_EQUAL(1, p_qhd->mult); // TDD operation model for mult

  TEST_ASSERT_FALSE(p_qhd->qtd_overlay.halted);
  TEST_ASSERT(p_qhd->qtd_overlay.next.terminate);
  TEST_ASSERT(p_qhd->qtd_overlay.alternate.terminate);

  //------------- HCD -------------//
  TEST_ASSERT(p_qhd->used);
  TEST_ASSERT_FALSE(p_qhd->is_removing);
  TEST_ASSERT_NULL(p_qhd->p_qtd_list_head);
  TEST_ASSERT_NULL(p_qhd->p_qtd_list_tail);
}

//--------------------------------------------------------------------+
// PIPE OPEN
//--------------------------------------------------------------------+
tusb_desc_endpoint_t const desc_ept_bulk_in =
{
    .bLength          = sizeof(tusb_desc_endpoint_t),
    .bDescriptorType  = TUSB_DESC_TYPE_ENDPOINT,
    .bEndpointAddress = 0x81,
    .bmAttributes     = { .xfer = TUSB_XFER_BULK },
    .wMaxPacketSize   = 512,
    .bInterval        = 0
};

tusb_desc_endpoint_t const desc_ept_bulk_out =
{
    .bLength          = sizeof(tusb_desc_endpoint_t),
    .bDescriptorType  = TUSB_DESC_TYPE_ENDPOINT,
    .bEndpointAddress = 0x01,
    .bmAttributes     = { .xfer = TUSB_XFER_BULK },
    .wMaxPacketSize   = 512,
    .bInterval        = 0
};

void verify_bulk_open_qhd(ehci_qhd_t *p_qhd, tusb_desc_endpoint_t const * desc_endpoint, uint8_t class_code)
{
  verify_open_qhd(p_qhd, desc_endpoint->bEndpointAddress, desc_endpoint->wMaxPacketSize.size);

  TEST_ASSERT_FALSE(p_qhd->head_list_flag);
  TEST_ASSERT_EQUAL(0, p_qhd->data_toggle_control);
  TEST_ASSERT_EQUAL(0, p_qhd->interrupt_smask);
  TEST_ASSERT_EQUAL(0, p_qhd->non_hs_interrupt_cmask);
  TEST_ASSERT_FALSE(p_qhd->non_hs_control_endpoint);

  //  TEST_ASSERT_EQUAL(desc_endpoint->bInterval); TDD highspeed bulk/control OUT

  TEST_ASSERT_EQUAL(desc_endpoint->bEndpointAddress & 0x80 ? EHCI_PID_IN : EHCI_PID_OUT, p_qhd->pid_non_control);

  TEST_ASSERT_EQUAL(class_code, p_qhd->class_code);
  //------------- async list check -------------//
  TEST_ASSERT_EQUAL_HEX((uint32_t) p_qhd, tu_align32(async_head->next.address));
  TEST_ASSERT_FALSE(async_head->next.terminate);
  TEST_ASSERT_EQUAL(EHCI_QUEUE_ELEMENT_QHD, async_head->next.type);
}

void test_open_bulk_qhd_data(void)
{
  ehci_qhd_t *p_qhd;
  pipe_handle_t pipe_hdl;
  tusb_desc_endpoint_t const * desc_endpoint = &desc_ept_bulk_in;

  //------------- Code Under TEST -------------//
  pipe_hdl = hcd_edpt_open(dev_addr, desc_endpoint, TUSB_CLASS_MSC);

  TEST_ASSERT_EQUAL(dev_addr, pipe_hdl.dev_addr);
  TEST_ASSERT_EQUAL(TUSB_XFER_BULK, pipe_hdl.xfer_type);

  p_qhd = &ehci_data.device[ pipe_hdl.dev_addr-1 ].qhd[ pipe_hdl.index ];
  verify_bulk_open_qhd(p_qhd, desc_endpoint, TUSB_CLASS_MSC);

  //------------- async list check -------------//
  TEST_ASSERT_EQUAL_HEX((uint32_t) p_qhd, tu_align32(async_head->next.address));
  TEST_ASSERT_FALSE(async_head->next.terminate);
  TEST_ASSERT_EQUAL(EHCI_QUEUE_ELEMENT_QHD, async_head->next.type);
}

void test_open_bulk_hs_out_pingstate(void)
{
  ehci_qhd_t *p_qhd;
  pipe_handle_t pipe_hdl;

  //------------- Code Under TEST -------------//
  pipe_hdl = hcd_edpt_open(dev_addr, &desc_ept_bulk_out, TUSB_CLASS_MSC);

  p_qhd = &ehci_data.device[ pipe_hdl.dev_addr-1 ].qhd[ pipe_hdl.index ];
  TEST_ASSERT(p_qhd->qtd_overlay.pingstate_err);
}

//--------------------------------------------------------------------+
// PIPE CLOSE
//--------------------------------------------------------------------+
void test_bulk_close(void)
{
  tusb_desc_endpoint_t const * desc_endpoint = &desc_ept_bulk_in;
  pipe_handle_t pipe_hdl = hcd_edpt_open(dev_addr, desc_endpoint, TUSB_CLASS_MSC);
  ehci_qhd_t *p_qhd = &ehci_data.device[ pipe_hdl.dev_addr-1].qhd[ pipe_hdl.index ];

  //------------- Code Under TEST -------------//
  hcd_pipe_close(pipe_hdl);

  TEST_ASSERT(p_qhd->is_removing);
  TEST_ASSERT( tu_align32(async_head->next.address) != (uint32_t) p_qhd );
  TEST_ASSERT_EQUAL_HEX( (uint32_t) async_head, tu_align32(p_qhd->next.address) );
  TEST_ASSERT_EQUAL(EHCI_QUEUE_ELEMENT_QHD, p_qhd->next.type);
}

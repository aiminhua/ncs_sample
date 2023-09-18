/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <bluetooth/services/nus.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <nrfx.h>
#include <hal/nrf_power.h>
#include <zephyr/drivers/flash/nrf_qspi_nor.h>
#include <zephyr/pm/pm.h>
#include <hal/nrf_regulators.h>

LOG_MODULE_REGISTER(main);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN	(sizeof(DEVICE_NAME) - 1)

#define RUN_STATUS_LED DK_LED1
#define RUN_LED_BLINK_INTERVAL 1000

#define CON_STATUS_LED DK_LED2

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};
static struct bt_conn *current_conn;
static bool xip_enabled = true;
static bool xip_restore = false;

static void enable_xip(bool enable)
{
	const struct device *devQSPI;	
	// LOG_INF("=%d", enable);
	devQSPI = DEVICE_DT_GET(DT_INST(0, nordic_qspi_nor));
	nrf_qspi_nor_xip_enable(devQSPI, enable);
	xip_enabled = enable;
}
#if 1
/* Invoke Low Power/System Off specific Tasks */
void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SOFT_OFF:
		nrf_regulators_system_off(NRF_REGULATORS);
		break;
	case PM_STATE_RUNTIME_IDLE:
		if (xip_enabled)
		{
			enable_xip(false);
			xip_restore = true;			
		}
		k_cpu_idle();			
		break;		
	default:
		printk("Unsupported power state %u\n\r", state);
		break;
	}
}

void restore_xip()
{
	if (xip_restore)
	{
		enable_xip(true);
		xip_restore = false;
		// LOG_INF("=%d", k_ticks_to_ms_floor32(ticks));
	}	
}
#endif

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		printk("Connection failed (err %u)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Connected %s", (addr));

	current_conn = bt_conn_ref(conn);

	dk_set_led_on(CON_STATUS_LED);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s (reason %u)", (addr), reason);

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
		dk_set_led_off(CON_STATUS_LED);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
};

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
			  uint16_t len)
{
	// int err;
	char addr[BT_ADDR_LE_STR_LEN] = {0};

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, ARRAY_SIZE(addr));

	printk("Received data from: %s", (addr));

}


static struct bt_nus_cb nus_cb = {
	.received = bt_receive_cb,
};

extern const k_tid_t adc_thread_id;
// #define SPI_FLASH_TEST_REGION_OFFSET 0x330000
#define SPI_FLASH_TEST_REGION_OFFSET 0
#define SPI_FLASH_SECTOR_SIZE        4096

void set_device_pm_state(void)
{	
	int err = 0;
	const struct device *devUart0;
	const struct device *devSPI4;

	devUart0 =  DEVICE_DT_GET(DT_NODELABEL(uart0));
	devSPI4 =	DEVICE_DT_GET(DT_NODELABEL(spi4));

	err = pm_device_runtime_enable(devUart0);

	//this is a workaround for SPI device since its driver doesn't support runtime PM
	err |= pm_device_action_run(devSPI4, PM_DEVICE_ACTION_SUSPEND);
	if (err) {
		printk("PM enable error (err: %d)", err);
	}	
}

// void button_changed(uint32_t button_state, uint32_t has_changed)
// {
// 	uint32_t buttons = button_state & has_changed;

// 	if (buttons & DK_BTN1_MSK) {
// 		printk("button1 isr");
// #ifdef CONFIG_PM_DEVICE		
// 		set_device_pm_state();
// #endif		
// 	}
// }

extern const uint32_t xip_const[4];
extern uint32_t get_xip_var(uint8_t i);

void main(void)
{
	const uint8_t expected[] = { 0x55, 0xaa, 0x66, 0x99 };
	const size_t len = sizeof(expected);
	uint8_t buf[sizeof(expected)];
	const struct device *flash_dev;
	const struct flash_area *fap;
	int rc, err;
	uint32_t loop = 0;
	uint32_t val;

	
	printk("\n\r### reset reason: 0x%08x  ####\r\n\n\r", NRF_RESET_S->RESETREAS);
	NRF_RESET_S->RESETREAS = 0xFFFFFFFF;

	set_device_pm_state();

	// err = dk_buttons_init(button_changed);
	// if (err) {
	// 	printk("Cannot init buttons (err: %d)", err);
	// }

	err = dk_leds_init();
	if (err) {
		printk("Cannot init LEDs (err: %d)", err);
	}

	err = bt_enable(NULL);
	if (err) {
		printk("bt enable error %d", err);
	}

	printk("Bluetooth initialized\r");

	err = bt_nus_init(&nus_cb);
	if (err) {
		printk("Failed to initialize UART service (err: %d)", err);
		return;
	}

	struct bt_le_adv_param adv_para;
	memset(&adv_para, 0, sizeof(struct bt_le_adv_param));
	adv_para.options = BT_LE_ADV_OPT_CONNECTABLE;
	adv_para.interval_min = BT_GAP_ADV_FAST_INT_MIN_2 * 3;
	adv_para.interval_max = BT_GAP_ADV_FAST_INT_MAX_2 * 2;

	err = bt_le_adv_start(&adv_para, ad, ARRAY_SIZE(ad), NULL,
			      0);

	if (err) {
		printk("Advertising failed to start (err %d)", err);
		return;
	}

	// flash_dev = DEVICE_DT_GET(DT_ALIAS(spi_flash0));
	rc = flash_area_open(PM_EXTERNAL_FLASH_ID, &fap);
	flash_dev = fap->fa_dev;

	if (!device_is_ready(flash_dev)) {
		printk("%s: device not ready.\n", flash_dev->name);
		return;
	}

	printk("\nXIP variable: %p\n", xip_const);
	val = xip_const[0] + 0x10;
	printk("XIP variable calculation result: %x\r\n", val);

	while (1)
	{
		printk("\n\r\n\r##### Loop: %d  start ######\n\r\n\r", ++loop);
		
		dk_set_led(RUN_STATUS_LED, loop%2);			
	
		k_thread_suspend(adc_thread_id);

		enable_xip(false);

		printk("\n#####Switched to QSPI DMA #####\n");
		printk("\nWatch the power consumption of QSPI DMA mode\n");
	
		k_sleep(K_SECONDS(10));		

		printk("\nTest 1: Flash erase\n");

		rc = flash_area_erase(fap, SPI_FLASH_TEST_REGION_OFFSET, SPI_FLASH_SECTOR_SIZE);
		if (rc != 0) {
			printk("Flash erase failed! %d\n", rc);
		} else {
			printk("Flash erase succeeded!\n");
		}

		printk("\nTest 2: Flash write\n");

		printk("Attempting to write %zu bytes\n", len);
		
		rc = flash_area_write(fap, SPI_FLASH_TEST_REGION_OFFSET, expected, len);
		if (rc != 0) {
			printk("Flash write failed! %d\n", rc);
			return;
		}

		memset(buf, 0, len);
		
		rc = flash_area_read(fap, SPI_FLASH_TEST_REGION_OFFSET, buf, len);
		if (rc != 0) {
			printk("Flash read failed! %d\n", rc);
			return;
		}

		if (memcmp(expected, buf, len) == 0) {
			printk("Data read matches data written. Good!!\n");
		} else {
			const uint8_t *wp = expected;
			const uint8_t *rp = buf;
			const uint8_t *rpe = rp + len;

			printk("Data read does not match data written!!\n");
			while (rp < rpe) {
				printk("%08x wrote %02x read %02x %s\n",
					(uint32_t)(SPI_FLASH_TEST_REGION_OFFSET + (rp - buf)),
					*wp, *rp, (*rp == *wp) ? "match" : "MISMATCH");
				++rp;
				++wp;
			}
		}

		enable_xip(true);
		k_thread_resume(adc_thread_id);
		printk("\n#####Switched to QSPI XIP #####\n");
		printk("\nWatch the power consumption of QSPI XIP mode\n");

		k_sleep(K_SECONDS(20));	

		printk("get xip var:0x%x\r\n", get_xip_var(loop));
		printk("XIP variable calculation result: %x\r\n", xip_const[1]+loop);
		
		printk("\n\r\n\r##### Loop: %d  end ######\n\r\n\r", loop);		

	}
}

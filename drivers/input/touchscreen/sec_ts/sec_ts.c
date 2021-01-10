/* drivers/input/touchscreen/sec_ts.c
 *
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 * http://www.samsungsemi.com/
 *
 * Core file for Samsung TSC driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

struct sec_ts_data *tsp_info;

#include "sec_ts.h"

/* Switch GPIO values */
#define SEC_SWITCH_GPIO_VALUE_SLPI_MASTER	1
#define SEC_SWITCH_GPIO_VALUE_AP_MASTER		0

struct sec_ts_data *ts_dup;

#ifndef CONFIG_SEC_SYSFS
/* Declare extern sec_class */
struct class *sec_class;
#endif

#ifdef USE_POWER_RESET_WORK
static void sec_ts_reset_work(struct work_struct *work);
#endif
static void sec_ts_fw_update_work(struct work_struct *work);
static void sec_ts_suspend_work(struct work_struct *work);
static void sec_ts_resume_work(struct work_struct *work);
static void sec_ts_charger_work(struct work_struct *work);

#ifdef USE_OPEN_CLOSE
static int sec_ts_input_open(struct input_dev *dev);
static void sec_ts_input_close(struct input_dev *dev);
#endif

int sec_ts_read_information(struct sec_ts_data *ts);

#ifndef I2C_INTERFACE
int sec_ts_spi_delay(u8 reg)
{
	switch (reg) {
	case SEC_TS_READ_TOUCH_RAWDATA:
		return 400;
	case SEC_TS_CMD_HEATMAP_READ:
		return 500;
	case SEC_TS_READ_ALL_EVENT:
		return 500;
	case SEC_TS_READ_CSRAM_RTDP_DATA:
		return 500;
	case SEC_TS_CAAT_READ_STORED_DATA:
		return 500;
	case SEC_TS_CMD_FLASH_READ_DATA:
		return 1800;
	case SEC_TS_READ_FIRMWARE_INTEGRITY:
		return 20*1000;
	case SEC_TS_READ_SELFTEST_RESULT:
		return 3500;
	default: return 100;
	}
}

int sec_ts_spi_post_delay(u8 reg)
{
	switch (reg) {
	case SEC_TS_READ_TOUCH_RAWDATA:
	case SEC_TS_CMD_FLASH_READ_DATA:
	case SEC_TS_READ_SELFTEST_RESULT:
		return 500;
	default: return 0;
	}
}
#endif

int sec_ts_write(struct sec_ts_data *ts, u8 reg, u8 *data, int len)
{
	u8 *buf;
	int ret;
	unsigned char retry;
#ifdef I2C_INTERFACE
	struct i2c_msg msg;
#else
	struct spi_message msg;
	struct spi_transfer transfer[1] = { { 0 } };
	unsigned int i;
	unsigned int spi_len = 0;
	unsigned char checksum = 0x0;
#endif

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		input_err(true, &ts->client->dev,
			"%s: POWER_STATUS : OFF\n", __func__);
		goto err;
	}

#ifdef I2C_INTERFACE
	if (len + 1 > sizeof(ts->io_write_buf)) {
		input_err(true, &ts->client->dev,
			"%s: len is larger than buffer size\n", __func__);
		return -EINVAL;
	}
#else
	/* add 3 zero stuffing tx bytes at last */
	if (SEC_TS_SPI_HEADER_SIZE + 1 + len + SEC_TS_SPI_CHECKSUM_SIZE + 3 >
		sizeof(ts->io_write_buf)) {
		input_err(true, &ts->client->dev,
			"%s: len is larger than buffer size\n", __func__);
		return -EINVAL;
	}
#endif

	mutex_lock(&ts->io_mutex);

	buf = ts->io_write_buf;
#ifdef I2C_INTERFACE
	buf[0] = reg;
	memcpy(buf + 1, data, len);

	msg.addr = ts->client->addr;
	msg.flags = 0;
	msg.len = len + 1;
	msg.buf = buf;
#else

	buf[0] = SEC_TS_SPI_SYNC_CODE;
	buf[1] = ((len + 1) >> 8) & 0xFF;
	buf[2] = (len + 1) & 0xFF;
	buf[3] = 0x00;
	buf[4] = 0x00;
	buf[5] = reg;
	memcpy(buf + SEC_TS_SPI_HEADER_SIZE + 1, data, len);

	spi_len = SEC_TS_SPI_HEADER_SIZE + 1 + len;
	// spi_len = SPI header size(5)+register(1)+data size(len)
	for (i = 0; i < spi_len ; i++)
		checksum += buf[i];
	buf[spi_len++] = checksum;
	// spi_len += checksum(1)

	spi_message_init(&msg);

	/* add 3 zero stuffing tx bytes at last */
	memset(ts->io_write_buf + spi_len, 0x00, 3);
	/* spi transfer size should be multiple of 4
	 **/
	spi_len = (spi_len + 3) & ~3;
	transfer[0].len = spi_len;
	transfer[0].tx_buf = buf;
	transfer[0].rx_buf = NULL;
	spi_message_add_tail(&transfer[0], &msg);

#ifdef SEC_TS_DEBUG_IO
	input_info(true, &ts->client->dev, "%s: ", __func__);
//	for (i = 0; i < SEC_TS_SPI_HEADER_SIZE + 1 + len + 1; i++)
	for (i = 0; i < 8; i++)
		input_info(true, &ts->client->dev, "%X ", buf[i]);
	input_info(true, &ts->client->dev, "\n");
#endif

#endif

	for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
#ifdef I2C_INTERFACE
		if ((ret = i2c_transfer(ts->client->adapter, &msg, 1)) == 1)
			break;
#else
		if ((ret = spi_sync(ts->client, &msg)) == 0)
			break;
#endif

		if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
			input_err(true, &ts->client->dev,
				  "%s: POWER_STATUS : OFF, retry:%d\n",
				  __func__, retry);
			mutex_unlock(&ts->io_mutex);
			goto err;
		}

		usleep_range(1 * 1000, 1 * 1000);

		if (retry > 1) {
			input_err(true, &ts->client->dev,
				  "%s: retry %d\n", __func__, retry + 1);
			ts->comm_err_count++;
		}
	}

	mutex_unlock(&ts->io_mutex);

	if (retry == SEC_TS_IO_RETRY_CNT) {
		input_err(true, &ts->client->dev,
			  "%s: write over retry limit\n", __func__);
		ret = -EIO;
#ifdef USE_POR_AFTER_I2C_RETRY
		if (ts->probe_done && !ts->reset_is_on_going)
			schedule_delayed_work(&ts->reset_work,
				msecs_to_jiffies(TOUCH_RESET_DWORK_TIME));
#endif
	}

#ifdef I2C_INTERFACE
	if (ret == 1)
#else
	if (ret == 0)
#endif
		return 0;
err:
	return -EIO;
}

static int sec_ts_read_internal(struct sec_ts_data *ts, u8 reg,
			     u8 *data, int len, bool dma_safe)
{
	u8 *buf;
	int ret;
	unsigned char retry;
#ifdef I2C_INTERFASCE
	struct i2c_msg msg[2];
#else
	struct spi_message msg;
	struct spi_transfer transfer[1] = { { 0 } };
	unsigned int i;
	unsigned int spi_write_len = 0, spi_read_len = 0;
	unsigned char write_checksum = 0x0, read_checksum = 0x0;
	int copy_size = 0, copy_cur = 0;
#endif
	int remain = len;

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		input_err(true, &ts->client->dev,
			"%s: POWER_STATUS : OFF\n", __func__);
		goto err;
	}

#ifndef I2C_INTERFACE
	/* add 3 zero stuffing tx bytes at last */
	if (SEC_TS_SPI_HEADER_SIZE + 1 + SEC_TS_SPI_CHECKSUM_SIZE + 3 >
		sizeof(ts->io_write_buf)) {
		input_err(true, &ts->client->dev,
			"%s: len is larger than buffer size\n", __func__);
		return -EINVAL;
	}
#endif

	if (len > sizeof(ts->io_read_buf) && dma_safe == false) {
		input_err(true, &ts->client->dev,
			"%s: len %d over pre-allocated size %d\n",
			__func__, len, IO_PREALLOC_READ_BUF_SZ);
		return -ENOSPC;
	}

	mutex_lock(&ts->io_mutex);

	buf = ts->io_write_buf;
#ifdef I2C_INTERFACE
	buf[0] = reg;

	msg[0].addr = ts->client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = buf;

	msg[1].addr = ts->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	if (dma_safe == false)
		msg[1].buf = ts->io_read_buf;
	else
		msg[1].buf = data;
#else

	buf[0] = SEC_TS_SPI_SYNC_CODE;
	buf[1] = 0x00;
	buf[2] = 0x01;
	buf[3] = (len >> 8) & 0xFF;
	buf[4] = len & 0xFF;
	buf[5] = reg;

	spi_write_len = SEC_TS_SPI_HEADER_SIZE + 1;
	for (i = 0; i < spi_write_len; i++)
		write_checksum += buf[i];
	buf[spi_write_len] = write_checksum;
	spi_write_len += SEC_TS_SPI_CHECKSUM_SIZE;
	/* add 3 zero stuffing tx bytes at last */
	memset(ts->io_write_buf + spi_write_len, 0x00, 3);
	spi_write_len = (spi_write_len + 3) & ~3;

	spi_read_len = len +
		SEC_TS_SPI_READ_HEADER_SIZE + SEC_TS_SPI_CHECKSUM_SIZE;
	spi_read_len = (spi_read_len + 3) & ~3;
#endif
	if (len <= ts->io_burstmax) {
#ifdef I2C_INTERFACE
		for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
			ret = i2c_transfer(ts->client->adapter, msg, 2);
			if (ret == 2)
				break;

			usleep_range(1 * 1000, 1 * 1000);
			if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
				input_err(true, &ts->client->dev,
					  "%s: POWER_STATUS : OFF, retry:%d\n",
					  __func__, retry);
				mutex_unlock(&ts->io_mutex);
				goto err;
			}

			if (retry > 1) {
				input_err(true, &ts->client->dev,
					"%s: retry %d\n", __func__, retry + 1);
				ts->comm_err_count++;
			}
		}
		if (ret == 2 && dma_safe == false)
			memcpy(data,
			       ts->io_read_buf[SEC_TS_SPI_READ_HEADER_SIZE],
			       len);
#else
		for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
			spi_message_init(&msg);
			// spi transfer size should be multiple of 4
			transfer[0].len = spi_write_len;
			transfer[0].tx_buf = buf;
			transfer[0].rx_buf = NULL;
			spi_message_add_tail(&transfer[0], &msg);

			ret = spi_sync(ts->client, &msg);
#ifdef SEC_TS_DEBUG_IO
			input_info(true, &ts->client->dev,
				"%s: spi write buf %X %X %X %X %X %X %X\n",
				__func__, buf[0], buf[1], buf[2],
				buf[3], buf[4], buf[5], buf[6]);
#endif
			// write fail
			if (ret != 0) {
				ret = -EIO;

				input_err(true, &ts->client->dev,
					"%s: spi write retry %d\n",
					__func__, retry + 1);
				ts->comm_err_count++;

				usleep_range(1 * 1000, 1 * 1000);
				if (ts->power_status ==
					SEC_TS_STATE_POWER_OFF) {
					input_err(true, &ts->client->dev,
						"%s: POWER_STATUS : OFF, retry:%d\n",
						__func__, retry);
					mutex_unlock(&ts->io_mutex);
					goto err;
				}

				if (retry == SEC_TS_IO_RETRY_CNT - 1) {
					input_err(true, &ts->client->dev,
						"%s: write reg retry over retry limit, skip read\n",
						__func__);
					goto skip_spi_read;
				}

				continue;
			}

			usleep_range(sec_ts_spi_delay(reg),
					sec_ts_spi_delay(reg) + 1);

			// read sequence start
			spi_message_init(&msg);
			transfer[0].len = spi_read_len;
			transfer[0].tx_buf = NULL;
			transfer[0].rx_buf = ts->io_read_buf;
			spi_message_add_tail(&transfer[0], &msg);
			ret = spi_sync(ts->client, &msg);

			for (i = 0, read_checksum = 0x0;
				i < (SEC_TS_SPI_READ_HEADER_SIZE + len);
				i++)
				read_checksum += ts->io_read_buf[i];

#ifdef SEC_TS_DEBUG_IO
			input_info(true, &ts->client->dev, "%s: ", __func__);
//			for (i = 0; i < spi_read_len; i++)
			for (i = 0; i < 8; i++)
				input_info(true, &ts->client->dev,
					"%X ",
					ts->io_read_buf[i]);
			input_info(true, &ts->client->dev,
				"\n%s: checksum = %X",
				__func__, read_checksum);
#endif
			// read fail
			if (ret != 0 ||
				ts->io_read_buf[0] != SEC_TS_SPI_SYNC_CODE ||
				reg != ts->io_read_buf[5] ||
				// ts->io_read_buf[6] != SEC_TS_SPI_CMD_OK ||
				read_checksum !=
					ts->io_read_buf[
						SEC_TS_SPI_READ_HEADER_SIZE +
					len]) {

				ret = -EIO;

				input_err(true, &ts->client->dev,
					"%s: retry %d\n",
					__func__, retry + 1);
				ts->comm_err_count++;

				usleep_range(1 * 1000, 1 * 1000);
				if (ts->power_status ==
					SEC_TS_STATE_POWER_OFF) {
					input_err(true, &ts->client->dev,
						"%s: POWER_STATUS : OFF, retry:%d\n",
						__func__, retry);
					mutex_unlock(&ts->io_mutex);
					goto err;
				}
				continue;
			} else
				break;
		}
		if (ret == 0)
			memcpy(data, ts->io_read_buf +
			       SEC_TS_SPI_READ_HEADER_SIZE, len);

		usleep_range(sec_ts_spi_post_delay(reg),
			     sec_ts_spi_post_delay(reg) + 1);

#endif //I2C_INTERFACE
	} else {
		/*
		 * read buffer is 256 byte. do not support long buffer over
		 * than 256. So, try to separate reading data about 256 bytes.
		 **/
#ifdef I2C_INTERFACE
		for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
			ret = i2c_transfer(ts->client->adapter, msg, 1);
			if (ret == 1)
				break;

			usleep_range(1 * 1000, 1 * 1000);
			if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
				input_err(true, &ts->client->dev,
					"%s: POWER_STATUS : OFF, retry:%d\n",
					__func__, retry);
				mutex_unlock(&ts->io_mutex);
				goto err;
			}

			if (retry > 1) {
				input_err(true, &ts->client->dev,
					"%s: retry %d\n",
					__func__, retry + 1);
				ts->comm_err_count++;
			}
		}

		do {
			if (remain > ts->io_burstmax)
				msg[1].len = ts->io_burstmax;
			else
				msg[1].len = remain;

			remain -= ts->io_burstmax;

			for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
				ret = i2c_transfer(ts->client->adapter,
						   &msg[1], 1);
				if (ret == 1)
					break;
				usleep_range(1 * 1000, 1 * 1000);
				if (ts->power_status ==
					SEC_TS_STATE_POWER_OFF) {
					input_err(true, &ts->client->dev,
						"%s: POWER_STATUS : OFF, retry:%d\n",
						__func__, retry);
					mutex_unlock(&ts->io_mutex);
					goto err;
				}

				if (retry > 1) {
					input_err(true, &ts->client->dev,
						"%s: retry %d\n",
						__func__, retry + 1);
					ts->comm_err_count++;
				}
			}

			msg[1].buf += msg[1].len;

		} while (remain > 0);

		if (ret == 1 && dma_safe == false)
			memcpy(data, ts->io_read_buf, len);
#else
		for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
			spi_message_init(&msg);
			// spi transfer size should be multiple of 4
			transfer[0].len = spi_write_len;
			transfer[0].tx_buf = buf;
			transfer[0].rx_buf = NULL;
			spi_message_add_tail(&transfer[0], &msg);

			ret = spi_sync(ts->client, &msg);

			// write fail
			if (ret != 0) {
				ret = -EIO;

				input_err(true, &ts->client->dev,
					"%s: spi write retry %d\n",
					__func__, retry + 1);
				ts->comm_err_count++;

				usleep_range(1 * 1000, 1 * 1000);

				if (ts->power_status ==
					SEC_TS_STATE_POWER_OFF) {
					input_err(true, &ts->client->dev,
						"%s: POWER_STATUS : OFF, retry:%d\n",
						__func__, retry);
					mutex_unlock(&ts->io_mutex);
					goto err;
				}

				if (retry == SEC_TS_IO_RETRY_CNT - 1) {
					input_err(true, &ts->client->dev,
						"%s: write reg retry over retry limit, skip read\n",
						__func__);
					goto skip_spi_read;
				}

				continue;
			}

			usleep_range(sec_ts_spi_delay(reg),
					sec_ts_spi_delay(reg) + 1);

			copy_size = 0;
			remain = spi_read_len;
			do {
				if (remain > ts->io_burstmax)
					copy_cur = ts->io_burstmax;
				else
					copy_cur = remain;

				spi_message_init(&msg);

				transfer[0].len = copy_cur;
				transfer[0].tx_buf = NULL;
				transfer[0].rx_buf =
					&ts->io_read_buf[copy_size];
				// CS needs to stay low until read seq. is done
				transfer[0].cs_change =
					(remain > ts->io_burstmax) ? 1 : 0;

				spi_message_add_tail(&transfer[0], &msg);

				copy_size += copy_cur;
				remain -= copy_cur;

				ret = spi_sync(ts->client, &msg);
#ifdef SEC_TS_DEBUG_IO
				input_info(true, &ts->client->dev,
					"%s: ", __func__);
				for (i = 0; i < 8; i++)
					input_info(true,
						   &ts->client->dev, "%X ",
						   ts->io_read_buf[i]);
				input_info(true, &ts->client->dev,
					"\n%s: checksum = %X",
					__func__, read_checksum);
#endif

				if (ret != 0) {
					ret = -EIO;

					input_err(true, &ts->client->dev,
						"%s: retry %d\n",
						__func__, retry + 1);
					ts->comm_err_count++;

					usleep_range(1 * 1000, 1 * 1000);
					if (ts->power_status
						== SEC_TS_STATE_POWER_OFF) {
						input_err(true,
							&ts->client->dev,
							"%s: POWER_STATUS : OFF, retry:%d\n",
							__func__, retry);
						mutex_unlock(&ts->io_mutex);
						goto err;
					}
					break;
				}
			} while (remain > 0);
			if (ret != 0) { // read fail, retry
				ret = -EIO;
				continue;
			}

			for (i = 0, read_checksum = 0x0;
				i < SEC_TS_SPI_READ_HEADER_SIZE + len; i++)
				read_checksum += ts->io_read_buf[i];
			//read success
			if (ts->io_read_buf[0] == SEC_TS_SPI_SYNC_CODE &&
				// ts->io_read_buf[6] == SEC_TS_SPI_CMD_OK &&
				reg == ts->io_read_buf[5] &&
				read_checksum ==
				ts->io_read_buf[SEC_TS_SPI_READ_HEADER_SIZE +
				len])
				break;
			//read data fail
			else if (ts->io_read_buf[6]
				 == SEC_TS_SPI_CMD_UNKNOWN ||
				 ts->io_read_buf[6]
				 == SEC_TS_SPI_CMD_BAD_PARAM) {
				input_info(true, &ts->client->dev,
					"%s: CMD_NG cmd(M) = %X, cmd(S) = %X, cmd_result = %X\n",
					__func__, reg, ts->io_read_buf[5],
					ts->io_read_buf[6]);
				ret = -EIO;
				continue;
			} else {
				input_info(true, &ts->client->dev,
					"%s: spi fail, ret %d, sync code %X, reg(M) %X, reg(S) %X, cmd_result %X, chksum(M) %X, chksum(S) %X\n",
					__func__, ret, ts->io_read_buf[0],
					reg, ts->io_read_buf[5],
					ts->io_read_buf[6], read_checksum,
					ts->io_read_buf[
						SEC_TS_SPI_READ_HEADER_SIZE +
					len]);
				ret = -EIO;
				continue;
			}
		}
		if (ret == 0)
			memcpy(data, ts->io_read_buf +
				SEC_TS_SPI_READ_HEADER_SIZE, len);

		usleep_range(sec_ts_spi_post_delay(reg),
			     sec_ts_spi_post_delay(reg) + 1);
#endif
	}
skip_spi_read:
	mutex_unlock(&ts->io_mutex);

	if (retry == SEC_TS_IO_RETRY_CNT) {
		input_err(true, &ts->client->dev,
			"%s: read reg(%#x) over retry limit, comm_err_count %d, io_err_count %d\n",
			__func__, reg, ts->comm_err_count, ts->io_err_count);
		ret = -EIO;
		ts->io_err_count++;
#ifdef USE_POR_AFTER_I2C_RETRY
		if (ts->probe_done && !ts->reset_is_on_going)
			schedule_delayed_work(&ts->reset_work,
				msecs_to_jiffies(TOUCH_RESET_DWORK_TIME));
#endif

	} else
		ts->io_err_count = 0;

	/* do hw reset if continuously failed over SEC_TS_IO_RESET_CNT times */
	if (ts->io_err_count >= SEC_TS_IO_RESET_CNT) {
		ts->io_err_count = 0;
		sec_ts_hw_reset(ts);
	}

	return ret;

err:
	return -EIO;
}

static int sec_ts_write_burst_internal(struct sec_ts_data *ts,
					   u8 *data, int len, bool dma_safe)
{
	int ret;
	int retry;
#ifndef I2C_INTERFACE
	struct spi_message msg;
	struct spi_transfer transfer[1] = { { 0 } };
	unsigned int i;
	unsigned int spi_len = 0;
	unsigned char checksum = 0x0;
#endif

#ifdef I2C_INTERFACE
	if (len > sizeof(ts->io_write_buf) && dma_safe == false) {
		input_err(true, &ts->client->dev,
			"%s: len %d over pre-allocated size %d\n",
			__func__, len, sizeof(ts->io_write_buf));
		return -ENOSPC;
	}
#else
	/* add 3 zero stuffing tx bytes at last */
	if (SEC_TS_SPI_HEADER_SIZE + len + SEC_TS_SPI_CHECKSUM_SIZE + 3 >
		sizeof(ts->io_write_buf)) {
		input_err(true, &ts->client->dev,
			"%s: len is larger than buffer size\n", __func__);
		return -EINVAL;
	}
#endif

	mutex_lock(&ts->io_mutex);
#ifdef I2C_INTERFACE
	if (dma_safe == false) {
		memcpy(ts->io_write_buf, data, len);
		data = ts->io_write_buf;
	}
#else

	ts->io_write_buf[0] = SEC_TS_SPI_SYNC_CODE;
	ts->io_write_buf[1] = (len >> 8) & 0xFF;
	ts->io_write_buf[2] = len & 0xFF;
	ts->io_write_buf[3] = 0x0;
	ts->io_write_buf[4] = 0x0;

	memcpy(ts->io_write_buf + SEC_TS_SPI_HEADER_SIZE, data, len);


	spi_len = SEC_TS_SPI_HEADER_SIZE + len;
	for (i = 0; i < spi_len; i++)
		checksum += ts->io_write_buf[i];

	ts->io_write_buf[spi_len] = checksum;
	spi_len += SEC_TS_SPI_CHECKSUM_SIZE;

	spi_message_init(&msg);

	/* add 3 zero stuffing tx bytes at last */
	memset(ts->io_write_buf + spi_len, 0x00, 3);
	spi_len = (spi_len + 3) & ~3;
	transfer[0].len = spi_len;
	transfer[0].tx_buf = ts->io_write_buf;
	transfer[0].rx_buf = NULL;
	spi_message_add_tail(&transfer[0], &msg);

#ifdef SEC_TS_DEBUG_IO
	input_info(true, &ts->client->dev, "%s:\n", __func__);
	for (i = 0; i < spi_len; i++)
		input_info(true, &ts->client->dev, "%X ", ts->io_write_buf[i]);
	input_info(true, &ts->client->dev, "\n");
#endif

#endif

	for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
#ifdef I2C_INTERFACE
		if ((ret = i2c_master_send(ts->client, data, len)) == len)
			break;
#else
		if ((ret = spi_sync(ts->client, &msg)) == 0)
			break;
#endif

		usleep_range(1 * 1000, 1 * 1000);

		if (retry > 1) {
			input_err(true, &ts->client->dev,
				  "%s: retry %d\n", __func__, retry + 1);
			ts->comm_err_count++;
		}
	}

	mutex_unlock(&ts->io_mutex);
	if (retry == SEC_TS_IO_RETRY_CNT) {
		input_err(true, &ts->client->dev,
			  "%s: write over retry limit\n", __func__);
		ret = -EIO;
	}

	return ret;
}

static int sec_ts_read_bulk_internal(struct sec_ts_data *ts,
					 u8 *data, int len, bool dma_safe)
{
	int ret;
	unsigned char retry;
	int remain = len;
#ifdef I2C_INTERFACE
	struct i2c_msg msg;
#else
	struct spi_message msg;
	struct spi_transfer transfer[1] = { { 0 } };
	unsigned int i;
	unsigned int spi_len = 0;
	unsigned char checksum = 0x0;
	int copy_size = 0, copy_cur = 0;
	int retry_msg = 0;
#endif

	if (len > sizeof(ts->io_read_buf) && dma_safe == false) {
		input_err(true, &ts->client->dev,
			  "%s: len %d over pre-allocated size %d\n", __func__,
			  len, sizeof(ts->io_read_buf));
		return -ENOSPC;
	}

	mutex_lock(&ts->io_mutex);

#ifdef I2C_INTERFACE
	msg.addr = ts->client->addr;
	msg.flags = I2C_M_RD;
	msg.len = len;
	if (dma_safe == false)
		msg.buf = ts->io_read_buf;
	else
		msg.buf = data;

	do {
		if (remain > ts->io_burstmax)
			msg.len = ts->io_burstmax;
		else
			msg.len = remain;

		remain -= ts->io_burstmax;

		for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
			ret = i2c_transfer(ts->client->adapter, &msg, 1);
			if (ret == 1)
				break;
			usleep_range(1 * 1000, 1 * 1000);

			if (retry > 1) {
				input_err(true, &ts->client->dev,
					  "%s: retry %d\n",
					  __func__, retry + 1);
				ts->comm_err_count++;
			}
		}

		if (retry == SEC_TS_IO_RETRY_CNT) {
			input_err(true, &ts->client->dev,
				  "%s: read over retry limit\n", __func__);
			ret = -EIO;
			break;
		}

		msg.buf += msg.len;

	} while (remain > 0);

	if (ret == 1 && dma_safe == false)
		memcpy(data, ts->io_read_buf, len);
#else
retry_message:
	remain = spi_len = (SEC_TS_SPI_READ_HEADER_SIZE + len +
			    SEC_TS_SPI_CHECKSUM_SIZE + 3) & ~3;
	do {
		if (remain > ts->io_burstmax)
			copy_cur = ts->io_burstmax;
		else
			copy_cur = remain;

		spi_message_init(&msg);
		transfer[0].len = copy_cur;
		transfer[0].tx_buf = NULL;
		transfer[0].rx_buf = &ts->io_read_buf[copy_size];
		/* CS needs to stay low until read seq. is done
		 */
		transfer[0].cs_change = (remain > ts->io_burstmax) ? 1 : 0;

		spi_message_add_tail(&transfer[0], &msg);

		copy_size += copy_cur;
		remain -= copy_cur;

		for (retry = 0; retry < SEC_TS_IO_RETRY_CNT; retry++) {
			ret = spi_sync(ts->client, &msg);
			if (ret == 0)
				break;

			usleep_range(1 * 1000, 1 * 1000);
			if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
				input_err(true, &ts->client->dev,
					  "%s: POWER_STATUS : OFF, retry:%d\n",
					  __func__, retry);
				mutex_unlock(&ts->io_mutex);
				goto err;
			}

			if (retry > 1) {
				input_err(true, &ts->client->dev,
					"%s: retry %d\n", __func__, retry + 1);
				ts->comm_err_count++;
			}
		}
	} while (remain > 0);

	for (i = 0, checksum = 0; i < SEC_TS_SPI_READ_HEADER_SIZE + len; i++)
		checksum += ts->io_read_buf[i];

	if (ret == 0 && ts->io_read_buf[0] == SEC_TS_SPI_SYNC_CODE &&
		checksum == ts->io_read_buf[SEC_TS_SPI_READ_HEADER_SIZE + len])
		memcpy(data, ts->io_read_buf + SEC_TS_SPI_READ_HEADER_SIZE,
		       len);
	else {
		input_info(true, &ts->client->dev,
			   "%s: spi fail, ret %d, sync code %X, reg(S) %X, chksum(M) %X, chksum(S) %X\n",
			__func__, ret, ts->io_read_buf[0], ts->io_read_buf[5],
			checksum,
			ts->io_read_buf[SEC_TS_SPI_READ_HEADER_SIZE + len]);
		if (retry_msg++ < SEC_TS_IO_RETRY_CNT)
			goto retry_message;
	}
#endif
	mutex_unlock(&ts->io_mutex);

#ifdef I2C_INTERFACE
	if (ret == 1)
#else
	if (ret == 0)
#endif
		return 0;
err:
	return -EIO;
}

/* Wrapper API for read and write */
int sec_ts_read(struct sec_ts_data *ts, u8 reg, u8 *data, int len)
{
	return sec_ts_read_internal(ts, reg, data, len, false);
}

int sec_ts_read_heap(struct sec_ts_data *ts, u8 reg, u8 *data, int len)
{
	return sec_ts_read_internal(ts, reg, data, len, true);
}

int sec_ts_write_burst(struct sec_ts_data *ts, u8 *data, int len)
{
	return sec_ts_write_burst_internal(ts, data, len, false);
}

int sec_ts_write_burst_heap(struct sec_ts_data *ts, u8 *data, int len)
{
	return sec_ts_write_burst_internal(ts, data, len, true);
}

int sec_ts_read_bulk(struct sec_ts_data *ts, u8 *data, int len)
{
	return sec_ts_read_bulk_internal(ts, data, len, false);
}

int sec_ts_read_bulk_heap(struct sec_ts_data *ts, u8 *data, int len)
{
	return sec_ts_read_bulk_internal(ts, data, len, true);
}

static int sec_ts_read_from_customlib(struct sec_ts_data *ts, u8 *data, int len)
{
	int ret;

	ret = sec_ts_write(ts, SEC_TS_CMD_CUSTOMLIB_READ_PARAM, data, 2);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			"%s: fail to read custom library command\n", __func__);

	ret = sec_ts_read(ts, SEC_TS_CMD_CUSTOMLIB_READ_PARAM, (u8 *)data, len);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			"%s: fail to read custom library command\n", __func__);

	return ret;
}

#if defined(CONFIG_TOUCHSCREEN_DUMP_MODE)
#include <linux/sec_debug.h>
extern struct tsp_dump_callbacks dump_callbacks;
static struct delayed_work *p_ghost_check;

static void sec_ts_check_rawdata(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
					      ghost_check.work);

	if (ts->tsp_dump_lock == 1) {
		input_err(true, &ts->client->dev,
			  "%s: ignored ## already checking..\n", __func__);
		return;
	}
	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		input_err(true, &ts->client->dev,
			  "%s: ignored ## IC is power off\n", __func__);
		return;
	}

	ts->tsp_dump_lock = 1;
	input_info(true, &ts->client->dev, "%s: start ##\n", __func__);
	sec_ts_run_rawdata_all((void *)ts, false);
	msleep(100);

	input_info(true, &ts->client->dev, "%s: done ##\n", __func__);
	ts->tsp_dump_lock = 0;

}

static void dump_tsp_log(void)
{
	pr_info("%s: %s %s: start\n", SEC_TS_NAME, SECLOG, __func__);

#ifdef CONFIG_BATTERY_SAMSUNG
	if (lpcharge == 1) {
		pr_err("%s: %s %s: ignored ## lpm charging Mode!!\n",
		       SEC_TS_NAME, SECLOG, __func__);
		return;
	}
#endif

	if (p_ghost_check == NULL) {
		pr_err("%s: %s %s: ignored ## tsp probe fail!!\n",
		       SEC_TS_NAME, SECLOG, __func__);
		return;
	}
	schedule_delayed_work(p_ghost_check, msecs_to_jiffies(100));
}
#endif


void sec_ts_delay(unsigned int ms)
{
	if (ms < 20)
		usleep_range(ms * 1000, ms * 1000);
	else
		msleep(ms);
}

int sec_ts_wait_for_ready(struct sec_ts_data *ts, unsigned int ack)
{
	return sec_ts_wait_for_ready_with_count(ts, ack,
						SEC_TS_WAIT_RETRY_CNT);
}

int sec_ts_wait_for_ready_with_count(struct sec_ts_data *ts, unsigned int ack,
				     unsigned int count)
{
	int rc = -1;
	int retry = 0;
	u8 tBuff[SEC_TS_EVENT_BUFF_SIZE] = {0,};

	while (retry < count) {
		if (sec_ts_read(ts, SEC_TS_READ_ONE_EVENT, tBuff,
			SEC_TS_EVENT_BUFF_SIZE) >= 0) {
			if (((tBuff[0] >> 2) & 0xF) == TYPE_STATUS_EVENT_INFO) {
				if (tBuff[1] == ack) {
					rc = 0;
					break;
				}
			} else if (((tBuff[0] >> 2) & 0xF) ==
				   TYPE_STATUS_EVENT_VENDOR_INFO) {
				if (tBuff[1] == ack) {
					rc = 0;
					break;
				}
			}
		}
		sec_ts_delay(20);
		retry++;
	}
	if (retry == count)
		input_err(true, &ts->client->dev, "%s: Time Over\n",
			__func__);

	input_dbg(true, &ts->client->dev,
		"%s: %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X [%d]\n",
		__func__, tBuff[0], tBuff[1], tBuff[2], tBuff[3],
		tBuff[4], tBuff[5], tBuff[6], tBuff[7], retry);

	return rc;
}

int sec_ts_read_calibration_report(struct sec_ts_data *ts)
{
	int ret;

	memset(ts->cali_report, 0, sizeof(ts->cali_report));
	ret = sec_ts_read(ts, SEC_TS_READ_CALIBRATION_REPORT,
		ts->cali_report, sizeof(ts->cali_report));
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: failed to read, %d\n", __func__, ret);
		return ret;
	}

	input_info(true, &ts->client->dev,
		"%s: count:%d, pass count:%d, fail count:%d, status:%X, param version:%X %X %X %X\n",
		__func__, ts->cali_report_try_cnt, ts->cali_report_pass_cnt,
		ts->cali_report_fail_cnt, ts->cali_report_status,
		ts->cali_report_param_ver[0], ts->cali_report_param_ver[1],
		ts->cali_report_param_ver[2], ts->cali_report_param_ver[3]);

	return ts->cali_report_status;
}

static void sec_ts_reinit(struct sec_ts_data *ts)
{
	u8 w_data[2] = {0x00, 0x00};
	int ret = 0;

	input_info(true, &ts->client->dev,
		"%s : charger=0x%x, Cover=0x%x, Power mode=0x%x\n",
		__func__, ts->charger_mode, ts->touch_functions,
		ts->lowpower_status);

	/* charger mode */
	if (ts->charger_mode != SEC_TS_BIT_CHARGER_MODE_NO) {
		w_data[0] = ts->charger_mode;
		ret = ts->sec_ts_write(ts, SET_TS_CMD_SET_CHARGER_MODE,
				       (u8 *)&w_data[0], 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: Failed to send command(0x%x)",
				__func__, SET_TS_CMD_SET_CHARGER_MODE);
	}

	/* Cover mode */
	if (ts->touch_functions & SEC_TS_BIT_SETFUNC_COVER) {
		w_data[0] = ts->cover_cmd;
		ret = sec_ts_write(ts, SEC_TS_CMD_SET_COVERTYPE,
				   (u8 *)&w_data[0], 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: Failed to send command(0x%x)",
				__func__, SEC_TS_CMD_SET_COVERTYPE);

		ret = sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
				   (u8 *)&(ts->touch_functions), 2);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: Failed to send command(0x%x)",
				__func__, SEC_TS_CMD_SET_TOUCHFUNCTION);
	}

	#ifdef SEC_TS_SUPPORT_CUSTOMLIB
	if (ts->use_customlib)
		sec_ts_set_custom_library(ts);
	#endif

	/* Power mode */
	if (ts->lowpower_status == TO_LOWPOWER_MODE) {
		w_data[0] = (ts->lowpower_mode &
			     SEC_TS_MODE_LOWPOWER_FLAG) >> 1;
		ret = sec_ts_write(ts, SEC_TS_CMD_WAKEUP_GESTURE_MODE,
				   (u8 *)&w_data[0], 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: Failed to send command(0x%x)",
				__func__, SEC_TS_CMD_WAKEUP_GESTURE_MODE);

		w_data[0] = TO_LOWPOWER_MODE;
		ret = sec_ts_write(ts, SEC_TS_CMD_SET_POWER_MODE,
				   (u8 *)&w_data[0], 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: Failed to send command(0x%x)",
				__func__, SEC_TS_CMD_SET_POWER_MODE);

		sec_ts_delay(50);

		if (ts->lowpower_mode & SEC_TS_MODE_CUSTOMLIB_AOD) {
			int i, ret;
			u8 data[10] = {0x02, 0};

			for (i = 0; i < 4; i++) {
				data[i * 2 + 2] = ts->rect_data[i] & 0xFF;
				data[i * 2 + 3] =
					(ts->rect_data[i] >> 8) & 0xFF;
			}

			ret = ts->sec_ts_write(ts,
					SEC_TS_CMD_CUSTOMLIB_WRITE_PARAM,
					&data[0], 10);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: Failed to write offset\n",
					__func__);

			ret = ts->sec_ts_write(ts,
				SEC_TS_CMD_CUSTOMLIB_NOTIFY_PACKET, NULL, 0);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: Failed to send notify\n",
					__func__);

		}

	} else {

		sec_ts_set_grip_type(ts, ONLY_EDGE_HANDLER);

		if (ts->dex_mode) {
			input_info(true, &ts->client->dev,
				   "%s: set dex mode\n", __func__);
			ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_DEX_MODE,
					       &ts->dex_mode, 1);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: failed to set dex mode %x\n",
					__func__, ts->dex_mode);
		}

		if (ts->brush_mode) {
			input_info(true, &ts->client->dev,
				   "%s: set brush mode\n", __func__);
			ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_BRUSH_MODE,
					       &ts->brush_mode, 1);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: failed to set brush mode\n",
					__func__);
		}

		if (ts->touchable_area) {
			input_info(true, &ts->client->dev,
				   "%s: set 16:9 mode\n", __func__);
			ret = ts->sec_ts_write(ts,
					SEC_TS_CMD_SET_TOUCHABLE_AREA,
					&ts->touchable_area, 1);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: failed to set 16:9 mode\n",
					__func__);
		}

	}
}

#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
/* Update a state machine used to toggle control of the touch IC's motion
 * filter.
 */
static void update_motion_filter(struct sec_ts_data *ts)
{
	/* Motion filter timeout, in milliseconds */
	const u32 mf_timeout_ms = 500;
	u8 next_state;
	/* Count the active touches */
	u8 touches = hweight32(ts->tid_touch_state);

	if (ts->use_default_mf)
		return;

	/* Determine the next filter state. The motion filter is enabled by
	 * default and it is disabled while a single finger is touching the
	 * screen. If another finger is touched down or if a timeout expires,
	 * the motion filter is reenabled and remains enabled until all fingers
	 * are lifted.
	 */
	next_state = ts->mf_state;
	switch (ts->mf_state) {
	case SEC_TS_MF_FILTERED:
		if (touches == 1) {
			next_state = SEC_TS_MF_UNFILTERED;
			ts->mf_downtime = ktime_get();
		}
		break;
	case SEC_TS_MF_UNFILTERED:
		if (touches == 0) {
			next_state = SEC_TS_MF_FILTERED;
		} else if (touches > 1 ||
			   ktime_after(ktime_get(),
				       ktime_add_ms(ts->mf_downtime,
						    mf_timeout_ms))) {
			next_state = SEC_TS_MF_FILTERED_LOCKED;
		}
		break;
	case SEC_TS_MF_FILTERED_LOCKED:
		if (touches == 0)
			next_state = SEC_TS_MF_FILTERED;
		break;
	}

	/* Send command to update filter state */
	if ((next_state == SEC_TS_MF_UNFILTERED) !=
	    (ts->mf_state == SEC_TS_MF_UNFILTERED)) {
		int ret;
		u8 para;

		pr_debug("%s: setting motion filter = %s.\n", __func__,
			 (next_state == SEC_TS_MF_UNFILTERED) ?
			 "false" : "true");
		para = (next_state == SEC_TS_MF_UNFILTERED) ? 0x01 : 0x00;
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_CONT_REPORT,
				       &para, 1);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
			 "%s: write reg %#x para %#x failed, returned %i\n",
			__func__, SEC_TS_CMD_SET_CONT_REPORT, para, ret);
		}
	}
	ts->mf_state = next_state;
}

static bool read_heatmap_raw(struct v4l2_heatmap *v4l2)
{
	struct sec_ts_data *ts = container_of(v4l2, struct sec_ts_data, v4l2);
	const struct sec_ts_plat_data *pdata = ts->plat_data;
	int result;
	int max_x = v4l2->format.width;
	int max_y = v4l2->format.height;

	if (ts->tsp_dump_lock == 1) {
		input_info(true, &ts->client->dev,
			"%s: drop this because raw data reading by others\n",
			__func__);
		return false;
	}

	if (pdata->heatmap_mode == HEATMAP_PARTIAL) {
		strength_t heatmap_value;
		int heatmap_x, heatmap_y;
		/* index for through the heatmap buffer read over the bus */
		unsigned int local_i;
		/* final position of the heatmap value in the full frame */
		unsigned int frame_i;
		unsigned int num_elements;
		u8 enable;
		struct heatmap_report report = {0};

		result = sec_ts_read(ts,
			SEC_TS_CMD_HEATMAP_ENABLE, &enable, 1);
		if (result < 0) {
			input_err(true, &ts->client->dev,
				 "%s: read reg %#x failed, returned %i\n",
				__func__, SEC_TS_CMD_HEATMAP_ENABLE, result);
			return false;
		}

		if (!enable) {
			enable = 1;
			result = sec_ts_write(ts,
				SEC_TS_CMD_HEATMAP_ENABLE, &enable, 1);
			if (result < 0)
				input_err(true, &ts->client->dev,
					"%s: enable local heatmap failed, returned %i\n",
					__func__, result);
			/*
			 * After local heatmap enabled, it takes `1/SCAN_RATE`
			 * time to make data ready. But, we don't want to wait
			 * here to cause overhead. Just drop this and wait for
			 * next reading.
			 */
			return false;
		}

		result = sec_ts_read(ts, SEC_TS_CMD_HEATMAP_READ,
			(uint8_t *) &report, sizeof(report));
		if (result < 0) {
			input_err(true, &ts->client->dev,
				 "%s: read failed, returned %i\n",
				__func__, result);
			return false;
		}

		num_elements = report.size_x * report.size_y;
		if (num_elements > LOCAL_HEATMAP_WIDTH * LOCAL_HEATMAP_HEIGHT) {
			input_err(true, &ts->client->dev,
				"Unexpected heatmap size: %i x %i",
				report.size_x, report.size_y);
				return false;
		}

		/*
		 * Set all to zero, will only write to non-zero locations
		 * in the loop.
		 */
		memset(v4l2->frame, 0, v4l2->format.sizeimage);
		/* populate the data buffer, rearranging into final locations */
		for (local_i = 0; local_i < num_elements; local_i++) {
			/* big-endian order raw data into heatmap data type */
			be16_to_cpus(&report.data[local_i]);
			heatmap_value = report.data[local_i];

			if (heatmap_value == 0) {
				/*
				 * Already initialized to zero. More
				 * importantly, samples around edges may go out
				 * of bounds.
				 * If their value is zero, this is ok.
				 */
				continue;
			}
			heatmap_x = report.offset_x + (local_i % report.size_x);
			heatmap_y = report.offset_y + (local_i / report.size_x);

			if (heatmap_x < 0 || heatmap_x >= max_x ||
				heatmap_y < 0 || heatmap_y >= max_y) {
				input_err(true, &ts->client->dev,
					"Invalid x or y: (%i, %i), value=%i, ending loop\n",
					heatmap_x, heatmap_y,
					heatmap_value);
					return false;
			}
			frame_i = heatmap_y * max_x + heatmap_x;
			v4l2->frame[frame_i] = heatmap_value;
		}
	} else if (pdata->heatmap_mode == HEATMAP_FULL) {
		int i, j, index = 0;
		int ret = 0;
		u8 type;

		if (!ts->heatmap_buff) {
			ts->heatmap_buff = kmalloc(
				sizeof(strength_t) * max_x * max_y, GFP_KERNEL);
			if (!ts->heatmap_buff) {
				input_err(true, &ts->client->dev,
				"%s: alloc heatmap_buff failed\n", __func__);
				return false;
			}
		}

		ret = sec_ts_read(ts,
			SEC_TS_CMD_MUTU_RAW_TYPE, &ts->ms_frame_type, 1);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				"%s: read rawdata type failed\n",
				__func__);
			return false;
		}

		/* Check raw type is TYPE_SIGNAL_DATA */
		if (ts->ms_frame_type != TYPE_SIGNAL_DATA) {
			input_info(true, &ts->client->dev,
				"%s: ms_frame_type change from %#x\n",
				__func__, ts->ms_frame_type);

			/* Check raw type is TYPE_INVALID_DATA */
			if (ts->ms_frame_type != TYPE_INVALID_DATA) {
				type = TYPE_INVALID_DATA;
				ret = sec_ts_write(ts,
					SEC_TS_CMD_MUTU_RAW_TYPE, &type, 1);
				if (ret < 0) {
					input_err(true, &ts->client->dev,
						"%s: recover rawdata type failed\n",
						__func__);
					return false;
				}
				ts->ms_frame_type = type;
			}

			/* Set raw type to TYPE_SIGNAL_DATA */
			type = TYPE_SIGNAL_DATA;
			ret = sec_ts_write(ts, SEC_TS_CMD_MUTU_RAW_TYPE,
				&type, 1);
			if (ret < 0) {
				input_err(true, &ts->client->dev,
					"%s: Set rawdata type failed\n",
					__func__);
				return false;
			}
			ts->ms_frame_type = type;

			/*
			 * If raw type change, need to wait 50 ms to read data
			 * back. But, we don't wanto to wait here to cause
			 * overhead. Just drop this and wait for next reading.
			 */
			return false;
		}

		ret = sec_ts_read_heap(ts, SEC_TS_READ_TOUCH_RAWDATA,
			(u8 *)ts->heatmap_buff,
			sizeof(strength_t) * max_x * max_y);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				"%s: Read delta frame failed\n", __func__);
			return false;
		}

		/* big-endian order raw data into heatmap data type */
		for (i = max_y - 1; i >= 0; i--)
			for (j = max_x - 1; j >= 0 ; j--)
				v4l2->frame[index++] = be16_to_cpup(
					ts->heatmap_buff + (j * max_y) + i);
	} else
		return false;

	return true;
}
#endif

#ifdef SEC_TS_SUPPORT_CUSTOMLIB
/* WARNING: touch_offload does not currently support the custom library
 * interface!
 * TODO: when custom library support is enabled, ensure that the output is
 * routed through touch_offload.
 */
static void sec_ts_handle_lib_status_event(struct sec_ts_data *ts,
				struct sec_ts_event_status *p_event_status)
{
	if ((p_event_status->stype == TYPE_STATUS_EVENT_CUSTOMLIB_INFO) &&
	    (p_event_status->status_id == SEC_TS_EVENT_CUSTOMLIB_FORCE_KEY)) {
		if (ts->power_status == SEC_TS_STATE_POWER_ON) {
			if (p_event_status->status_data_1 &
			    SEC_TS_CUSTOMLIB_EVENT_PRESSURE_TOUCHED) {
				ts->all_force_count++;
				ts->scrub_id =
					CUSTOMLIB_EVENT_TYPE_PRESSURE_TOUCHED;
			} else {
				if (ts->scrub_id ==
				    CUSTOMLIB_EVENT_TYPE_AOD_HOMEKEY_PRESS) {
					input_report_key(ts->input_dev,
							 KEY_HOMEPAGE,
							 0);
					ts->scrub_id =
				CUSTOMLIB_EVENT_TYPE_AOD_HOMEKEY_RELEASE;
				} else {
					ts->scrub_id =
					CUSTOMLIB_EVENT_TYPE_PRESSURE_RELEASED;
				}
			}

			input_report_key(ts->input_dev,
					 KEY_BLACK_UI_GESTURE, 1);
		} else {
			if (p_event_status->status_data_1 &
			    SEC_TS_CUSTOMLIB_EVENT_PRESSURE_RELEASED) {
				input_report_key(ts->input_dev,
						 KEY_HOMEPAGE, 0);
				input_report_key(ts->input_dev,
						 KEY_BLACK_UI_GESTURE, 1);
				ts->scrub_id =
				CUSTOMLIB_EVENT_TYPE_AOD_HOMEKEY_RLS_NO_HAPTIC;
				input_sync(ts->input_dev);
				haptic_homekey_release();
			} else {
				input_report_key(ts->input_dev,
						 KEY_HOMEPAGE, 1);
				input_sync(ts->input_dev);
				ts->scrub_id =
					CUSTOMLIB_EVENT_TYPE_AOD_HOMEKEY_PRESS;
				haptic_homekey_press();
				ts->all_force_count++;
			}
		}

		ts->scrub_x =
			((p_event_status->status_data_4 >> 4) & 0xF) << 8 |
			(p_event_status->status_data_3 & 0xFF);
		ts->scrub_y =
			((p_event_status->status_data_4 >> 0) & 0xF) << 8 |
			(p_event_status->status_data_2 & 0xFF);

		input_info(true, &ts->client->dev, "%s: PRESSURE[%d]\n",
			   __func__, ts->scrub_id);

		input_sync(ts->input_dev);
		input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 0);
	}
}
#endif

static void sec_ts_handle_coord_event(struct sec_ts_data *ts,
				struct sec_ts_event_coordinate *p_event_coord)
{
	u8 t_id;

	if (ts->input_closed) {
		input_err(true, &ts->client->dev, "%s: device is closed\n",
			__func__);
		return;
	}

	t_id = (p_event_coord->tid - 1);

	if (t_id < MAX_SUPPORT_TOUCH_COUNT + MAX_SUPPORT_HOVER_COUNT) {
		ts->coord[t_id].id = t_id;
		ts->coord[t_id].action = p_event_coord->tchsta;
		ts->coord[t_id].x = (p_event_coord->x_11_4 << 4) |
					(p_event_coord->x_3_0);
		ts->coord[t_id].y = (p_event_coord->y_11_4 << 4) |
					(p_event_coord->y_3_0);
		ts->coord[t_id].z = p_event_coord->z &
					SEC_TS_PRESSURE_MAX;
		ts->coord[t_id].ttype = p_event_coord->ttype_3_2 << 2 |
					p_event_coord->ttype_1_0 << 0;
		ts->coord[t_id].major = p_event_coord->major;
		ts->coord[t_id].minor = p_event_coord->minor;

		if (!ts->coord[t_id].palm &&
			(ts->coord[t_id].ttype == SEC_TS_TOUCHTYPE_PALM))
			ts->coord[t_id].palm_count++;

		ts->coord[t_id].palm =
			(ts->coord[t_id].ttype == SEC_TS_TOUCHTYPE_PALM);

		ts->coord[t_id].grip =
			(ts->coord[t_id].ttype == SEC_TS_TOUCHTYPE_GRIP);

		ts->coord[t_id].left_event = p_event_coord->left_event;

		if (ts->coord[t_id].z <= 0)
			ts->coord[t_id].z = 1;

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
		ts->offload.coords[t_id].x = ts->coord[t_id].x;
		ts->offload.coords[t_id].y = ts->coord[t_id].y;
		ts->offload.coords[t_id].major = ts->coord[t_id].major;
		ts->offload.coords[t_id].minor = ts->coord[t_id].minor;
		ts->offload.coords[t_id].pressure = ts->coord[t_id].z;
#endif

		if ((ts->coord[t_id].ttype ==
		     SEC_TS_TOUCHTYPE_NORMAL) ||
		    (ts->coord[t_id].ttype ==
		     SEC_TS_TOUCHTYPE_PALM) ||
		    (ts->coord[t_id].ttype ==
		     SEC_TS_TOUCHTYPE_GRIP) ||
		    (ts->coord[t_id].ttype ==
		     SEC_TS_TOUCHTYPE_WET) ||
		    (ts->coord[t_id].ttype ==
		     SEC_TS_TOUCHTYPE_GLOVE)) {

			if (ts->coord[t_id].action ==
				SEC_TS_COORDINATE_ACTION_RELEASE) {

				do_gettimeofday(&ts->time_released[t_id]);

				if (ts->time_longest <
					(ts->time_released[t_id].tv_sec -
						ts->time_pressed[t_id].tv_sec))
					ts->time_longest =
					(ts->time_released[t_id].tv_sec
					  - ts->time_pressed[t_id].tv_sec);

				if (ts->touch_count > 0)
					ts->touch_count--;
				if (ts->touch_count == 0 ||
					ts->tid_touch_state == 0) {
					ts->check_multi = 0;
				}
				__clear_bit(t_id, &ts->tid_palm_state);
				__clear_bit(t_id, &ts->tid_grip_state);
				__clear_bit(t_id, &ts->tid_touch_state);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
				ts->offload.coords[t_id].status =
					COORD_STATUS_INACTIVE;
				if (!ts->offload.offload_running) {
#endif
				input_mt_slot(ts->input_dev, t_id);
				if (ts->plat_data->support_mt_pressure)
					input_report_abs(ts->input_dev,
						ABS_MT_PRESSURE, 0);
				input_mt_report_slot_state(ts->input_dev,
					MT_TOOL_FINGER, 0);

				if (ts->touch_count == 0 ||
					ts->tid_touch_state == 0) {
					input_report_key(ts->input_dev,
						BTN_TOUCH, 0);
					input_report_key(ts->input_dev,
						BTN_TOOL_FINGER, 0);
				}
#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
				}
#endif
			} else if (ts->coord[t_id].action ==
					SEC_TS_COORDINATE_ACTION_PRESS) {
				do_gettimeofday(&ts->time_pressed[t_id]);

				ts->touch_count++;
				if ((ts->touch_count > 4) &&
					(ts->check_multi == 0)) {
					ts->check_multi = 1;
					ts->multi_count++;
				}
				ts->all_finger_count++;

				ts->max_z_value = max_t(unsigned int,
							ts->coord[t_id].z,
							ts->max_z_value);
				ts->min_z_value = min_t(unsigned int,
							ts->coord[t_id].z,
							ts->min_z_value);
				ts->sum_z_value +=
						(unsigned int)ts->coord[t_id].z;

				__set_bit(t_id, &ts->tid_touch_state);
				__clear_bit(t_id, &ts->tid_palm_state);
				__clear_bit(t_id, &ts->tid_grip_state);
				if (ts->coord[t_id].palm)
					__set_bit(t_id, &ts->tid_palm_state);
				else if (ts->coord[t_id].grip)
					__set_bit(t_id, &ts->tid_grip_state);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
				ts->offload.coords[t_id].status =
					COORD_STATUS_FINGER;
				if (!ts->offload.offload_running) {
#endif
				input_mt_slot(ts->input_dev, t_id);
				if (ts->coord[t_id].palm)
					input_mt_report_slot_state(
						ts->input_dev, MT_TOOL_PALM, 1);
				else if (ts->coord[t_id].grip)
					input_mt_report_slot_state(
						ts->input_dev, MT_TOOL_PALM, 1);
				else
					input_mt_report_slot_state(
						ts->input_dev,
						MT_TOOL_FINGER, 1);

				input_report_key(ts->input_dev, BTN_TOUCH, 1);
				input_report_key(ts->input_dev,
							BTN_TOOL_FINGER, 1);

				input_report_abs(ts->input_dev,
					ABS_MT_POSITION_X, ts->coord[t_id].x);
				input_report_abs(ts->input_dev,
					ABS_MT_POSITION_Y, ts->coord[t_id].y);
				input_report_abs(ts->input_dev,
						ABS_MT_TOUCH_MAJOR,
						ts->coord[t_id].major);
				input_report_abs(ts->input_dev,
						ABS_MT_TOUCH_MINOR,
						ts->coord[t_id].minor);
#ifdef ABS_MT_CUSTOM
				if (ts->brush_mode)
					input_report_abs(ts->input_dev,
						ABS_MT_CUSTOM,
						(ts->coord[t_id].z << 1) |
							ts->coord[t_id].palm);
				else
					input_report_abs(ts->input_dev,
						ABS_MT_CUSTOM,
						(BRUSH_Z_DATA << 1) |
							ts->coord[t_id].palm);
#endif
				if (ts->plat_data->support_mt_pressure)
					input_report_abs(ts->input_dev,
						ABS_MT_PRESSURE,
						ts->coord[t_id].z);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
				}
#endif
			} else if (ts->coord[t_id].action ==
					SEC_TS_COORDINATE_ACTION_MOVE) {

				ts->coord[t_id].mcount++;

#ifdef SW_GLOVE
				if ((ts->coord[t_id].ttype ==
					SEC_TS_TOUCHTYPE_GLOVE) &&
				    !ts->touchkey_glove_mode_status) {
					ts->touchkey_glove_mode_status = true;
				} else if ((ts->coord[t_id].ttype !=
						SEC_TS_TOUCHTYPE_GLOVE) &&
					   ts->touchkey_glove_mode_status) {
					ts->touchkey_glove_mode_status = false;
				}
#endif
				__set_bit(t_id, &ts->tid_touch_state);
				__clear_bit(t_id, &ts->tid_palm_state);
				__clear_bit(t_id, &ts->tid_grip_state);
				if (ts->coord[t_id].palm)
					__set_bit(t_id, &ts->tid_palm_state);
				else if (ts->coord[t_id].grip)
					__set_bit(t_id, &ts->tid_grip_state);
#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
				ts->offload.coords[t_id].status =
					COORD_STATUS_FINGER;
				if (!ts->offload.offload_running) {
#endif
#ifdef SW_GLOVE
				if ((ts->coord[t_id].ttype ==
					SEC_TS_TOUCHTYPE_GLOVE) &&
				    !ts->touchkey_glove_mode_status) {
					input_report_switch(ts->input_dev,
						SW_GLOVE, 1);
				} else if ((ts->coord[t_id].ttype !=
						SEC_TS_TOUCHTYPE_GLOVE) &&
					   ts->touchkey_glove_mode_status) {
					input_report_switch(ts->input_dev,
						SW_GLOVE, 0);
				}
#endif
				input_mt_slot(ts->input_dev, t_id);
				if (ts->coord[t_id].palm)
					input_mt_report_slot_state(
						ts->input_dev, MT_TOOL_PALM, 1);
				else if (ts->coord[t_id].grip)
					input_mt_report_slot_state(
						ts->input_dev, MT_TOOL_PALM, 1);
				else
					input_mt_report_slot_state(
						ts->input_dev,
						MT_TOOL_FINGER, 1);

				input_report_key(ts->input_dev, BTN_TOUCH, 1);
				input_report_key(ts->input_dev,
							BTN_TOOL_FINGER, 1);

				input_report_abs(ts->input_dev,
					ABS_MT_POSITION_X, ts->coord[t_id].x);
				input_report_abs(ts->input_dev,
					ABS_MT_POSITION_Y, ts->coord[t_id].y);
				input_report_abs(ts->input_dev,
						ABS_MT_TOUCH_MAJOR,
						ts->coord[t_id].major);
				input_report_abs(ts->input_dev,
						ABS_MT_TOUCH_MINOR,
						ts->coord[t_id].minor);
#ifdef ABS_MT_CUSTOM
				if (ts->brush_mode)
					input_report_abs(ts->input_dev,
						ABS_MT_CUSTOM,
						(ts->coord[t_id].z << 1) |
							ts->coord[t_id].palm);
				else
					input_report_abs(ts->input_dev,
						ABS_MT_CUSTOM,
						(BRUSH_Z_DATA << 1) |
							ts->coord[t_id].palm);
#endif
				if (ts->plat_data->support_mt_pressure)
					input_report_abs(ts->input_dev,
							ABS_MT_PRESSURE,
							ts->coord[t_id].z);
#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
				}
#endif
			} else
				input_dbg(true, &ts->client->dev,
					"%s: do not support coordinate action(%d)\n",
					__func__, ts->coord[t_id].action);
		} else
			input_dbg(true, &ts->client->dev,
				"%s: do not support coordinate type(%d)\n",
				__func__, ts->coord[t_id].ttype);
	} else
		input_err(true, &ts->client->dev,
				"%s: tid(%d) is out of range\n",
				__func__, t_id);
}

#ifdef SEC_TS_SUPPORT_CUSTOMLIB
static void sec_ts_handle_gesture_event(struct sec_ts_data *ts,
				struct sec_ts_gesture_status *p_gesture_status)
{
	if ((p_gesture_status->eid == 0x02) &&
	    (p_gesture_status->stype == 0x00)) {
		u8 customlib[3] = { 0 };

		ret = sec_ts_read_from_customlib(ts, customlib, 3);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: fail to read custom library data\n",
				__func__);

		input_info(true, &ts->client->dev,
			"%s: Custom Library, %x, %x, %x\n",
			__func__, customlib[0], customlib[1], customlib[2]);

		if (p_gesture_status->gesture_id == SEC_TS_GESTURE_CODE_SPAY ||
		    p_gesture_status->gesture_id ==
			SEC_TS_GESTURE_CODE_DOUBLE_TAP) {
			/* will be fixed to data structure */
			if (customlib[1] & SEC_TS_MODE_CUSTOMLIB_AOD) {
				u8 data[5] = { 0x0A, 0x00, 0x00, 0x00, 0x00 };

				ret = sec_ts_read_from_customlib(ts, data, 5);
				if (ret < 0)
					input_err(true, &ts->client->dev,
						"%s: fail to read custom library data\n",
						__func__);

				if (data[4] & SEC_TS_AOD_GESTURE_DOUBLETAB)
					ts->scrub_id =
					CUSTOMLIB_EVENT_TYPE_AOD_DOUBLETAB;

				ts->scrub_x = (data[1] & 0xFF) << 8 |
						(data[0] & 0xFF);
				ts->scrub_y = (data[3] & 0xFF) << 8 |
						(data[2] & 0xFF);
				input_info(true, &ts->client->dev,
					"%s: aod: %d\n",
					__func__, ts->scrub_id);
				ts->all_aod_tap_count++;
			}
			if (customlib[1] & SEC_TS_MODE_CUSTOMLIB_SPAY) {
				ts->scrub_id = CUSTOMLIB_EVENT_TYPE_SPAY;
				input_info(true, &ts->client->dev,
					"%s: SPAY: %d\n",
					__func__, ts->scrub_id);
				ts->all_spay_count++;
			}
			input_report_key(ts->input_dev,
					 KEY_BLACK_UI_GESTURE, 1);
			input_sync(ts->input_dev);
			input_report_key(ts->input_dev,
					 KEY_BLACK_UI_GESTURE, 0);
		}
	}
}
#endif

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)

static void sec_ts_populate_coordinate_channel(struct sec_ts_data *ts,
					struct touch_offload_frame *frame,
					int channel)
{
	int j;

	struct TouchOffloadDataCoord *dc =
		(struct TouchOffloadDataCoord *)frame->channel_data[channel];
	memset(dc, 0, frame->channel_data_size[channel]);
	dc->header.channel_type = TOUCH_DATA_TYPE_COORD;
	dc->header.channel_size = TOUCH_OFFLOAD_FRAME_SIZE_COORD;

	for (j = 0; j < MAX_COORDS; j++) {
		dc->coords[j].x = ts->offload.coords[j].x;
		dc->coords[j].y = ts->offload.coords[j].y;
		dc->coords[j].major = ts->offload.coords[j].major;
		dc->coords[j].minor = ts->offload.coords[j].minor;
		dc->coords[j].pressure = ts->offload.coords[j].pressure;
		dc->coords[j].status = ts->offload.coords[j].status;
	}
}

static void sec_ts_populate_mutual_channel(struct sec_ts_data *ts,
					struct touch_offload_frame *frame,
					int channel)
{
	uint32_t frame_index = 0;
	int32_t x, y;
	uint16_t heatmap_value;
	int ret = 0;
	u8 target_data_type, type;
	struct TouchOffloadData2d *mutual_strength =
		(struct TouchOffloadData2d *)frame->channel_data[channel];

	switch (frame->channel_type[channel] & ~TOUCH_SCAN_TYPE_MUTUAL) {
	case TOUCH_DATA_TYPE_RAW:
		target_data_type = TYPE_DECODED_DATA;
		break;
	case TOUCH_DATA_TYPE_FILTERED:
		target_data_type = TYPE_REMV_AMB_DATA;
		break;
	case TOUCH_DATA_TYPE_STRENGTH:
		target_data_type = TYPE_SIGNAL_DATA;
		break;
	case TOUCH_DATA_TYPE_BASELINE:
		target_data_type = TYPE_AMBIENT_DATA;
		break;
	}

	mutual_strength->tx_size = ts->tx_count;
	mutual_strength->rx_size = ts->rx_count;
	mutual_strength->header.channel_type = frame->channel_type[channel];
	mutual_strength->header.channel_size =
		TOUCH_OFFLOAD_FRAME_SIZE_2D(mutual_strength->rx_size,
					    mutual_strength->tx_size);

	ret = sec_ts_read(ts,
		SEC_TS_CMD_MUTU_RAW_TYPE, &ts->ms_frame_type, 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			"%s: read rawdata type failed\n",
			__func__);
		return;
	}

	/* Check raw type is correct */
	if (ts->ms_frame_type != target_data_type) {
		input_info(true, &ts->client->dev,
			"%s: ms_frame_type change from %#x\n",
			__func__, ts->ms_frame_type);

		/* Check raw type is TYPE_INVALID_DATA */
		if (ts->ms_frame_type != TYPE_INVALID_DATA) {
			type = TYPE_INVALID_DATA;
			ret = sec_ts_write(ts,
				SEC_TS_CMD_MUTU_RAW_TYPE, &type, 1);
			if (ret < 0) {
				input_err(true, &ts->client->dev,
					"%s: recover rawdata type failed\n",
					__func__);
				return;
			}
			ts->ms_frame_type = type;
		}

		/* Set the targeted data type */
		ret = sec_ts_write(ts, SEC_TS_CMD_MUTU_RAW_TYPE,
			&target_data_type, 1);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				"%s: Set rawdata type failed\n",
				__func__);
			return;
		}
		ts->ms_frame_type = target_data_type;

		/*
		 * If raw type change, need to wait 50 ms to read data
		 * back. But, we don't wanto to wait here to cause
		 * overhead. Just drop this and wait for next reading.
		 */

		return;
	}

	ret = sec_ts_read_heap(ts, SEC_TS_READ_TOUCH_RAWDATA,
		(u8 *)ts->heatmap_buff,
		mutual_strength->header.channel_size);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			"%s: Read mutual frame failed\n", __func__);
		return;
	}

	for (y = mutual_strength->rx_size - 1; y >= 0; y--) {
		for (x = mutual_strength->tx_size - 1; x >= 0; x--) {
			heatmap_value =
			    ts->heatmap_buff[x * mutual_strength->rx_size + y];
			((uint16_t *)
			 mutual_strength->data)[frame_index++] =
			    be16_to_cpu(heatmap_value);
		}
	}
}

static void sec_ts_populate_self_channel(struct sec_ts_data *ts,
					struct touch_offload_frame *frame,
					int channel)
{
	uint32_t frame_index = 0;
	int32_t x, y;
	uint16_t heatmap_value;
	int ret = 0;
	u8 target_data_type, type;
	struct TouchOffloadData1d *self_strength =
		(struct TouchOffloadData1d *)frame->channel_data[channel];

	switch (frame->channel_type[channel] & ~TOUCH_SCAN_TYPE_SELF) {
	case TOUCH_DATA_TYPE_RAW:
		target_data_type = TYPE_DECODED_DATA;
		break;
	case TOUCH_DATA_TYPE_FILTERED:
		target_data_type = TYPE_REMV_AMB_DATA;
		break;
	case TOUCH_DATA_TYPE_STRENGTH:
		target_data_type = TYPE_SIGNAL_DATA;
		break;
	case TOUCH_DATA_TYPE_BASELINE:
		target_data_type = TYPE_AMBIENT_DATA;
		break;
	}

	self_strength->tx_size = ts->tx_count;
	self_strength->rx_size = ts->rx_count;
	self_strength->header.channel_type = frame->channel_type[channel];
	self_strength->header.channel_size =
		TOUCH_OFFLOAD_FRAME_SIZE_1D(self_strength->rx_size,
					    self_strength->tx_size);

	ret = sec_ts_read(ts,
		SEC_TS_CMD_SELF_RAW_TYPE, &ts->ss_frame_type, 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			"%s: read rawdata type failed\n",
			__func__);
		return;
	}

	/* Check raw type is TYPE_SIGNAL_DATA */
	if (ts->ss_frame_type != target_data_type) {
		input_info(true, &ts->client->dev,
			"%s: ss_frame_type change from %#x\n",
			__func__, ts->ss_frame_type);

		/* Check raw type is TYPE_INVALID_DATA */
		if (ts->ss_frame_type != TYPE_INVALID_DATA) {
			type = TYPE_INVALID_DATA;
			ret = sec_ts_write(ts,
				SEC_TS_CMD_SELF_RAW_TYPE, &type, 1);
			if (ret < 0) {
				input_err(true, &ts->client->dev,
					"%s: recover rawdata type failed\n",
					__func__);
				return;
			}
			ts->ss_frame_type = type;
		}

		/* Set the targeted data type */
		ret = sec_ts_write(ts, SEC_TS_CMD_SELF_RAW_TYPE,
			&target_data_type, 1);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				"%s: Set rawdata type failed\n",
				__func__);
			return;
		}
		ts->ss_frame_type = target_data_type;

		/*
		 * If raw type change, need to wait 50 ms to read data
		 * back. But, we don't wanto to wait here to cause
		 * overhead. Just drop this and wait for next reading.
		 */

		return;
	}

	ret = sec_ts_read_heap(ts, SEC_TS_READ_TOUCH_SELF_RAWDATA,
		(u8 *)ts->heatmap_buff,
		self_strength->header.channel_size);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			"%s: Read self frame failed\n", __func__);
		return;
	}

	for (x = self_strength->tx_size - 1; x >= 0; x--) {
		heatmap_value = ts->heatmap_buff[x];
		((uint16_t *)
		 self_strength->data)[frame_index++] =
		    be16_to_cpu(heatmap_value);
	}
	for (y = self_strength->rx_size - 1; y >= 0; y--) {
		heatmap_value = ts->heatmap_buff[self_strength->tx_size + y];
		((uint16_t *)
		 self_strength->data)[frame_index++] =
		    be16_to_cpu(heatmap_value);
	}
}

static void sec_ts_populate_frame(struct sec_ts_data *ts,
				struct touch_offload_frame *frame)
{
	static u64 index;
	int i;

	frame->header.index = index++;
	frame->header.timestamp = ts->timestamp;

	if (!ts->heatmap_buff) {
		ts->heatmap_buff = kmalloc(
			ts->rx_count * ts->rx_count * 2, GFP_KERNEL);
	}

	/* Populate all channels */
	for (i = 0; i < frame->num_channels; i++) {
		if (frame->channel_type[i] == TOUCH_DATA_TYPE_COORD)
			sec_ts_populate_coordinate_channel(ts, frame, i);
		else if ((frame->channel_type[i] & TOUCH_SCAN_TYPE_MUTUAL) != 0)
			sec_ts_populate_mutual_channel(ts, frame, i);
		else if ((frame->channel_type[i] & TOUCH_SCAN_TYPE_SELF) != 0)
			sec_ts_populate_self_channel(ts, frame, i);
	}
}

int sec_ts_enable_grip(struct sec_ts_data *ts, bool enable)
{
	u8 value = enable ? 1 : 0;
	int ret;
	int final_result = 0;

	/* Set grip */
	ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_GRIP_DETEC, &value, 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			 "%s: SEC_TS_CMD_SET_GRIP_DETEC failed with ret=%d\n",
			__func__, ret);
		final_result = ret;
	}

	/* Set deadzone */
	value = enable ? 1 : 0;
	ret = ts->sec_ts_write(ts, SEC_TS_CMD_EDGE_DEADZONE, &value, 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			 "%s: SEC_TS_CMD_EDGE_DEADZONE failed with ret=%d\n",
			__func__, ret);
		final_result = ret;
	}

	return final_result;
}

static void sec_ts_offload_set_running(struct sec_ts_data *ts, bool running)
{
	if (ts->offload.offload_running != running) {
		ts->offload.offload_running = running;
		if (running) {
			pr_info("%s: disabling FW grip.\n", __func__);
			sec_ts_enable_grip(ts, false);
		} else {
			pr_info("%s: enabling FW grip.\n", __func__);
			sec_ts_enable_grip(ts, true);
		}
	}
}

#endif /* CONFIG_TOUCHSCREEN_OFFLOAD */

#define MAX_EVENT_COUNT 32
static void sec_ts_read_event(struct sec_ts_data *ts)
{
	int ret;
	u8 t_id;
	u8 event_id;
	u8 left_event_count;
	u8 read_event_buff[MAX_EVENT_COUNT][SEC_TS_EVENT_BUFF_SIZE] = { { 0 } };
	u8 *event_buff;
	struct sec_ts_gesture_status *p_gesture_status;
	struct sec_ts_event_status *p_event_status;
	int curr_pos;
	int remain_event_count = 0;
	bool processed_pointer_event = false;
	unsigned long last_tid_palm_state = ts->tid_palm_state;
	unsigned long last_tid_grip_state = ts->tid_grip_state;
#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	struct touch_offload_frame *frame = NULL;
#endif

	if (ts->power_status == SEC_TS_STATE_LPM) {

		pm_wakeup_event(&ts->client->dev, 3 * MSEC_PER_SEC);
		/* waiting for blsp block resuming, if not occurs error */
		ret = wait_for_completion_interruptible_timeout(
				&ts->resume_done,
				msecs_to_jiffies(3 * MSEC_PER_SEC));
		if (ret == 0) {
			input_err(true, &ts->client->dev,
				  "%s: LPM: pm resume is not handled\n",
				  __func__);
			return;
		}

		if (ret < 0) {
			input_err(true, &ts->client->dev,
				  "%s: LPM: -ERESTARTSYS if interrupted, %d\n",
				  __func__, ret);
			return;
		}

		input_info(true, &ts->client->dev,
			"%s: run LPM interrupt handler, %d\n", __func__, ret);
		/* run lpm interrupt handler */
	}

	ret = t_id = event_id = curr_pos = remain_event_count = 0;
	/* repeat READ_ONE_EVENT until buffer is empty(No event) */
	ret = sec_ts_read(ts, SEC_TS_READ_ONE_EVENT,
			  (u8 *)read_event_buff[0], SEC_TS_EVENT_BUFF_SIZE);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: read one event failed\n", __func__);
		return;
	}

	if (ts->temp == 0x01)
		input_info(true, &ts->client->dev,
			"ONE: %02X %02X %02X %02X %02X %02X %02X %02X\n",
			read_event_buff[0][0], read_event_buff[0][1],
			read_event_buff[0][2], read_event_buff[0][3],
			read_event_buff[0][4], read_event_buff[0][5],
			read_event_buff[0][6], read_event_buff[0][7]);

	if (read_event_buff[0][0] == 0) {
		input_info(true, &ts->client->dev,
			"%s: event buffer is empty\n", __func__);
		return;
	}

	left_event_count = read_event_buff[0][7] & 0x3F;
	remain_event_count = left_event_count;

	if (left_event_count > MAX_EVENT_COUNT - 1 ||
		left_event_count == 0xFF) {
		input_err(true, &ts->client->dev,
			"%s: event buffer overflow %d\n",
			__func__, left_event_count);

		/* write clear event stack command
		 * when read_event_count > MAX_EVENT_COUNT
		 **/
		ret = sec_ts_write(ts, SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: write clear event failed\n", __func__);
		return;
	}

	if (left_event_count > 0) {
		ret = sec_ts_read(ts, SEC_TS_READ_ALL_EVENT,
			(u8 *)read_event_buff[1],
			sizeof(u8) * (SEC_TS_EVENT_BUFF_SIZE) *
				(left_event_count));
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				  "%s: read one event failed\n", __func__);
			return;
		}
	}

	do {
		s16 max_force_p = 0;

		event_buff = read_event_buff[curr_pos];
		event_id = event_buff[0] & 0x3;

		if (ts->temp == 0x01)
			input_info(true, &ts->client->dev,
				 "ALL: %02X %02X %02X %02X %02X %02X %02X %02X\n",
				event_buff[0], event_buff[1], event_buff[2],
				event_buff[3], event_buff[4], event_buff[5],
				event_buff[6], event_buff[7]);

		switch (event_id) {
		case SEC_TS_STATUS_EVENT:
			p_event_status =
				(struct sec_ts_event_status *)event_buff;

			/* tchsta == 0 && ttype == 0 && eid == 0 : buffer empty
			 **/
			if (p_event_status->stype > 0) {
				/* Demote 'vendor' messages */
				if (p_event_status->stype ==
					TYPE_STATUS_EVENT_VENDOR_INFO) {
					u8 status_id =
						p_event_status->status_id;
					u8 status_data_1 =
						p_event_status->status_data_1;

					input_dbg(true, &ts->client->dev,
						"%s: STATUS %x %x %x %x %x %x %x %x\n",
						__func__, event_buff[0],
						event_buff[1], event_buff[2],
						event_buff[3], event_buff[4],
						event_buff[5], event_buff[6],
						event_buff[7]);

					switch (status_id) {
					case SEC_TS_EVENT_STATUS_ID_WLC:
						input_info(true,
							&ts->client->dev,
							"STATUS: wlc mode change to %x\n",
							status_data_1);
						break;

					case SEC_TS_EVENT_STATUS_ID_NOISE:
						input_info(true,
							&ts->client->dev,
							"STATUS: noise mode change to %x\n",
							status_data_1);
						break;

					case SEC_TS_EVENT_STATUS_ID_GRIP:
						input_info(true,
							&ts->client->dev,
							"STATUS: detect grip %s!\n",
							(status_data_1) ?
							"enter" : "leave");
						break;

					case SEC_TS_EVENT_STATUS_ID_PALM:
						input_info(true,
							&ts->client->dev,
							"STATUS: detect palm!\n");
						break;

					default:
						break;
					}
				} else
					input_info(true, &ts->client->dev,
						"%s: STATUS %x %x %x %x %x %x %x %x\n",
						__func__, event_buff[0],
						event_buff[1], event_buff[2],
						event_buff[3], event_buff[4],
						event_buff[5], event_buff[6],
						event_buff[7]);
			}

			if ((p_event_status->stype ==
					TYPE_STATUS_EVENT_INFO) &&
				(p_event_status->status_id ==
					SEC_TS_ACK_BOOT_COMPLETE)) {
				u8 status_data_1 =
					p_event_status->status_data_1;

				switch (status_data_1) {
				case 0x20:
					/* watchdog reset !? */
					sec_ts_unlocked_release_all_finger(ts);
					ret = sec_ts_write(ts,
						SEC_TS_CMD_SENSE_ON, NULL, 0);
					if (ret < 0)
						input_err(true,
							&ts->client->dev,
							"%s: fail to write Sense_on\n",
							__func__);
						sec_ts_reinit(ts);
					break;
				case 0x40:
					input_info(true, &ts->client->dev,
						"%s: sw_reset done\n",
						__func__);
					sec_ts_unlocked_release_all_finger(ts);
					complete_all(&ts->boot_completed);
					break;
				case 0x10:
					input_info(true, &ts->client->dev,
						"%s: hw_reset done\n",
						__func__);
					sec_ts_unlocked_release_all_finger(ts);
					complete_all(&ts->boot_completed);
					break;
				default:
					break;
				}

			}

			/* event queue full-> all finger release */
			if ((p_event_status->stype == TYPE_STATUS_EVENT_ERR) &&
				(p_event_status->status_id ==
					SEC_TS_ERR_EVENT_QUEUE_FULL)) {
				input_err(true, &ts->client->dev,
					"%s: IC Event Queue is full\n",
					__func__);
				sec_ts_unlocked_release_all_finger(ts);
			}

			if ((p_event_status->stype ==
				TYPE_STATUS_EVENT_ERR) &&
			    (p_event_status->status_id ==
				SEC_TS_ERR_EVENT_ESD)) {
				input_err(true, &ts->client->dev,
					  "%s: ESD detected. run reset\n",
					  __func__);
#ifdef USE_RESET_DURING_POWER_ON
				schedule_work(&ts->reset_work.work);
#endif
			}

			if ((p_event_status->stype ==
				TYPE_STATUS_EVENT_INFO) &&
			    (p_event_status->status_id ==
				SEC_TS_ACK_WET_MODE)) {
				ts->wet_mode = p_event_status->status_data_1;
				input_info(true, &ts->client->dev,
					"%s: water wet mode %d\n",
					__func__, ts->wet_mode);
				if (ts->wet_mode)
					ts->wet_count++;

				}

#ifdef SEC_TS_SUPPORT_CUSTOMLIB
			sec_ts_handle_lib_status_event(ts, p_event_status);
#endif
			break;

		case SEC_TS_COORDINATE_EVENT:
			processed_pointer_event = true;
			sec_ts_handle_coord_event(ts,
				(struct sec_ts_event_coordinate *)event_buff);
			break;

		case SEC_TS_GESTURE_EVENT:
			p_gesture_status =
				(struct sec_ts_gesture_status *)event_buff;
#ifdef SEC_TS_SUPPORT_CUSTOMLIB
			sec_ts_handle_gesture_event(ts, p_gesture_status);
#endif
			break;

		default:
			input_err(true, &ts->client->dev,
				"%s: unknown event %x %x %x %x %x %x\n",
				__func__,
				event_buff[0], event_buff[1], event_buff[2],
				event_buff[3], event_buff[4], event_buff[5]);
			break;
		}

		if (t_id < MAX_SUPPORT_TOUCH_COUNT + MAX_SUPPORT_HOVER_COUNT) {
			if (ts->coord[t_id].action ==
			    SEC_TS_COORDINATE_ACTION_PRESS) {
				input_dbg(false, &ts->client->dev,
					"%s[P] tID:%d x:%d y:%d z:%d major:%d minor:%d tc:%d type:%X\n",
					ts->dex_name,
					t_id, ts->coord[t_id].x,
					ts->coord[t_id].y, ts->coord[t_id].z,
					ts->coord[t_id].major,
					ts->coord[t_id].minor,
					ts->touch_count,
					ts->coord[t_id].ttype);

			} else if (ts->coord[t_id].action ==
				   SEC_TS_COORDINATE_ACTION_RELEASE) {
				input_dbg(false, &ts->client->dev,
					"%s[R] tID:%d mc:%d tc:%d lx:%d ly:%d f:%d v:%02X%02X cal:%02X(%02X) id(%d,%d) p:%d P%02XT%04X\n",
					ts->dex_name,
					t_id, ts->coord[t_id].mcount,
					ts->touch_count,
					ts->coord[t_id].x, ts->coord[t_id].y,
					max_force_p,
					ts->plat_data->img_version_of_ic[2],
					ts->plat_data->img_version_of_ic[3],
					ts->cal_status, ts->nv, ts->tspid_val,
					ts->tspicid_val,
					ts->coord[t_id].palm_count,
					ts->cal_count, ts->tune_fix_ver);

				ts->coord[t_id].action =
						SEC_TS_COORDINATE_ACTION_NONE;
				ts->coord[t_id].mcount = 0;
				ts->coord[t_id].palm_count = 0;
				max_force_p = 0;
			}
		}

		curr_pos++;
		remain_event_count--;
	} while (remain_event_count >= 0);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	if (!ts->offload.offload_running) {
#endif

	input_sync(ts->input_dev);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	}

	if (processed_pointer_event) {
		ret = touch_offload_reserve_frame(&ts->offload, &frame);
		if (ret != 0) {
			input_dbg(true, &ts->client->dev,
				  "Could not reserve a frame: ret=%d.\n", ret);

			/* Stop offload when there are no buffers available */
			sec_ts_offload_set_running(ts, false);
		} else {
			sec_ts_offload_set_running(ts, true);

			sec_ts_populate_frame(ts, frame);

			ret = touch_offload_queue_frame(&ts->offload, frame);
			if (ret != 0) {
				pr_err("%s: Failed to queue reserved frame: ret=%d.\n",
				       __func__, ret);
			}
		}
	}
#endif

	/* TODO: If the mutual strength heatmap was already read into the touch
	 * offload interface, use it here instead of reading again.
	 */
#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
	if (processed_pointer_event) {
		heatmap_read(&ts->v4l2, ktime_to_ns(ts->timestamp));

		/* palm */
		if (last_tid_palm_state == 0 &&
			ts->tid_palm_state >= 1) {
			input_info(true, &ts->client->dev,
				"COORD: detect palm enter(tid 0x0 -> %#x)\n",
				ts->tid_palm_state);
		}
		if (last_tid_palm_state >= 1 &&
			ts->tid_palm_state == 0) {
			input_info(true, &ts->client->dev,
				"COORD: detect palm leave(tid %#x -> 0x0), tid_touch %#x\n",
				last_tid_palm_state, ts->tid_touch_state);
			if (ts->touch_count || ts->tid_touch_state) {
				ts->palms_leaved_once = true;
				input_dbg(true, &ts->client->dev,
					"COORD: wait all finger(s) release after palm entered\n");
			}
		}
		/* grip */
		if (last_tid_grip_state == 0 &&
			ts->tid_grip_state >= 1) {
			input_info(true, &ts->client->dev,
				"COORD: detect grip enter(tid 0x0 -> %#x)\n",
				ts->tid_grip_state);
		}
		if (last_tid_grip_state >= 1 &&
			ts->tid_grip_state == 0) {
			input_info(true, &ts->client->dev,
				"COORD: detect grip leave(tid %#x -> 0x0), tid_touch %#x\n",
				last_tid_grip_state, ts->tid_touch_state);
			if (ts->touch_count || ts->tid_touch_state) {
				ts->grips_leaved_once = true;
				input_dbg(true, &ts->client->dev,
					"COORD: wait all finger(s) release after grip entered\n");
			}
		}
		if ((ts->touch_count == 0 || ts->tid_touch_state == 0) &&
			(ts->palms_leaved_once || ts->grips_leaved_once)) {
			ts->palms_leaved_once = false;
			ts->grips_leaved_once = false;
			input_info(true, &ts->client->dev,
				"COORD: all fingers released with palm(s)/grip(s) leaved once\n");
		}
	}
#endif
}

static irqreturn_t sec_ts_isr(int irq, void *handle)
{
	struct sec_ts_data *ts = (struct sec_ts_data *)handle;

	ts->timestamp = ktime_get();
#if !IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	input_set_timestamp(ts->input_dev, ts->timestamp);
#endif

	return IRQ_WAKE_THREAD;
}

static irqreturn_t sec_ts_irq_thread(int irq, void *ptr)
{
	struct sec_ts_data *ts = (struct sec_ts_data *)ptr;

	if (sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_IRQ, true) < 0) {
		/* Interrupt during bus suspend */
		input_info(true, &ts->client->dev,
			"%s: Skipping stray interrupt since bus is suspended(power_status: %d)\n",
			__func__, ts->power_status);
		return IRQ_HANDLED;
	}

	/* prevent CPU from entering deep sleep */
	pm_qos_update_request(&ts->pm_touch_req, 100);
	pm_qos_update_request(&ts->pm_spi_req, 100);
	pm_wakeup_event(&ts->client->dev, MSEC_PER_SEC);

	mutex_lock(&ts->eventlock);

	sec_ts_read_event(ts);

	mutex_unlock(&ts->eventlock);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
	/* Disable the firmware motion filter during single touch */
	update_motion_filter(ts);
#endif

	pm_qos_update_request(&ts->pm_spi_req, PM_QOS_DEFAULT_VALUE);
	pm_qos_update_request(&ts->pm_touch_req, PM_QOS_DEFAULT_VALUE);

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_IRQ, false);

	return IRQ_HANDLED;
}

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
static void sec_ts_offload_report(void *handle,
				  struct TouchOffloadIocReport *report)
{
	struct sec_ts_data *ts = (struct sec_ts_data *)handle;
	bool touch_down = 0;
	int i;

	input_set_timestamp(ts->input_dev, report->timestamp);

	for (i = 0; i < MAX_COORDS; i++) {
		if (report->coords[i].status == COORD_STATUS_FINGER) {
			input_mt_slot(ts->input_dev, i);
			touch_down = 1;
			input_report_key(ts->input_dev, BTN_TOUCH,
					 touch_down);
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, 1);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					 report->coords[i].x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					 report->coords[i].y);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					 report->coords[i].major);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR,
					 report->coords[i].minor);
			if (ts->plat_data->support_mt_pressure)
				input_report_abs(ts->input_dev,
					ABS_MT_PRESSURE,
					report->coords[i].pressure);
		} else {
			input_mt_slot(ts->input_dev, i);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, 0);
			input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID,
					 -1);
		}
	}

	input_report_key(ts->input_dev, BTN_TOUCH, touch_down);

	input_sync(ts->input_dev);
}
#endif /* CONFIG_TOUCHSCREEN_OFFLOAD */

int get_tsp_status(void)
{
	return 0;
}
EXPORT_SYMBOL(get_tsp_status);

int sec_ts_glove_mode_enables(struct sec_ts_data *ts, int mode)
{
	int ret;

	if (mode)
		ts->touch_functions = (ts->touch_functions |
				       SEC_TS_BIT_SETFUNC_GLOVE |
				       SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC);
	else
		ts->touch_functions = ((ts->touch_functions &
					(~SEC_TS_BIT_SETFUNC_GLOVE)) |
				       SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		input_err(true, &ts->client->dev,
			"%s: pwr off, glove:%d, status:%x\n", __func__,
			mode, ts->touch_functions);
		goto glove_enable_err;
	}

	ret = sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
			   (u8 *)&ts->touch_functions, 2);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: Failed to send command", __func__);
		goto glove_enable_err;
	}

	input_info(true, &ts->client->dev,
		"%s: glove:%d, status:%x\n", __func__,
		mode, ts->touch_functions);

	return 0;

glove_enable_err:
	return -EIO;
}
EXPORT_SYMBOL(sec_ts_glove_mode_enables);

int sec_ts_set_cover_type(struct sec_ts_data *ts, bool enable)
{
	int ret;

	input_info(true, &ts->client->dev, "%s: %d\n",
		   __func__, ts->cover_type);


	switch (ts->cover_type) {
	case SEC_TS_VIEW_WIRELESS:
	case SEC_TS_VIEW_COVER:
	case SEC_TS_VIEW_WALLET:
	case SEC_TS_FLIP_WALLET:
	case SEC_TS_LED_COVER:
	case SEC_TS_MONTBLANC_COVER:
	case SEC_TS_CLEAR_FLIP_COVER:
	case SEC_TS_QWERTY_KEYBOARD_EUR:
	case SEC_TS_QWERTY_KEYBOARD_KOR:
		ts->cover_cmd = (u8)ts->cover_type;
		break;
	case SEC_TS_CHARGER_COVER:
	case SEC_TS_COVER_NOTHING1:
	case SEC_TS_COVER_NOTHING2:
	default:
		ts->cover_cmd = 0;
		input_err(true, &ts->client->dev,
			 "%s: not chage touch state, %d\n",
			__func__, ts->cover_type);
		break;
	}

	if (enable)
		ts->touch_functions = (ts->touch_functions |
				       SEC_TS_BIT_SETFUNC_COVER |
				       SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC);
	else
		ts->touch_functions = ((ts->touch_functions &
					(~SEC_TS_BIT_SETFUNC_COVER)) |
				       SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		input_err(true, &ts->client->dev,
			  "%s: pwr off, close:%d, status:%x\n", __func__,
			enable, ts->touch_functions);
		goto cover_enable_err;
	}

	if (enable) {
		ret = sec_ts_write(ts, SEC_TS_CMD_SET_COVERTYPE,
				   &ts->cover_cmd, 1);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				  "%s: Failed to send covertype command: %d",
				  __func__, ts->cover_cmd);
			goto cover_enable_err;
		}
	}

	ret = sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
			   (u8 *)&(ts->touch_functions), 2);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: Failed to send command", __func__);
		goto cover_enable_err;
	}

	input_info(true, &ts->client->dev,
		"%s: close:%d, status:%x\n", __func__,
		enable, ts->touch_functions);

	return 0;

cover_enable_err:
	return -EIO;


}
EXPORT_SYMBOL(sec_ts_set_cover_type);

void sec_ts_set_grip_type(struct sec_ts_data *ts, u8 set_type)
{
	u8 mode = G_NONE;

	input_dbg(true, &ts->client->dev,
		"%s: re-init grip(%d), edh:%d, edg:%d, lan:%d\n", __func__,
		set_type, ts->grip_edgehandler_direction, ts->grip_edge_range,
		ts->grip_landscape_mode);

	/* edge handler */
	if (ts->grip_edgehandler_direction != 0)
		mode |= G_SET_EDGE_HANDLER;

	if (set_type == GRIP_ALL_DATA) {
		/* edge */
		if (ts->grip_edge_range != 60)
			mode |= G_SET_EDGE_ZONE;

		/* dead zone */
		if (ts->grip_landscape_mode == 1)	/* default 0 mode, 32 */
			mode |= G_SET_LANDSCAPE_MODE;
		else
			mode |= G_SET_NORMAL_MODE;
	}

	if (mode)
		set_grip_data_to_ic(ts, mode);

}

/* for debugging--------------------------------------------------------------*/

static int sec_ts_pinctrl_configure(struct sec_ts_data *ts, bool enable)
{
	struct pinctrl_state *state;

	input_info(true, &ts->client->dev, "%s: %s\n",
		   __func__, enable ? "ACTIVE" : "SUSPEND");

	if (enable) {
		state = pinctrl_lookup_state(ts->plat_data->pinctrl,
					     "on_state");
		if (IS_ERR(ts->plat_data->pinctrl))
			input_err(true, &ts->client->dev,
				"%s: could not get active pinstate\n",
				__func__);
	} else {
		state = pinctrl_lookup_state(ts->plat_data->pinctrl,
					     "off_state");
		if (IS_ERR(ts->plat_data->pinctrl))
			input_err(true, &ts->client->dev,
				"%s: could not get suspend pinstate\n",
				__func__);
	}

	if (!IS_ERR_OR_NULL(state))
		return pinctrl_select_state(ts->plat_data->pinctrl, state);

	return 0;

}

static int sec_ts_power(void *data, bool on)
{
	struct sec_ts_data *ts = (struct sec_ts_data *)data;
	const struct sec_ts_plat_data *pdata = ts->plat_data;
	struct regulator *regulator_dvdd = NULL;
	struct regulator *regulator_avdd = NULL;
	static bool dvdd_enabled, avdd_enabled;
	int ret = 0;

	if (pdata->regulator_dvdd) {
		regulator_dvdd = regulator_get(&ts->client->dev,
			pdata->regulator_dvdd);
		if (IS_ERR_OR_NULL(regulator_dvdd))
			input_err(true, &ts->client->dev,
				"%s: Failed to get %s regulator.\n",
				__func__, pdata->regulator_dvdd);
	}

	if (pdata->regulator_avdd) {
		regulator_avdd = regulator_get(&ts->client->dev,
			pdata->regulator_avdd);
		if (IS_ERR_OR_NULL(regulator_avdd))
			input_err(true, &ts->client->dev,
				"%s: Failed to get %s regulator.\n",
				 __func__, pdata->regulator_avdd);
	}

	if (regulator_dvdd && (dvdd_enabled != on)) {
		ret = (on) ? regulator_enable(regulator_dvdd) :
			regulator_disable(regulator_dvdd);
		if (ret)
			input_err(true, &ts->client->dev,
				"%s: Failed to control dvdd: %d\n",
				__func__, ret);
		else {
			sec_ts_delay(1);
			dvdd_enabled = on;
		}
	}

	if (regulator_avdd && (avdd_enabled != on)) {
		ret = (on) ? regulator_enable(regulator_avdd) :
			regulator_disable(regulator_avdd);
		if (ret)
			input_err(true, &ts->client->dev,
				"%s: Failed to control avdd: %d\n",
				__func__, ret);
		else
			avdd_enabled = on;
	}

	if (regulator_dvdd) {
		input_info(true, &ts->client->dev, "%s: %s: dvdd:%s\n",
			__func__, on ? "on" : "off",
			regulator_is_enabled(regulator_dvdd) ? "on" : "off");
		regulator_put(regulator_dvdd);
	}

	if (regulator_avdd) {
		input_info(true, &ts->client->dev, "%s: %s: avdd:%s\n",
			__func__, on ? "on" : "off",
			regulator_is_enabled(regulator_avdd) ? "on" : "off");
		regulator_put(regulator_avdd);
	}

	return ret;
}

#ifdef I2C_INTERFACE
static int sec_ts_parse_dt(struct i2c_client *client)
#else
static int sec_ts_parse_dt(struct spi_device *client)
#endif
{
	struct device *dev = &client->dev;
	struct sec_ts_plat_data *pdata = dev->platform_data;
	struct device_node *np = dev->of_node;
	u32 coords[2];
	int ret = 0;
	int count = 0;
	u32 ic_match_value;
	int lcdtype = 0;
#if defined(CONFIG_EXYNOS_DECON_FB)
	int connected;
#endif
	int index;
	struct of_phandle_args panelmap;
	struct drm_panel *panel = NULL;

	if (of_property_read_bool(np, "sec,panel_map")) {
		for (index = 0 ;; index++) {
			ret = of_parse_phandle_with_fixed_args(np,
					"sec,panel_map",
					1,
					index,
					&panelmap);
			if (ret)
				return -EPROBE_DEFER;
			panel = of_drm_find_panel(panelmap.np);
			of_node_put(panelmap.np);
			if (!IS_ERR_OR_NULL(panel)) {
				pdata->panel = panel;
				pdata->initial_panel_index = panelmap.args[0];
				break;
			}
		}
	}

	pdata->tsp_icid = of_get_named_gpio(np, "sec,tsp-icid_gpio", 0);
	if (gpio_is_valid(pdata->tsp_icid)) {
		input_info(true, dev, "%s: TSP_ICID : %d\n",
			   __func__, gpio_get_value(pdata->tsp_icid));
		if (of_property_read_u32(np, "sec,icid_match_value",
					 &ic_match_value)) {
			input_err(true, dev,
				"%s: Failed to get icid match value\n",
				__func__);
			return -EINVAL;
		}

		if (gpio_get_value(pdata->tsp_icid) != ic_match_value) {
			input_err(true, dev,
				  "%s: Do not match TSP_ICID\n", __func__);
			return -EINVAL;
		}
	} else {
		input_err(true, dev,
			  "%s: Failed to get tsp-icid gpio\n", __func__);
	}

	pdata->tsp_vsync = of_get_named_gpio(np, "sec,tsp_vsync_gpio", 0);
	if (gpio_is_valid(pdata->tsp_vsync))
		input_info(true, &client->dev, "%s: vsync %s\n", __func__,
			gpio_get_value(pdata->tsp_vsync) ?
				"disable" : "enable");

	pdata->irq_gpio = of_get_named_gpio(np, "sec,irq_gpio", 0);
	if (gpio_is_valid(pdata->irq_gpio)) {
		ret = gpio_request_one(pdata->irq_gpio, GPIOF_DIR_IN,
				       "sec,tsp_int");
		if (ret) {
			input_err(true, &client->dev,
				  "%s: Unable to request tsp_int [%d]\n",
				  __func__, pdata->irq_gpio);
			return -EINVAL;
		}
	} else {
		input_err(true, &client->dev,
			  "%s: Failed to get irq gpio\n", __func__);
		return -EINVAL;
	}

	client->irq = gpio_to_irq(pdata->irq_gpio);

	if (of_property_read_u32(np, "sec,irq_type", &pdata->irq_type)) {
		input_err(true, dev,
			  "%s: Failed to get irq_type property\n", __func__);
		pdata->irq_type = IRQF_TRIGGER_LOW | IRQF_ONESHOT;
	}

	if (of_property_read_u32(np, "sec,i2c-burstmax", &pdata->io_burstmax)) {
		input_dbg(false, &client->dev,
			  "%s: Failed to get io_burstmax property\n", __func__);
		pdata->io_burstmax = 1024; //TODO: check this
	}
	if (pdata->io_burstmax > IO_PREALLOC_READ_BUF_SZ ||
	    pdata->io_burstmax > IO_PREALLOC_WRITE_BUF_SZ) {
		input_err(true, &client->dev,
			  "%s: io_burstmax is larger than io_read_buf and/or io_write_buf.\n",
			  __func__);
//TODO: check this
//		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "sec,max_coords", coords, 2)) {
		input_err(true, &client->dev,
			  "%s: Failed to get max_coords property\n", __func__);
		return -EINVAL;
	}
	pdata->max_x = coords[0] - 1;
	pdata->max_y = coords[1] - 1;

#ifdef PAT_CONTROL
	if (of_property_read_u32(np, "sec,pat_function",
				 &pdata->pat_function) < 0) {
		pdata->pat_function = 0;
		input_err(true, dev,
			"%s: Failed to get pat_function property\n", __func__);
	}

	if (of_property_read_u32(np, "sec,afe_base", &pdata->afe_base) < 0) {
		pdata->afe_base = 0;
		input_err(true, dev,
			  "%s: Failed to get afe_base property\n", __func__);
	}
#endif

	pdata->tsp_id = of_get_named_gpio(np, "sec,tsp-id_gpio", 0);
	if (gpio_is_valid(pdata->tsp_id))
		input_info(true, dev, "%s: TSP_ID : %d\n", __func__,
			   gpio_get_value(pdata->tsp_id));
	else
		input_err(true, dev,
			  "%s: Failed to get tsp-id gpio\n", __func__);

	pdata->switch_gpio = of_get_named_gpio(np,
					       "sec,switch_gpio", 0);
	if (gpio_is_valid(pdata->switch_gpio)) {
		ret = gpio_request_one(pdata->switch_gpio,
				       GPIOF_OUT_INIT_LOW,
				       "sec,touch_i2c_switch");
		if (ret) {
			input_err(true, dev,
				  "%s: Failed to request gpio %d\n",
				  __func__, pdata->switch_gpio);
			return -EINVAL;
		}

		ret = gpio_direction_output(pdata->switch_gpio,
					    SEC_SWITCH_GPIO_VALUE_AP_MASTER);
		if (ret) {
			input_err(true, dev,
				  "%s: Failed to set gpio %d direction\n",
				  __func__, pdata->switch_gpio);
			return -EINVAL;
		}
	} else {
		input_err(true, dev, "%s: Failed to get switch_gpio\n",
			  __func__);
	}

	pdata->reset_gpio = of_get_named_gpio(np, "sec,reset_gpio", 0);
	if (gpio_is_valid(pdata->reset_gpio)) {
		ret = gpio_request_one(pdata->reset_gpio,
					GPIOF_OUT_INIT_HIGH,
					"sec,touch_reset_gpio");
		if (ret) {
			input_err(true, dev,
				  "%s: Failed to request gpio %d, ret %d\n",
				  __func__, pdata->reset_gpio, ret);
			pdata->reset_gpio = -1;
		}
		//TODO: check this
		ret = gpio_direction_output(pdata->reset_gpio, 1);
		mdelay(10);
		ret = gpio_direction_output(pdata->reset_gpio, 0);
		mdelay(10);
		ret = gpio_direction_output(pdata->reset_gpio, 1);

	} else
		input_err(true, dev, "%s: Failed to get reset_gpio\n",
			__func__);

	count = of_property_count_strings(np, "sec,firmware_name");
	if (count <= 0) {
		pdata->firmware_name = NULL;
	} else {
		if (gpio_is_valid(pdata->tsp_id))
			of_property_read_string_index(np, "sec,firmware_name",
						gpio_get_value(pdata->tsp_id),
						&pdata->firmware_name);
		else
			of_property_read_string_index(np, "sec,firmware_name",
						      0, &pdata->firmware_name);
	}

	if (of_property_read_string_index(np, "sec,project_name", 0,
					  &pdata->project_name))
		input_err(true, &client->dev,
			"%s: skipped to get project_name property\n", __func__);
	if (of_property_read_string_index(np, "sec,project_name",
					  1, &pdata->model_name))
		input_err(true, &client->dev,
			  "%s: skipped to get model_name property\n", __func__);

#if defined(CONFIG_FB_MSM_MDSS_SAMSUNG)
	lcdtype = get_lcd_attached("GET");
	if (lcdtype < 0) {
		input_err(true, &client->dev,
			  "%s: lcd is not attached\n", __func__);
		return -ENODEV;
	}
#endif

#if defined(CONFIG_EXYNOS_DECON_FB)
	connected = get_lcd_info("connected");
	if (connected < 0) {
		input_err(true, dev, "%s: Failed to get lcd info\n", __func__);
		return -EINVAL;
	}

	if (!connected) {
		input_err(true, &client->dev,
			  "%s: lcd is disconnected\n", __func__);
		return -ENODEV;
	}

	input_info(true, &client->dev, "%s: lcd is connected\n", __func__);

	lcdtype = get_lcd_info("id");
	if (lcdtype < 0) {
		input_err(true, dev, "%s: Failed to get lcd info\n", __func__);
		return -EINVAL;
	}
#endif

	input_info(true, &client->dev,
		   "%s: lcdtype 0x%08X\n", __func__, lcdtype);

	if (pdata->model_name && strncmp(pdata->model_name, "G950", 4) == 0)
		pdata->panel_revision = 0;
	else
		pdata->panel_revision = ((lcdtype >> 8) & 0xFF) >> 4;

	if (of_property_read_string(np,
		"sec,regulator_dvdd", &pdata->regulator_dvdd))
		input_err(true, dev,
			"%s: Failed to get regulator_dvdd name property\n",
			__func__);

	if (of_property_read_string(np,
		"sec,regulator_avdd", &pdata->regulator_avdd))
		input_err(true, dev,
			"%s: Failed to get regulator_avdd name property\n",
			__func__);

	pdata->power = sec_ts_power;

	if (of_property_read_u32(np, "sec,always_lpmode",
				 &pdata->always_lpmode) < 0)
		pdata->always_lpmode = 0;

	if (of_property_read_u32(np, "sec,bringup", &pdata->bringup) < 0)
		pdata->bringup = 0;

	if (of_property_read_u32(np, "sec,mis_cal_check",
				 &pdata->mis_cal_check) < 0)
		pdata->mis_cal_check = 0;

	if (of_property_read_u32(np, "sec,heatmap_mode",
		&pdata->heatmap_mode) < 0)
		pdata->heatmap_mode = 0;

	pdata->regulator_boot_on = of_property_read_bool(np,
						"sec,regulator_boot_on");
	pdata->support_sidegesture = of_property_read_bool(np,
						"sec,support_sidegesture");
	pdata->support_dex = of_property_read_bool(np, "support_dex_mode");

	pdata->support_mt_pressure = true;

#ifdef PAT_CONTROL
	input_err(true, &client->dev,
		"%s: buffer limit: %d, lcd_id:%06X, bringup:%d, FW:%s(%d), id:%d,%d, pat_function:%d mis_cal:%d dex:%d, gesture:%d\n",
		__func__, pdata->io_burstmax, lcdtype, pdata->bringup,
		pdata->firmware_name, count, pdata->tsp_id, pdata->tsp_icid,
		pdata->pat_function, pdata->mis_cal_check, pdata->support_dex,
		pdata->support_sidegesture);
#else
	input_err(true, &client->dev,
		  "%s: buffer limit: %d, lcd_id:%06X, bringup:%d, FW:%s(%d), id:%d,%d, dex:%d, gesture:%d\n",
		  __func__, pdata->io_burstmax, lcdtype, pdata->bringup,
		  pdata->firmware_name, count, pdata->tsp_id, pdata->tsp_icid,
		  pdata->support_dex, pdata->support_sidegesture);
#endif
	return ret;
}

int sec_ts_read_information(struct sec_ts_data *ts)
{
	unsigned char data[13] = { 0 };
	int ret;

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_READ_INFO, true);

	memset(data, 0x0, 3);
	ret = sec_ts_read(ts, SEC_TS_READ_ID, data, 3);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
					"%s: failed to read device id(%d)\n",
					__func__, ret);
		goto out;
	}

	input_info(true, &ts->client->dev,
				"%s: %X, %X, %X\n",
				__func__, data[0], data[1], data[2]);
	memset(data, 0x0, 11);
	ret = sec_ts_read(ts,  SEC_TS_READ_PANEL_INFO, data, 11);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
					"%s: failed to read sub id(%d)\n",
					__func__, ret);
		goto out;
	}

	input_info(true, &ts->client->dev,
		   "%s: nTX:%X, nRX:%X, rY:%d, rX:%d\n",
		   __func__, data[8], data[9],
		   (data[2] << 8) | data[3], (data[0] << 8) | data[1]);

	/* Set X,Y Resolution from IC information. */
	if (((data[0] << 8) | data[1]) > 0)
		ts->plat_data->max_x = ((data[0] << 8) | data[1]) - 1;

	if (((data[2] << 8) | data[3]) > 0)
		ts->plat_data->max_y = ((data[2] << 8) | data[3]) - 1;

	ts->tx_count = data[8];
	ts->rx_count = data[9];

	data[0] = 0;
	ret = sec_ts_read(ts, SEC_TS_READ_BOOT_STATUS, data, 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
					"%s: failed to read sub id(%d)\n",
					__func__, ret);
		goto out;
	}

	input_info(true, &ts->client->dev,
				"%s: STATUS : %X\n",
				__func__, data[0]);

	memset(data, 0x0, 4);
	ret = sec_ts_read(ts, SEC_TS_READ_TS_STATUS, data, 4);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
					"%s: failed to read sub id(%d)\n",
					__func__, ret);
		goto out;
	}

	input_info(true, &ts->client->dev,
		   "%s: TOUCH STATUS : %02X, %02X, %02X, %02X\n",
		   __func__, data[0], data[1], data[2], data[3]);
	ret = sec_ts_read(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
			  (u8 *)&(ts->touch_functions), 2);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			"%s: failed to read touch functions(%d)\n",
			__func__, ret);
		goto out;
	}

	input_info(true, &ts->client->dev,
				"%s: Functions : %02X\n",
				__func__, ts->touch_functions);

out:
	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_READ_INFO, false);
	return ret;
}

#ifdef SEC_TS_SUPPORT_CUSTOMLIB
int sec_ts_set_custom_library(struct sec_ts_data *ts)
{
	u8 data[3] = { 0 };
	int ret;

	input_err(true, &ts->client->dev, "%s: Custom Library (0x%02x)\n",
				__func__, ts->lowpower_mode);

	data[2] = ts->lowpower_mode;

	ret = sec_ts_write(ts, SEC_TS_CMD_CUSTOMLIB_WRITE_PARAM, &data[0], 3);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: Failed to Custom Library\n", __func__);

	ret = sec_ts_write(ts, SEC_TS_CMD_CUSTOMLIB_NOTIFY_PACKET, NULL, 0);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			"%s: Failed to send NOTIFY Custom Library\n", __func__);

	return ret;
}

int sec_ts_check_custom_library(struct sec_ts_data *ts)
{
	u8 data[10] = { 0 };
	int ret = -1;

	ret = ts->sec_ts_read(ts, SEC_TS_CMD_CUSTOMLIB_GET_INFO, &data[0], 10);

	input_info(true, &ts->client->dev,
		"%s: (%d) %c%c%c%c, || %02X, %02X, %02X, %02X, || %02X, %02X\n",
		__func__, ret, data[0], data[1], data[2], data[3], data[4],
		data[5], data[6], data[7], data[8], data[9]);

	/* compare model name with device tree */
	if (ts->plat_data->model_name)
		ret = strncmp(data, ts->plat_data->model_name, 4);

	if (ret == 0)
		ts->use_customlib = true;
	else
		ts->use_customlib = false;

	input_err(true, &ts->client->dev, "%s: use %s\n",
		  __func__, ts->use_customlib ? "CUSTOMLIB" : "VENDOR");

	return ret;
}
#endif

static void sec_ts_set_input_prop(struct sec_ts_data *ts,
				  struct input_dev *dev, u8 propbit)
{
	static char sec_ts_phys[64] = { 0 };

	snprintf(sec_ts_phys, sizeof(sec_ts_phys), "%s/input1",
			dev->name);
	dev->phys = sec_ts_phys;
#ifdef I2C_INTERFACE
	dev->id.bustype = BUS_I2C;
#else
	dev->id.bustype = BUS_SPI;
#endif
	dev->dev.parent = &ts->client->dev;

	set_bit(EV_SYN, dev->evbit);
	set_bit(EV_KEY, dev->evbit);
	set_bit(EV_ABS, dev->evbit);
	set_bit(EV_SW, dev->evbit);
	set_bit(BTN_TOUCH, dev->keybit);
	set_bit(BTN_TOOL_FINGER, dev->keybit);
#ifdef SEC_TS_SUPPORT_CUSTOMLIB
	set_bit(KEY_BLACK_UI_GESTURE, dev->keybit);
#endif
#ifdef SEC_TS_SUPPORT_TOUCH_KEY
	if (ts->plat_data->support_mskey) {
		int i;

		for (i = 0 ; i < ts->plat_data->num_touchkey ; i++)
			set_bit(ts->plat_data->touchkey[i].keycode,
				dev->keybit);

		set_bit(EV_LED, dev->evbit);
		set_bit(LED_MISC, dev->ledbit);
	}
#endif
#ifdef KEY_SIDE_GESTURE
	if (ts->plat_data->support_sidegesture) {
		set_bit(KEY_SIDE_GESTURE, dev->keybit);
		set_bit(KEY_SIDE_GESTURE_RIGHT, dev->keybit);
		set_bit(KEY_SIDE_GESTURE_LEFT, dev->keybit);
	}
#endif
	set_bit(propbit, dev->propbit);
	set_bit(KEY_HOMEPAGE, dev->keybit);

#ifdef SW_GLOVE
	input_set_capability(dev, EV_SW, SW_GLOVE);
#endif
	input_set_abs_params(dev, ABS_MT_POSITION_X, 0, ts->plat_data->max_x,
			     0, 0);
	input_set_abs_params(dev, ABS_MT_POSITION_Y, 0, ts->plat_data->max_y,
			     0, 0);
	input_set_abs_params(dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER,
			     MT_TOOL_FINGER, 0, 0);
#ifdef ABS_MT_CUSTOM
	input_set_abs_params(dev, ABS_MT_CUSTOM, 0, 0xFFFF, 0, 0);
#endif
	if (ts->plat_data->support_mt_pressure)
		input_set_abs_params(dev, ABS_MT_PRESSURE, 0,
				     SEC_TS_PRESSURE_MAX, 0, 0);

	if (propbit == INPUT_PROP_POINTER)
		input_mt_init_slots(dev, MAX_SUPPORT_TOUCH_COUNT,
				    INPUT_MT_POINTER);
	else
		input_mt_init_slots(dev, MAX_SUPPORT_TOUCH_COUNT,
				    INPUT_MT_DIRECT);

	input_set_drvdata(dev, ts);
}

static int sec_ts_fw_init(struct sec_ts_data *ts)
{
	int ret = SEC_TS_ERR_NA;
	bool force_update = false;
	bool valid_firmware_integrity = false;
	unsigned char data[5] = { 0 };
	unsigned char deviceID[5] = { 0 };
	unsigned char result = 0;

	ret = sec_ts_read(ts, SEC_TS_READ_DEVICE_ID, deviceID, 5);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: failed to read device ID(%d)\n",
			  __func__, ret);
	else
		input_info(true, &ts->client->dev,
			"%s: TOUCH DEVICE ID : %02X, %02X, %02X, %02X, %02X\n",
			__func__, deviceID[0], deviceID[1], deviceID[2],
			deviceID[3], deviceID[4]);

	ret = sec_ts_read(ts, SEC_TS_READ_FIRMWARE_INTEGRITY, &result, 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: failed to integrity check (%d)\n",
			  __func__, ret);
	} else {
		if (result & 0x80)
			valid_firmware_integrity = true;
		else
			input_err(true, &ts->client->dev,
				  "%s: invalid integrity result (0x%x)\n",
				  __func__, result);
	}

	ret = sec_ts_read(ts, SEC_TS_READ_BOOT_STATUS, &data[0], 1);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: failed to read sub id(%d)\n", __func__, ret);
	} else {
		ret = sec_ts_read(ts, SEC_TS_READ_TS_STATUS, &data[1], 4);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: failed to touch status(%d)\n",
				  __func__, ret);
	}
	input_info(true, &ts->client->dev,
		"%s: TOUCH STATUS : %02X || %02X, %02X, %02X, %02X\n",
		__func__, data[0], data[1], data[2], data[3], data[4]);

	if (data[0] == SEC_TS_STATUS_BOOT_MODE)
		ts->checksum_result = 1;

	if (((data[0] == SEC_TS_STATUS_APP_MODE &&
	      data[2] == TOUCH_SYSTEM_MODE_FLASH) || ret < 0) &&
	    (valid_firmware_integrity == false))
		force_update = true;

	ret = sec_ts_read_information(ts);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: fail to read information 0x%x\n",
			  __func__, ret);
		return SEC_TS_ERR_INIT;
	}

	ts->touch_functions |= SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC;
	ret = sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
			       (u8 *)&ts->touch_functions, 2);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: Failed to send touch func_mode command",
			  __func__);

	/* Sense_on */
	ret = sec_ts_write(ts, SEC_TS_CMD_SENSE_ON, NULL, 0);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: fail to write Sense_on 0x%x\n",
			  __func__, ret);
		return SEC_TS_ERR_INIT;
	}

	ts->pFrame = kzalloc(ts->tx_count * ts->rx_count * 2, GFP_KERNEL);
	if (!ts->pFrame)
		return SEC_TS_ERR_ALLOC_FRAME;

	ts->gainTable = kzalloc(ts->tx_count * ts->rx_count, GFP_KERNEL);
	if (!ts->gainTable) {
		kfree(ts->pFrame);
		ts->pFrame = NULL;
		return SEC_TS_ERR_ALLOC_GAINTABLE;
	}

	if (ts->plat_data->support_dex) {
		ts->input_dev_pad->name = "sec_touchpad";
		sec_ts_set_input_prop(ts, ts->input_dev_pad,
				      INPUT_PROP_POINTER);
	}
	ts->dex_name = "";

	ts->input_dev->name = "sec_touchscreen";
	sec_ts_set_input_prop(ts, ts->input_dev, INPUT_PROP_DIRECT);
#ifdef USE_OPEN_CLOSE
	ts->input_dev->open = sec_ts_input_open;
	ts->input_dev->close = sec_ts_input_close;
#endif
	ts->input_dev_touch = ts->input_dev;

	ret = input_register_device(ts->input_dev);
	if (ret) {
		input_err(true, &ts->client->dev,
			  "%s: Unable to register %s input device 0x%x\n",
			  __func__, ts->input_dev->name, ret);
		return SEC_TS_ERR_REG_INPUT_DEV;
	}

	if (ts->plat_data->support_dex) {
		ret = input_register_device(ts->input_dev_pad);
		if (ret) {
			input_err(true, &ts->client->dev,
				  "%s: Unable to register %s input device 0x%x\n",
				  __func__, ts->input_dev_pad->name, ret);
			return SEC_TS_ERR_REG_INPUT_PAD_DEV;
		}
	}

	return SEC_TS_ERR_NA;
}

static void sec_ts_device_init(struct sec_ts_data *ts)
{
#if (1) //!defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
	sec_ts_raw_device_init(ts);
#endif
	sec_ts_fn_init(ts);

#ifdef SEC_TS_SUPPORT_CUSTOMLIB
	sec_ts_check_custom_library(ts);
	if (ts->use_customlib)
		sec_ts_set_custom_library(ts);
#endif
}

static struct notifier_block sec_ts_screen_nb;
static struct notifier_block sec_ts_psy_nb;

#ifdef I2C_INTERFACE
static int sec_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#else
static int sec_ts_probe(struct spi_device *client)
#endif
{
	struct sec_ts_data *ts;
	struct sec_ts_plat_data *pdata;
	int ret = 0;

	input_info(true, &client->dev, "%s\n", __func__);

#ifdef I2C_INTERFACE
	input_info(true, &client->dev, "%s: I2C interface\n", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		input_err(true, &client->dev, "%s: EIO err!\n", __func__);
		return -EIO;
	}
#else
	input_info(true, &client->dev, "%s: SPI interface\n", __func__);
#endif
	/* parse dt */
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
				sizeof(struct sec_ts_plat_data), GFP_KERNEL);

		if (!pdata) {
			input_err(true, &client->dev,
				"%s: Failed to allocate platform data\n",
				__func__);
			goto error_allocate_pdata;
		}

		client->dev.platform_data = pdata;

		ret = sec_ts_parse_dt(client);
		if (ret) {
			input_err(true, &client->dev,
				  "%s: Failed to parse dt\n", __func__);
			goto error_allocate_mem;
		}
	} else {
		pdata = client->dev.platform_data;
		if (!pdata) {
			input_err(true, &client->dev,
				  "%s: No platform data found\n", __func__);
			goto error_allocate_pdata;
		}
	}

	if (!pdata->power) {
		input_err(true, &client->dev, "%s: No power contorl found\n",
			  __func__);
		goto error_allocate_mem;
	}

	pdata->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(pdata->pinctrl))
		input_err(true, &client->dev, "%s: could not get pinctrl\n",
			  __func__);

	ts = kzalloc(sizeof(struct sec_ts_data), GFP_KERNEL);
	if (!ts)
		goto error_allocate_mem;

	ts->client = client;
	ts->plat_data = pdata;
	ts->crc_addr = 0x0001FE00;
	ts->fw_addr = 0x00002000;
	ts->para_addr = 0x18000;
	ts->flash_page_size = SEC_TS_FW_BLK_SIZE_DEFAULT;
	ts->sec_ts_read = sec_ts_read;
	ts->sec_ts_read_heap = sec_ts_read_heap;
	ts->sec_ts_write = sec_ts_write;
	ts->sec_ts_write_burst = sec_ts_write_burst;
	ts->sec_ts_write_burst_heap = sec_ts_write_burst_heap;
	ts->sec_ts_read_bulk = sec_ts_read_bulk;
	ts->sec_ts_read_bulk_heap = sec_ts_read_bulk_heap;
	ts->io_burstmax = pdata->io_burstmax;
#ifdef USE_POWER_RESET_WORK
	INIT_DELAYED_WORK(&ts->reset_work, sec_ts_reset_work);
#endif
	INIT_WORK(&ts->suspend_work, sec_ts_suspend_work);
	INIT_WORK(&ts->resume_work, sec_ts_resume_work);
	INIT_WORK(&ts->charger_work, sec_ts_charger_work);
	ts->event_wq = alloc_workqueue("sec_ts-event-queue", WQ_UNBOUND |
					 WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!ts->event_wq) {
		input_err(true, &ts->client->dev,
			"%s: Cannot create work thread\n", __func__);
		ret = -ENOMEM;
		goto error_alloc_workqueue;
	}

	init_completion(&ts->bus_resumed);
	complete_all(&ts->bus_resumed);

#ifdef SEC_TS_FW_UPDATE_ON_PROBE
	INIT_WORK(&ts->fw_update_work, sec_ts_fw_update_work);
#else
	input_info(true, &ts->client->dev, "%s: fw update on probe disabled!\n",
		   __func__);
	ts->fw_update_wq = alloc_workqueue("sec_ts-fw-update-queue",
					    WQ_UNBOUND | WQ_HIGHPRI |
					    WQ_CPU_INTENSIVE, 1);
	if (!ts->fw_update_wq) {
		input_err(true, &ts->client->dev,
			  "%s: Can't alloc fw update work thread\n",
			  __func__);
		ret = -ENOMEM;
		goto error_alloc_fw_update_wq;
	}
	INIT_DELAYED_WORK(&ts->fw_update_work, sec_ts_fw_update_work);
#endif

	ts->is_fw_corrupted = false;

	/* Assume screen is on throughout probe */
	ts->bus_refmask = SEC_TS_BUS_REF_SCREEN_ON;
#ifdef I2C_INTERFACE
	i2c_set_clientdata(client, ts);
#else
	spi_set_drvdata(client, ts);
#endif

#if IS_ENABLED(CONFIG_TOUCHSCREEN_TBN)
	ts->tbn = tbn_init(&ts->client->dev);
	if (!ts->tbn) {
		input_err(true, &ts->client->dev,
			  "%s: TBN initialization error\n", __func__);
		ret = -ENODEV;
		goto err_init_tbn;
	}
#endif

	if (gpio_is_valid(ts->plat_data->tsp_id))
		ts->tspid_val = gpio_get_value(ts->plat_data->tsp_id);

	if (gpio_is_valid(ts->plat_data->tsp_icid))
		ts->tspicid_val = gpio_get_value(ts->plat_data->tsp_icid);

	ts->input_dev = input_allocate_device();
	if (!ts->input_dev) {
		input_err(true, &ts->client->dev,
			  "%s: allocate device err!\n", __func__);
		ret = -ENOMEM;
		goto err_allocate_input_dev;
	}

	if (ts->plat_data->support_dex) {
		ts->input_dev_pad = input_allocate_device();
		if (!ts->input_dev_pad) {
			input_err(true, &ts->client->dev,
				  "%s: allocate device err!\n", __func__);
			ret = -ENOMEM;
			goto err_allocate_input_dev_pad;
		}
	}

	ts->touch_count = 0;
	ts->tid_palm_state = 0;
	ts->tid_grip_state = 0;
	ts->tid_touch_state = 0;
	ts->palms_leaved_once = false;
	ts->grips_leaved_once = false;

	ts->sec_ts_write = sec_ts_write;
	ts->sec_ts_read = sec_ts_read;
	ts->sec_ts_read_heap = sec_ts_read_heap;
	ts->sec_ts_read_customlib = sec_ts_read_from_customlib;

	ts->max_z_value = 0;
	ts->min_z_value = 0xFFFFFFFF;
	ts->sum_z_value = 0;

	mutex_init(&ts->bus_mutex);
	mutex_init(&ts->lock);
	mutex_init(&ts->device_mutex);
	mutex_init(&ts->io_mutex);
	mutex_init(&ts->eventlock);

	init_completion(&ts->resume_done);
	complete_all(&ts->resume_done);

	init_completion(&ts->boot_completed);
	complete_all(&ts->boot_completed);

	if (pdata->always_lpmode)
		ts->lowpower_mode |= SEC_TS_MODE_CUSTOMLIB_FORCE_KEY;
	else
		ts->lowpower_mode &= ~SEC_TS_MODE_CUSTOMLIB_FORCE_KEY;

	sec_ts_pinctrl_configure(ts, true);

	/* power enable */
	sec_ts_power(ts, true);
	if (!pdata->regulator_boot_on)
		sec_ts_delay(70);
	ts->power_status = SEC_TS_STATE_POWER_ON;
	ts->external_factory = false;

	ret = sec_ts_wait_for_ready(ts, SEC_TS_ACK_BOOT_COMPLETE);
	if (ret < 0) {
		u8 boot_status;
		/* Read the boot status in case device is in bootloader mode */
		ret = ts->sec_ts_read(ts, SEC_TS_READ_BOOT_STATUS,
					  &boot_status, 1);
		if (ret < 0) {
			input_err(true, &ts->client->dev,
				  "%s: could not read boot status. Assuming no device connected.\n",
				  __func__);
			goto err_init;
		}

		input_info(true, &ts->client->dev,
			   "%s: Attempting to reflash the firmware. Boot status = 0x%02X\n",
			   __func__, boot_status);
		if (boot_status != SEC_TS_STATUS_BOOT_MODE)
			input_err(true, &ts->client->dev,
				  "%s: device is not in bootloader mode!\n",
				  __func__);

		ts->is_fw_corrupted = true;
	}

	input_info(true, &client->dev, "%s: power enable\n", __func__);

	if (ts->is_fw_corrupted == false) {
		switch (sec_ts_fw_init(ts)) {
		case SEC_TS_ERR_INIT:
			goto err_init;
		case SEC_TS_ERR_ALLOC_FRAME:
			goto err_allocate_frame;
		case SEC_TS_ERR_ALLOC_GAINTABLE:
			goto err_allocate_gaintable;
		case SEC_TS_ERR_REG_INPUT_DEV:
			goto err_input_register_device;
		case SEC_TS_ERR_REG_INPUT_PAD_DEV:
			goto err_input_pad_register_device;
		}
	}

	ts->pm_spi_req.type = PM_QOS_REQ_AFFINE_IRQ;
	ts->pm_spi_req.irq = geni_spi_get_master_irq(client);
	irq_set_perf_affinity(ts->pm_spi_req.irq, IRQF_PERF_AFFINE);
	pm_qos_add_request(&ts->pm_spi_req, PM_QOS_CPU_DMA_LATENCY,
		PM_QOS_DEFAULT_VALUE);

	ts->pm_touch_req.type = PM_QOS_REQ_AFFINE_IRQ;
	ts->pm_touch_req.irq = client->irq;
	pm_qos_add_request(&ts->pm_touch_req, PM_QOS_CPU_DMA_LATENCY,
		PM_QOS_DEFAULT_VALUE);

	ts->ignore_charger_nb = 0;
	/* init motion filter mode */
	ts->use_default_mf = 0;
	ts->mf_state = SEC_TS_MF_FILTERED;
#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
	/*
	 * Heatmap_probe must be called before irq routine is registered,
	 * because heatmap_read is called from the irq context.
	 * If the ISR runs before heatmap_probe is finished, it will invoke
	 * heatmap_read and cause NPE, since read_frame would not yet be set.
	 */
	ts->v4l2.parent_dev = &ts->client->dev;
	ts->v4l2.input_dev = ts->input_dev;
	ts->v4l2.read_frame = read_heatmap_raw;
	ts->v4l2.width = ts->tx_count;
	ts->v4l2.height = ts->rx_count;
	/* 120 Hz operation */
	ts->v4l2.timeperframe.numerator = 1;
	ts->v4l2.timeperframe.denominator = 120;
	ret = heatmap_probe(&ts->v4l2);
	if (ret) {
		input_err(true, &ts->client->dev,
			"%s: Heatmap probe failed\n", __func__);
		goto err_irq;
	}
#endif

	input_info(true, &ts->client->dev, "%s: request_irq = %d\n", __func__,
			client->irq);

	ret = request_threaded_irq(client->irq, sec_ts_isr, sec_ts_irq_thread,
			ts->plat_data->irq_type | IRQF_PRIME_AFFINE,
			SEC_TS_NAME, ts);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			"%s: Unable to request threaded irq\n", __func__);
		goto err_heatmap;
	}

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	ts->offload.caps.touch_offload_major_version = 1;
	ts->offload.caps.touch_offload_minor_version = 0;
	/* ID equivalent to the 4-byte, little-endian string: '00r3' */
	ts->offload.caps.device_id =
	    '3' << 24 | 'r' << 16 | '0' << 8 | '0' << 0;
	ts->offload.caps.display_width = ts->plat_data->max_x + 1;
	ts->offload.caps.display_height = ts->plat_data->max_y + 1;
	ts->offload.caps.tx_size = ts->tx_count;
	ts->offload.caps.rx_size = ts->rx_count;
	ts->offload.caps.heatmap_size = HEATMAP_SIZE_FULL;
#ifdef I2C_INTERFACE
	ts->offload.caps.bus_type = BUS_TYPE_I2C;
	ts->offload.caps.bus_speed_hz = 1000000;
#else
	ts->offload.caps.bus_type = BUS_TYPE_SPI;
	ts->offload.caps.bus_speed_hz = client->max_speed_hz;
#endif

	/* Currently can only reliably read mutual and self strength heatmaps
	 * each frame. Cannot support other formats due to penalties associated
	 * with switching data types.
	 */
	ts->offload.caps.touch_data_types =
	    TOUCH_DATA_TYPE_COORD | TOUCH_DATA_TYPE_STRENGTH;
	ts->offload.caps.touch_scan_types =
	    TOUCH_SCAN_TYPE_MUTUAL | TOUCH_SCAN_TYPE_SELF;

	ts->offload.caps.continuous_reporting = true;
	ts->offload.caps.noise_reporting = false;
	ts->offload.caps.cancel_reporting = false;
	ts->offload.caps.size_reporting = true;
	ts->offload.caps.filter_grip = true;
	ts->offload.caps.filter_palm = true;
	ts->offload.caps.num_sensitivity_settings = 1;

	ts->offload.hcallback = (void *)ts;
	ts->offload.report_cb = sec_ts_offload_report;
	touch_offload_init(&ts->offload);
#endif

	ts->notifier = sec_ts_screen_nb;
	ret = drm_panel_notifier_register(pdata->panel, &ts->notifier);
	if (ret < 0) {
		input_err(true, &ts->client->dev,
			  "%s: drm_panel_notifier_register failed. ret = 0x%08X\n",
			  __func__, ret);
		goto err_register_drm_client;
	}

#ifndef CONFIG_SEC_SYSFS
	sec_class = class_create(THIS_MODULE, "sec");
#endif

	device_init_wakeup(&client->dev, true);

	if (ts->is_fw_corrupted == false)
		sec_ts_device_init(ts);

#ifdef SEC_TS_FW_UPDATE_ON_PROBE
	schedule_work(&ts->fw_update_work);

	/* Do not finish probe without checking and flashing the firmware */
	flush_work(&ts->fw_update_work);
#else
	queue_delayed_work(ts->fw_update_wq, &ts->fw_update_work,
		    msecs_to_jiffies(SEC_TS_FW_UPDATE_DELAY_MS_AFTER_PROBE));
#endif

#if defined(CONFIG_TOUCHSCREEN_DUMP_MODE)
	dump_callbacks.inform_dump = dump_tsp_log;
	INIT_DELAYED_WORK(&ts->ghost_check, sec_ts_check_rawdata);
	p_ghost_check = &ts->ghost_check;
#endif

	ts_dup = ts;
	ts->probe_done = true;

	ts->wlc_online = false;
	ts->usb_present = false;
	ts->charger_mode = SEC_TS_BIT_CHARGER_MODE_NO;
	ts->wireless_psy = power_supply_get_by_name("wireless");
	ts->usb_psy = power_supply_get_by_name("usb");
	ts->psy_nb = sec_ts_psy_nb;
	ret = power_supply_reg_notifier(&ts->psy_nb);
	if (ret < 0)
		input_err(true, &ts->client->dev, "psy notifier register failed\n");

	input_err(true, &ts->client->dev, "%s: done\n", __func__);
	input_log_fix();

	return 0;

	/* need to be enabled when new goto statement is added */
/*
 *	sec_ts_fn_remove(ts);
 *	free_irq(client->irq, ts);
 **/
err_register_drm_client:
	free_irq(client->irq, ts);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	touch_offload_cleanup(&ts->offload);
#endif

err_heatmap:
#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
	heatmap_remove(&ts->v4l2);
err_irq:
#endif
	pm_qos_remove_request(&ts->pm_touch_req);
	pm_qos_remove_request(&ts->pm_spi_req);
	if (ts->plat_data->support_dex) {
		input_unregister_device(ts->input_dev_pad);
		ts->input_dev_pad = NULL;
	}
err_input_pad_register_device:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
	ts->input_dev_touch = NULL;
err_input_register_device:
	kfree(ts->gainTable);
err_allocate_gaintable:
	kfree(ts->pFrame);
err_allocate_frame:
err_init:
	sec_ts_power(ts, false);
	if (ts->plat_data->support_dex) {
		if (ts->input_dev_pad)
			input_free_device(ts->input_dev_pad);
	}
err_allocate_input_dev_pad:
	if (ts->input_dev)
		input_free_device(ts->input_dev);
err_allocate_input_dev:
#if IS_ENABLED(CONFIG_TOUCHSCREEN_TBN)
	tbn_cleanup(ts->tbn);
err_init_tbn:
#endif

#ifndef SEC_TS_FW_UPDATE_ON_PROBE
	if (ts->fw_update_wq)
		destroy_workqueue(ts->fw_update_wq);
error_alloc_fw_update_wq:
#endif

	if (ts->event_wq)
		destroy_workqueue(ts->event_wq);
error_alloc_workqueue:
	kfree(ts);

error_allocate_mem:
	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);
	if (gpio_is_valid(pdata->tsp_id))
		gpio_free(pdata->tsp_id);
	if (gpio_is_valid(pdata->tsp_icid))
		gpio_free(pdata->tsp_icid);
	if (gpio_is_valid(pdata->switch_gpio))
		gpio_free(pdata->switch_gpio);
	if (gpio_is_valid(pdata->reset_gpio))
		gpio_free(pdata->reset_gpio);

error_allocate_pdata:
	if (ret == -ECONNREFUSED)
		sec_ts_delay(100);
	if (ret != -EPROBE_DEFER)
		ret = -ENODEV;
#ifdef CONFIG_TOUCHSCREEN_DUMP_MODE
	p_ghost_check = NULL;
#endif
	ts_dup = NULL;
	input_err(true, &client->dev, "%s: failed(%d)\n", __func__, ret);
	input_log_fix();
	return ret;
}

void sec_ts_unlocked_release_all_finger(struct sec_ts_data *ts)
{
	int i;

	for (i = 0; i < MAX_SUPPORT_TOUCH_COUNT; i++) {
		input_mt_slot(ts->input_dev, i);
		if (ts->plat_data->support_mt_pressure)
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER,
					   false);

		if ((ts->coord[i].action == SEC_TS_COORDINATE_ACTION_PRESS) ||
			(ts->coord[i].action ==
			 SEC_TS_COORDINATE_ACTION_MOVE)) {

			input_info(true, &ts->client->dev,
				"%s: [RA] tID:%d mc:%d tc:%d v:%02X%02X cal:%02X(%02X) id(%d,%d) p:%d\n",
				__func__, i,
				ts->coord[i].mcount, ts->touch_count,
				ts->plat_data->img_version_of_ic[2],
				ts->plat_data->img_version_of_ic[3],
				ts->cal_status, ts->nv, ts->tspid_val,
				ts->tspicid_val, ts->coord[i].palm_count);

			do_gettimeofday(&ts->time_released[i]);

			if (ts->time_longest <
				(ts->time_released[i].tv_sec -
				 ts->time_pressed[i].tv_sec))
				ts->time_longest =
					(ts->time_released[i].tv_sec -
					 ts->time_pressed[i].tv_sec);
		}

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
		ts->offload.coords[i].status = COORD_STATUS_INACTIVE;
		ts->offload.coords[i].major = 0;
		ts->offload.coords[i].minor = 0;
		ts->offload.coords[i].pressure = 0;
#endif
		ts->coord[i].action = SEC_TS_COORDINATE_ACTION_RELEASE;
		ts->coord[i].mcount = 0;
		ts->coord[i].palm_count = 0;

	}

	input_mt_slot(ts->input_dev, 0);

	input_report_key(ts->input_dev, BTN_TOUCH, false);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, false);
#ifdef SW_GLOVE
	input_report_switch(ts->input_dev, SW_GLOVE, false);
#endif
	ts->touchkey_glove_mode_status = false;
	ts->touch_count = 0;
	ts->check_multi = 0;
	ts->tid_palm_state = 0;
	ts->tid_grip_state = 0;
	ts->tid_touch_state = 0;
	ts->palms_leaved_once = false;
	ts->grips_leaved_once = false;

#ifdef KEY_SIDE_GESTURE
	if (ts->plat_data->support_sidegesture) {
		input_report_key(ts->input_dev, KEY_SIDE_GESTURE, 0);
		input_report_key(ts->input_dev, KEY_SIDE_GESTURE_LEFT, 0);
		input_report_key(ts->input_dev, KEY_SIDE_GESTURE_RIGHT, 0);
	}
#endif
	input_report_key(ts->input_dev, KEY_HOMEPAGE, 0);
	input_sync(ts->input_dev);

}

void sec_ts_locked_release_all_finger(struct sec_ts_data *ts)
{
	int i;

	mutex_lock(&ts->eventlock);

	for (i = 0; i < MAX_SUPPORT_TOUCH_COUNT; i++) {
		input_mt_slot(ts->input_dev, i);
		if (ts->plat_data->support_mt_pressure)
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, 0);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER,
					   false);

		if ((ts->coord[i].action == SEC_TS_COORDINATE_ACTION_PRESS) ||
			(ts->coord[i].action ==
			 SEC_TS_COORDINATE_ACTION_MOVE)) {

			input_info(true, &ts->client->dev,
				"%s: [RA] tID:%d mc: %d tc:%d, v:%02X%02X, cal:%X(%X|%X), id(%d,%d), p:%d\n",
				__func__, i, ts->coord[i].mcount,
				ts->touch_count,
				ts->plat_data->img_version_of_ic[2],
				ts->plat_data->img_version_of_ic[3],
				ts->cal_status, ts->nv, ts->cal_count,
				ts->tspid_val, ts->tspicid_val,
				ts->coord[i].palm_count);

			do_gettimeofday(&ts->time_released[i]);

			if (ts->time_longest <
				(ts->time_released[i].tv_sec -
				 ts->time_pressed[i].tv_sec))
				ts->time_longest =
					(ts->time_released[i].tv_sec -
					 ts->time_pressed[i].tv_sec);
		}

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
		ts->offload.coords[i].status = COORD_STATUS_INACTIVE;
		ts->offload.coords[i].major = 0;
		ts->offload.coords[i].minor = 0;
		ts->offload.coords[i].pressure = 0;
#endif
		ts->coord[i].action = SEC_TS_COORDINATE_ACTION_RELEASE;
		ts->coord[i].mcount = 0;
		ts->coord[i].palm_count = 0;

	}

	input_mt_slot(ts->input_dev, 0);

	input_report_key(ts->input_dev, BTN_TOUCH, false);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, false);
#ifdef SW_GLOVE
	input_report_switch(ts->input_dev, SW_GLOVE, false);
#endif
	ts->touchkey_glove_mode_status = false;
	ts->touch_count = 0;
	ts->check_multi = 0;
	ts->tid_palm_state = 0;
	ts->tid_grip_state = 0;
	ts->tid_touch_state = 0;
	ts->palms_leaved_once = false;
	ts->grips_leaved_once = false;

#ifdef KEY_SIDE_GESTURE
	if (ts->plat_data->support_sidegesture) {
		input_report_key(ts->input_dev, KEY_SIDE_GESTURE, 0);
		input_report_key(ts->input_dev, KEY_SIDE_GESTURE_LEFT, 0);
		input_report_key(ts->input_dev, KEY_SIDE_GESTURE_RIGHT, 0);
	}
#endif
	input_report_key(ts->input_dev, KEY_HOMEPAGE, 0);
	input_sync(ts->input_dev);

	mutex_unlock(&ts->eventlock);

}

#ifdef USE_POWER_RESET_WORK
static void sec_ts_reset_work(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
							reset_work.work);

	ts->reset_is_on_going = true;
	input_info(true, &ts->client->dev, "%s\n", __func__);

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_RESET, true);

	sec_ts_stop_device(ts);

	sec_ts_delay(30);

	sec_ts_start_device(ts);

	if (ts->input_dev_touch->disabled) {
		input_err(true, &ts->client->dev,
			  "%s: call input_close\n", __func__);

		sec_ts_input_close(ts->input_dev);

		if ((ts->lowpower_mode & SEC_TS_MODE_CUSTOMLIB_AOD) &&
		    ts->use_customlib) {
			int i, ret;
			u8 data[10] = {0x02, 0};

			for (i = 0; i < 4; i++) {
				data[i * 2 + 2] = ts->rect_data[i] & 0xFF;
				data[i * 2 + 3] =
						(ts->rect_data[i] >> 8) & 0xFF;
			}

			disable_irq(ts->client->irq);
			ret = ts->sec_ts_write(ts,
				SEC_TS_CMD_CUSTOMLIB_WRITE_PARAM, &data[0], 10);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: Failed to write offset\n",
					__func__);

			ret = ts->sec_ts_write(ts,
				SEC_TS_CMD_CUSTOMLIB_NOTIFY_PACKET, NULL, 0);
			if (ret < 0)
				input_err(true, &ts->client->dev,
					"%s: Failed to send notify\n",
					__func__);
			enable_irq(ts->client->irq);
		}
	}
	ts->reset_is_on_going = false;

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_RESET, false);
}
#endif

void sec_ts_read_init_info(struct sec_ts_data *ts)
{
#ifndef CONFIG_SEC_FACTORY
	struct sec_ts_test_mode mode;
	char para = TO_TOUCH_MODE;
#endif
#ifdef USE_PRESSURE_SENSOR
	unsigned char data[18] = { 0 };
#endif
	int ret;

	ts->nv = get_tsp_nvm_data(ts, SEC_TS_NVM_OFFSET_FAC_RESULT);
	ts->cal_count = get_tsp_nvm_data(ts, SEC_TS_NVM_OFFSET_CAL_COUNT);
	ts->pressure_cal_base = get_tsp_nvm_data(ts,
				SEC_TS_NVM_OFFSET_PRESSURE_BASE_CAL_COUNT);
	ts->pressure_cal_delta = get_tsp_nvm_data(ts,
				SEC_TS_NVM_OFFSET_PRESSURE_DELTA_CAL_COUNT);

	input_info(true, &ts->client->dev,
		    "%s: fac_nv:%02X, cal_count:%02X\n",
		    __func__, ts->nv, ts->cal_count);

#ifdef PAT_CONTROL
	ts->tune_fix_ver = (get_tsp_nvm_data(ts,
				SEC_TS_NVM_OFFSET_TUNE_VERSION) << 8) |
			    get_tsp_nvm_data(ts,
				SEC_TS_NVM_OFFSET_TUNE_VERSION + 1);
	input_info(true, &ts->client->dev,
	    "%s: tune_fix_ver [%04X]\n", __func__, ts->tune_fix_ver);
#endif

#ifdef USE_PRESSURE_SENSOR
	ret = ts->sec_ts_read(ts, SEC_TS_CMD_SET_GET_PRESSURE, data, 18);
	if (ret < 0)
		return;

	ts->pressure_left = ((data[16] << 8) | data[17]);
	ts->pressure_center = ((data[8] << 8) | data[9]);
	ts->pressure_right = ((data[0] << 8) | data[1]);
	input_info(true, &ts->client->dev,
		"%s: left: %d, center: %d, right: %d\n", __func__,
		ts->pressure_left, ts->pressure_center, ts->pressure_right);
#endif

#ifndef CONFIG_SEC_FACTORY
	/* run self-test */
	disable_irq(ts->client->irq);
	execute_selftest(ts,
		TEST_OPEN | TEST_NODE_VARIANCE |
		TEST_SHORT | TEST_SELF_NODE | TEST_NOT_SAVE);
	enable_irq(ts->client->irq);

	input_info(true, &ts->client->dev, "%s: %02X %02X %02X %02X\n",
		__func__, ts->ito_test[0], ts->ito_test[1]
		, ts->ito_test[2], ts->ito_test[3]);

	ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_POWER_MODE, &para, 1);
	if (ret < 0)
		input_err(true, &ts->client->dev, "%s: Failed to set\n",
				__func__);

	sec_ts_delay(350);

	/* run ambient read */
	memset(&mode, 0x00, sizeof(struct sec_ts_test_mode));
	mode.type = TYPE_AMBIENT_DATA;
	mode.allnode = TEST_MODE_ALL_NODE;

	sec_ts_read_raw_data(ts, NULL, &mode);
#endif

	input_log_fix();
}

static void sec_ts_fw_update_work(struct work_struct *work)
{
#ifdef SEC_TS_FW_UPDATE_ON_PROBE
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
					      fw_update_work);
#else
	struct delayed_work *fw_update_work = container_of(work,
					struct delayed_work, work);
	struct sec_ts_data *ts = container_of(fw_update_work,
					struct sec_ts_data, fw_update_work);
#endif

	int ret;

	input_info(true, &ts->client->dev,
		   "%s: Beginning firmware update after probe.\n", __func__);

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_FW_UPDATE, true);

	ret = sec_ts_firmware_update_on_probe(ts, false);
	if (ret < 0)
		input_info(true, &ts->client->dev,
			   "%s: firmware update was unsuccessful.\n",
			   __func__);

	if (ts->is_fw_corrupted == true && ret == 0) {
		ret = sec_ts_fw_init(ts);
		if (ret == SEC_TS_ERR_NA) {
			ts->is_fw_corrupted = false;
			sec_ts_device_init(ts);
		} else
			input_info(true, &ts->client->dev,
				"%s: fail to sec_ts_fw_init 0x%x\n",
				__func__, ret);
	}

	if (ts->is_fw_corrupted == false)
		sec_ts_read_init_info(ts);
	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_FW_UPDATE, false);
}

int sec_ts_set_lowpowermode(struct sec_ts_data *ts, u8 mode)
{
	int ret;
	int retrycnt = 0;
	u8 data;
	char para = 0;

	input_err(true, &ts->client->dev, "%s: %s(%X)\n", __func__,
			mode == TO_LOWPOWER_MODE ? "ENTER" : "EXIT",
			ts->lowpower_mode);

	if (mode) {
		#ifdef SEC_TS_SUPPORT_CUSTOMLIB
		if (ts->use_customlib)
			sec_ts_set_custom_library(ts);
		#endif

		data = (ts->lowpower_mode & SEC_TS_MODE_LOWPOWER_FLAG) >> 1;
		ret = sec_ts_write(ts, SEC_TS_CMD_WAKEUP_GESTURE_MODE,
				   &data, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: Failed to set\n", __func__);
	}

retry_pmode:
	ret = sec_ts_write(ts, SEC_TS_CMD_SET_POWER_MODE, &mode, 1);
	if (ret < 0)
		input_err(true, &ts->client->dev,
				"%s: failed\n", __func__);
	sec_ts_delay(50);

	/* read data */

	ret = sec_ts_read(ts, SEC_TS_CMD_SET_POWER_MODE, &para, 1);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: read power mode failed!\n", __func__);
	else
		input_info(true, &ts->client->dev,
			   "%s: power mode - write(%d) read(%d)\n",
			   __func__, mode, para);

	if (mode != para) {
		retrycnt++;
		if (retrycnt < 5)
			goto retry_pmode;
	}

	ret = sec_ts_write(ts, SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: write clear event failed\n", __func__);


	sec_ts_locked_release_all_finger(ts);

	if (device_may_wakeup(&ts->client->dev)) {
		if (mode)
			enable_irq_wake(ts->client->irq);
		else
			disable_irq_wake(ts->client->irq);
	}

	ts->lowpower_status = mode;
	input_info(true, &ts->client->dev, "%s: end\n", __func__);

	return ret;
}

#ifdef USE_OPEN_CLOSE
static int sec_ts_input_open(struct input_dev *dev)
{
	struct sec_ts_data *ts = input_get_drvdata(dev);
	int ret;

	ts->input_closed = false;

	input_info(true, &ts->client->dev, "%s\n", __func__);

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_INPUT_DEV, true);

	if (ts->lowpower_status) {
#ifdef USE_RESET_EXIT_LPM
		schedule_delayed_work(&ts->reset_work,
				      msecs_to_jiffies(TOUCH_RESET_DWORK_TIME));
#else
		sec_ts_set_lowpowermode(ts, TO_TOUCH_MODE);
#endif
		ts->power_status = SEC_TS_STATE_POWER_ON;
	} else {
		ret = sec_ts_start_device(ts);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: Failed to start device\n", __func__);
	}

	/* because edge and dead zone will recover soon */
	sec_ts_set_grip_type(ts, ONLY_EDGE_HANDLER);

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_INPUT_DEV, false);

	return 0;
}

static void sec_ts_input_close(struct input_dev *dev)
{
	struct sec_ts_data *ts = input_get_drvdata(dev);

	ts->input_closed = true;

	input_info(true, &ts->client->dev, "%s\n", __func__);

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_INPUT_DEV, true);

	cancel_work_sync(&ts->suspend_work);
	cancel_work_sync(&ts->resume_work);

#ifdef USE_POWER_RESET_WORK
	cancel_delayed_work(&ts->reset_work);
#endif

#ifndef CONFIG_SEC_FACTORY
	ts->lowpower_mode |= SEC_TS_MODE_CUSTOMLIB_FORCE_KEY;
#endif
	if (ts->lowpower_mode) {
		sec_ts_set_lowpowermode(ts, TO_LOWPOWER_MODE);
		ts->power_status = SEC_TS_STATE_LPM;
	} else {
		sec_ts_stop_device(ts);
	}

	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_INPUT_DEV, false);
}
#endif

#ifdef I2C_INTERFACE
static int sec_ts_remove(struct i2c_client *client)
#else
static int sec_ts_remove(struct spi_device *client)
#endif
{
#ifdef I2C_INTERFACE
	struct sec_ts_data *ts = i2c_get_clientdata(client);
#else
	struct sec_ts_data *ts = spi_get_drvdata(client);
#endif
	const struct sec_ts_plat_data *pdata = ts->plat_data;

	input_info(true, &ts->client->dev, "%s\n", __func__);

	if (ts_dup == NULL || ts->probe_done == false)
		return 0;

	/* Force the bus active throughout removal of the client */
	sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_FORCE_ACTIVE, true);

	power_supply_unreg_notifier(&ts->psy_nb);
	drm_panel_notifier_unregister(pdata->panel, &ts->notifier);

	cancel_work_sync(&ts->suspend_work);
	cancel_work_sync(&ts->resume_work);
	cancel_work_sync(&ts->charger_work);
	destroy_workqueue(ts->event_wq);

#ifdef SEC_TS_FW_UPDATE_ON_PROBE
	cancel_work_sync(&ts->fw_update_work);
#else
	cancel_delayed_work_sync(&ts->fw_update_work);
	destroy_workqueue(ts->fw_update_wq);
#endif

	disable_irq_nosync(ts->client->irq);
	free_irq(ts->client->irq, ts);
	input_info(true, &ts->client->dev, "%s: irq disabled\n", __func__);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	touch_offload_cleanup(&ts->offload);
#endif

#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
	heatmap_remove(&ts->v4l2);
#endif

	pm_qos_remove_request(&ts->pm_touch_req);
	pm_qos_remove_request(&ts->pm_spi_req);

#ifdef USE_POWER_RESET_WORK
	cancel_delayed_work_sync(&ts->reset_work);
	flush_delayed_work(&ts->reset_work);

	input_info(true, &ts->client->dev, "%s: flush queue\n", __func__);

#endif

	sec_ts_fn_remove(ts);

#ifdef CONFIG_TOUCHSCREEN_DUMP_MODE
	p_ghost_check = NULL;
#endif
	device_init_wakeup(&client->dev, false);

	ts->lowpower_mode = false;
	ts->probe_done = false;

	if (ts->plat_data->support_dex) {
		input_mt_destroy_slots(ts->input_dev_pad);
		input_unregister_device(ts->input_dev_pad);
	}

	ts->input_dev = ts->input_dev_touch;
	input_mt_destroy_slots(ts->input_dev);
	input_unregister_device(ts->input_dev);

	ts->input_dev_pad = NULL;
	ts->input_dev = NULL;
	ts->input_dev_touch = NULL;
	ts_dup = NULL;

	/* need to do software reset for next sec_ts_probe() without error */
	ts->sec_ts_write(ts, SEC_TS_CMD_SW_RESET, NULL, 0);

	ts->plat_data->power(ts, false);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_TBN)
	tbn_cleanup(ts->tbn);
#endif

	if (gpio_is_valid(ts->plat_data->irq_gpio))
		gpio_free(ts->plat_data->irq_gpio);
	if (gpio_is_valid(ts->plat_data->switch_gpio))
		gpio_free(ts->plat_data->switch_gpio);
	if (gpio_is_valid(ts->plat_data->reset_gpio))
		gpio_free(ts->plat_data->reset_gpio);

	sec_ts_raw_device_exit(ts);
#ifndef CONFIG_SEC_SYSFS
	class_destroy(sec_class);
#endif

#if IS_ENABLED(CONFIG_TOUCHSCREEN_HEATMAP)
	kfree(ts->heatmap_buff);
#endif
	kfree(ts->gainTable);
	kfree(ts->pFrame);
	kfree(ts);
	return 0;
}

#ifdef I2C_INTERFACE
static void sec_ts_shutdown(struct i2c_client *client)
#else
static void sec_ts_shutdown(struct spi_device *client)
#endif
{
	pr_info("%s\n", __func__);
	if (ts_dup)
		sec_ts_remove(client);
}

int sec_ts_stop_device(struct sec_ts_data *ts)
{
	input_info(true, &ts->client->dev, "%s\n", __func__);

	mutex_lock(&ts->device_mutex);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		input_err(true, &ts->client->dev,
			  "%s: already power off\n", __func__);
		goto out;
	}

	ts->power_status = SEC_TS_STATE_POWER_OFF;

	disable_irq(ts->client->irq);
	sec_ts_locked_release_all_finger(ts);

	ts->plat_data->power(ts, false);

	if (ts->plat_data->enable_sync)
		ts->plat_data->enable_sync(false);

	sec_ts_pinctrl_configure(ts, false);

out:
	mutex_unlock(&ts->device_mutex);
	return 0;
}

int sec_ts_start_device(struct sec_ts_data *ts)
{
	int ret;

	input_info(true, &ts->client->dev, "%s\n", __func__);

	sec_ts_pinctrl_configure(ts, true);

	mutex_lock(&ts->device_mutex);

	if (ts->power_status == SEC_TS_STATE_POWER_ON) {
		input_err(true, &ts->client->dev,
			  "%s: already power on\n", __func__);
		goto out;
	}

	sec_ts_locked_release_all_finger(ts);

	ts->plat_data->power(ts, true);
	sec_ts_delay(70);
	ts->power_status = SEC_TS_STATE_POWER_ON;
	sec_ts_wait_for_ready(ts, SEC_TS_ACK_BOOT_COMPLETE);

	if (ts->plat_data->enable_sync)
		ts->plat_data->enable_sync(true);

	if (ts->flip_enable) {
		ret = sec_ts_write(ts, SEC_TS_CMD_SET_COVERTYPE,
				   &ts->cover_cmd, 1);

		ts->touch_functions = ts->touch_functions |
				SEC_TS_BIT_SETFUNC_COVER;
		input_info(true, &ts->client->dev,
				"%s: cover cmd write type:%d, mode:%x, ret:%d",
				__func__, ts->touch_functions,
				ts->cover_cmd, ret);
	} else {
		ts->touch_functions = (ts->touch_functions &
				       (~SEC_TS_BIT_SETFUNC_COVER));
		input_info(true, &ts->client->dev,
			"%s: cover open, not send cmd", __func__);
	}

	ts->touch_functions = ts->touch_functions |
				SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC;
	ret = sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
			   (u8 *)&ts->touch_functions, 2);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			"%s: Failed to send touch function command", __func__);

	#ifdef SEC_TS_SUPPORT_CUSTOMLIB
	if (ts->use_customlib)
		sec_ts_set_custom_library(ts);
	#endif

	sec_ts_set_grip_type(ts, ONLY_EDGE_HANDLER);

	if (ts->dex_mode) {
		input_info(true, &ts->client->dev,
			   "%s: set dex mode\n", __func__);
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_DEX_MODE,
				       &ts->dex_mode, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: failed to set dex mode %x\n",
				__func__, ts->dex_mode);
	}

	if (ts->brush_mode) {
		input_info(true, &ts->client->dev,
			   "%s: set brush mode\n", __func__);
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_BRUSH_MODE,
				       &ts->brush_mode, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: failed to set brush mode\n", __func__);
	}

	if (ts->touchable_area) {
		input_info(true, &ts->client->dev,
			   "%s: set 16:9 mode\n", __func__);
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHABLE_AREA,
				       &ts->touchable_area, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				"%s: failed to set 16:9 mode\n", __func__);
	}

	/* Sense_on */
	ret = sec_ts_write(ts, SEC_TS_CMD_SENSE_ON, NULL, 0);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: fail to write Sense_on\n", __func__);

	enable_irq(ts->client->irq);

out:
	mutex_unlock(&ts->device_mutex);
	return 0;
}

#ifdef CONFIG_PM
static int sec_ts_pm_suspend(struct device *dev)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	if (ts->bus_refmask)
		input_info(true, &ts->client->dev,
			"%s: bus_refmask 0x%X\n", __func__, ts->bus_refmask);

	if (ts->power_status != SEC_TS_STATE_SUSPEND) {
		input_err(true, &ts->client->dev,
			"%s: can't suspend because touch bus is in use!\n",
			__func__);
		return -EBUSY;
	}

	if (ts->lowpower_mode)
		reinit_completion(&ts->resume_done);

	return 0;
}

static int sec_ts_pm_resume(struct device *dev)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	if (ts->lowpower_mode)
		complete_all(&ts->resume_done);

	return 0;
}
#endif

static const struct i2c_device_id sec_ts_id[] = {
	{ SEC_TS_NAME, 0 },
	{ },
};

#ifdef CONFIG_PM
static const struct dev_pm_ops sec_ts_dev_pm_ops = {
	.suspend = sec_ts_pm_suspend,
	.resume = sec_ts_pm_resume,
};
#endif

/*
 * Configure the switch GPIO to toggle bus master between AP and SLPI.
 * gpio_value takes one of
 * { SEC_SWITCH_GPIO_VALUE_SLPI_MASTER, SEC_SWITCH_GPIO_VALUE_AP_MASTER }
 */
static void sec_set_switch_gpio(struct sec_ts_data *ts, int gpio_value)
{
	int retval;
	unsigned int gpio = ts->plat_data->switch_gpio;

	if (!gpio_is_valid(gpio))
		return;

	input_dbg(true, &ts->client->dev, "%s: toggling switch to %s\n",
		   __func__, gpio_value == SEC_SWITCH_GPIO_VALUE_AP_MASTER ?
		   "AP" : "SLPI");

	retval = gpio_direction_output(gpio, gpio_value);
	if (retval < 0)
		input_err(true, &ts->client->dev,
			  "%s: Failed to toggle switch_gpio, err = %d\n",
			  __func__, retval);
}

static void sec_ts_suspend_work(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
					      suspend_work);
	int ret = 0;

	input_dbg(true, &ts->client->dev, "%s\n", __func__);

	mutex_lock(&ts->device_mutex);

	reinit_completion(&ts->bus_resumed);

	if (ts->power_status == SEC_TS_STATE_SUSPEND) {
		input_err(true, &ts->client->dev, "%s: already suspended.\n",
			  __func__);
		mutex_unlock(&ts->device_mutex);
		return;
	}

	pm_stay_awake(&ts->client->dev);

	/* Stop T-IC */
	sec_ts_fix_tmode(ts, TOUCH_SYSTEM_MODE_SLEEP, TOUCH_MODE_STATE_STOP);
	ret = sec_ts_write(ts, SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			"%s: write clear event failed\n", __func__);

	disable_irq_nosync(ts->client->irq);
	sec_ts_locked_release_all_finger(ts);

	if (ts->plat_data->enable_sync)
		ts->plat_data->enable_sync(false);

	ts->power_status = SEC_TS_STATE_SUSPEND;

	sec_ts_pinctrl_configure(ts, false);

	sec_set_switch_gpio(ts, SEC_SWITCH_GPIO_VALUE_SLPI_MASTER);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_TBN)
	if (ts->tbn)
		tbn_release_bus(ts->tbn);
#endif
	pm_relax(&ts->client->dev);
	mutex_unlock(&ts->device_mutex);
}

static void sec_ts_resume_work(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
					      resume_work);
	int ret = 0;

	input_dbg(true, &ts->client->dev, "%s\n", __func__);

	mutex_lock(&ts->device_mutex);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_TBN)
	if (ts->tbn)
		tbn_request_bus(ts->tbn);
#endif

	sec_set_switch_gpio(ts, SEC_SWITCH_GPIO_VALUE_AP_MASTER);

	sec_ts_pinctrl_configure(ts, true);

	if (ts->power_status == SEC_TS_STATE_POWER_ON) {
		input_err(true, &ts->client->dev, "%s: already resumed.\n",
			  __func__);
		mutex_unlock(&ts->device_mutex);
		return;
	}

	sec_ts_locked_release_all_finger(ts);

	ts->power_status = SEC_TS_STATE_POWER_ON;

	ret = sec_ts_system_reset(ts);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			"%s: reset failed! ret %d\n", __func__, ret);

	if (ts->plat_data->enable_sync)
		ts->plat_data->enable_sync(true);

	ts->touch_functions =
	    ts->touch_functions | SEC_TS_DEFAULT_ENABLE_BIT_SETFUNC;
	ret = sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION,
			       (u8 *)&ts->touch_functions, 2);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: Failed to send touch function command.",
			  __func__);

#ifdef SEC_TS_SUPPORT_CUSTOMLIB
	if (ts->use_customlib)
		sec_ts_set_custom_library(ts);
#endif

	sec_ts_set_grip_type(ts, ONLY_EDGE_HANDLER);

	if (ts->dex_mode) {
		input_info(true, &ts->client->dev, "%s: set dex mode.\n",
			   __func__);
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_DEX_MODE,
					   &ts->dex_mode, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: failed to set dex mode %x.\n", __func__,
				  ts->dex_mode);
	}

	if (ts->brush_mode) {
		input_info(true, &ts->client->dev, "%s: set brush mode.\n",
			   __func__);
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_BRUSH_MODE,
					   &ts->brush_mode, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: failed to set brush mode.\n", __func__);
	}

	if (ts->touchable_area) {
		input_info(true, &ts->client->dev, "%s: set 16:9 mode.\n",
			   __func__);
		ret = ts->sec_ts_write(ts, SEC_TS_CMD_SET_TOUCHABLE_AREA,
					   &ts->touchable_area, 1);
		if (ret < 0)
			input_err(true, &ts->client->dev,
				  "%s: failed to set 16:9 mode.\n", __func__);
	}

	/* set charger mode */
	ret = ts->sec_ts_write(ts, SET_TS_CMD_SET_CHARGER_MODE,
			       &ts->charger_mode, 1);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: write reg %#x %#x failed, returned %i\n",
			__func__, SET_TS_CMD_SET_CHARGER_MODE, ts->charger_mode,
			ret);
	else
		input_dbg(true, &ts->client->dev, "%s: set charger mode %#x\n",
			__func__, ts->charger_mode);
	queue_work(ts->event_wq, &ts->charger_work);

	/* Sense_on */
	ret = sec_ts_write(ts, SEC_TS_CMD_SENSE_ON, NULL, 0);
	if (ret < 0)
		input_err(true, &ts->client->dev,
			  "%s: failed to write Sense_on.\n", __func__);

#if IS_ENABLED(CONFIG_TOUCHSCREEN_OFFLOAD)
	/* Set touch_offload configuration */
	if (ts->offload.offload_running) {
		input_info(true, &ts->client->dev,
			   "applying touch_offload settings.\n");

		if (!ts->offload.config.filter_grip)
			sec_ts_enable_grip(ts, false);
	}
#endif

	enable_irq(ts->client->irq);

	complete_all(&ts->bus_resumed);

	mutex_unlock(&ts->device_mutex);
}

static void sec_ts_charger_work(struct work_struct *work)
{
	int ret;
	union power_supply_propval prop = {0,};
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
					      charger_work);
	u8 charger_mode = SEC_TS_BIT_CHARGER_MODE_NO;
	bool usb_present = ts->usb_present;
	bool wlc_online = ts->wlc_online;

	/* usb case */
	ret = power_supply_get_property(ts->usb_psy,
					POWER_SUPPLY_PROP_PRESENT, &prop);
	if (ret == 0) {
		usb_present = !!prop.intval;
		if (usb_present)
			charger_mode = SEC_TS_BIT_CHARGER_MODE_WIRE_CHARGER;
	}

	/* wlc case */
	ret = power_supply_get_property(ts->wireless_psy,
					POWER_SUPPLY_PROP_ONLINE, &prop);
	if (ret == 0) {
		wlc_online = !!prop.intval;
		if (wlc_online)
			charger_mode = SEC_TS_BIT_CHARGER_MODE_WIRELESS_CHARGER;
	}

	/* rtx case */
	ret = power_supply_get_property(ts->wireless_psy,
					POWER_SUPPLY_PROP_RTX, &prop);
	if (ret == 0)
		pr_debug("%s: RTX %s", __func__,
			(!!prop.intval) ? "ON" : "OFF");

	if (usb_present == ts->usb_present &&
	    wlc_online == ts->wlc_online &&
	    ts->keep_wlc_mode == false)
		return;

	/* keep wlc mode if usb plug in w/ wlc off case */
	if (ts->keep_wlc_mode) {
		input_dbg(true, &ts->client->dev,
			   "keep wlc mode after usb plug in during wlc online");
		charger_mode = SEC_TS_BIT_CHARGER_MODE_WIRELESS_CHARGER;
	}

	input_dbg(true, &ts->client->dev,
		"%s: keep_wlc_mode %d, USB(%d->%d), WLC(%d->%d), charger_mode(%#x->%#x)",
		__func__,
		ts->keep_wlc_mode,
		ts->usb_present, usb_present,
		ts->wlc_online, wlc_online,
		ts->charger_mode, charger_mode);

	if (ts->charger_mode != charger_mode) {
		if (ts->power_status == SEC_TS_STATE_POWER_ON) {
			ret = ts->sec_ts_write(ts, SET_TS_CMD_SET_CHARGER_MODE,
				       &charger_mode, 1);
			if (ret < 0) {
				input_err(true, &ts->client->dev,
				"%s: write reg %#x %#x failed, returned %i\n",
				__func__, SET_TS_CMD_SET_CHARGER_MODE,
				charger_mode, ret);
				return;
			}

			input_dbg(true, &ts->client->dev,
				"%s: charger_mode change from %#x to %#x\n",
				__func__, ts->charger_mode, charger_mode);
		} else {
			input_dbg(true, &ts->client->dev,
				"%s: ONLY update charger_mode status from %#x to %#x, then will apply during resume\n",
				__func__, ts->charger_mode, charger_mode);
		}
		ts->charger_mode = charger_mode;
	}

	/* update final charger state */
	ts->wlc_online = wlc_online;
	ts->usb_present = usb_present;
	ts->keep_wlc_mode = false;
}

static void sec_ts_aggregate_bus_state(struct sec_ts_data *ts)
{
	input_dbg(true, &ts->client->dev, "%s: bus_refmask = 0x%02X.\n",
		  __func__, ts->bus_refmask);

	/* Complete or cancel any outstanding transitions */
	cancel_work_sync(&ts->suspend_work);
	cancel_work_sync(&ts->resume_work);

	if ((ts->bus_refmask == 0 &&
		ts->power_status == SEC_TS_STATE_SUSPEND) ||
	    (ts->bus_refmask != 0 &&
		ts->power_status != SEC_TS_STATE_SUSPEND))
		return;

	if (ts->bus_refmask == 0)
		queue_work(ts->event_wq, &ts->suspend_work);
	else
		queue_work(ts->event_wq, &ts->resume_work);
}

int sec_ts_set_bus_ref(struct sec_ts_data *ts, u16 ref, bool enable)
{
	int result = 0;

	mutex_lock(&ts->bus_mutex);

	input_dbg(true, &ts->client->dev, "%s: bus_refmask = 0x%02X.\n",
		  __func__, ref);

	if ((enable && (ts->bus_refmask & ref)) ||
	    (!enable && !(ts->bus_refmask & ref))) {
		input_info(true, &ts->client->dev,
			"%s: reference is unexpectedly set: mask=0x%04X, ref=0x%04X, enable=%d.\n",
			__func__, ts->bus_refmask, ref, enable);
		mutex_unlock(&ts->bus_mutex);
		return -EINVAL;
	}

	if (enable) {
		/* IRQs can only keep the bus active. IRQs received while the
		 * bus is transferred to SLPI should be ignored.
		 */
		if (ref == SEC_TS_BUS_REF_IRQ && ts->bus_refmask == 0)
			result = -EAGAIN;
		else
			ts->bus_refmask |= ref;
	} else
		ts->bus_refmask &= ~ref;
	sec_ts_aggregate_bus_state(ts);

	mutex_unlock(&ts->bus_mutex);

	/* When triggering a wake, wait up to one second to resume. SCREEN_ON
	 * and IRQ references do not need to wait.
	 */
	if (enable &&
	    ref != SEC_TS_BUS_REF_SCREEN_ON && ref != SEC_TS_BUS_REF_IRQ) {
		wait_for_completion_timeout(&ts->bus_resumed, HZ);
		if (ts->power_status != SEC_TS_STATE_POWER_ON) {
			input_info(true, &ts->client->dev,
				   "%s: Failed to wake the touch bus.\n",
				   __func__);
			result = -ETIMEDOUT;
		}
	}

	return result;
}

static int sec_ts_screen_state_chg_callback(struct notifier_block *nb,
					    unsigned long val, void *data)
{
	struct sec_ts_data *ts = container_of(nb, struct sec_ts_data,
					      notifier);
	struct drm_panel_notifier *evdata = (struct drm_panel_notifier *)data;
	unsigned int blank;

	input_dbg(true, &ts->client->dev, "%s: enter.\n", __func__);

	if (val != DRM_PANEL_EVENT_BLANK && val != DRM_PANEL_EARLY_EVENT_BLANK)
		return NOTIFY_DONE;

	if (!ts || !evdata || !evdata->data) {
		input_err(true, &ts->client->dev,
			  "%s: Bad screen state change notifier call.\n",
			  __func__);
		return NOTIFY_DONE;
	}

	blank = *((unsigned int *)evdata->data);
	switch (blank) {
	case DRM_PANEL_BLANK_POWERDOWN:
	case DRM_PANEL_BLANK_LP:
		if (val == DRM_PANEL_EARLY_EVENT_BLANK) {
			input_dbg(true, &ts->client->dev,
				  "%s: DRM_PANEL_BLANK_POWERDOWN.\n", __func__);
			sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_SCREEN_ON, false);
		}
		break;
	case DRM_PANEL_BLANK_UNBLANK:
		if (val == DRM_PANEL_EVENT_BLANK) {
			input_dbg(true, &ts->client->dev,
				  "%s: DRM_PANEL_BLANK_UNBLANK.\n", __func__);
			sec_ts_set_bus_ref(ts, SEC_TS_BUS_REF_SCREEN_ON, true);
		}
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block sec_ts_screen_nb = {
	.notifier_call = sec_ts_screen_state_chg_callback,
};

/*
 * power supply callback
 */
static int sec_ts_psy_cb(struct notifier_block *nb,
			       unsigned long val, void *data)
{
	u64 debounce = 500;
	struct sec_ts_data *ts = container_of(nb, struct sec_ts_data, psy_nb);

	pr_debug("%s: val %lu", __func__, val);

	if (val != PSY_EVENT_PROP_CHANGED ||
	    ts->wireless_psy == NULL ||
	    ts->usb_psy == NULL ||
	    (ts->wireless_psy != data && ts->usb_psy != data) ||
	    ts->ignore_charger_nb == 1)
		return NOTIFY_OK;

	if (ts->usb_psy == data) {
		ts->usb_changed_timestamp = ktime_get();
		if (ts->wlc_online) {
			input_dbg(true, &ts->client->dev,
				"%s: ignore this usb_psy changed during wlc_online!",
				__func__);
			return NOTIFY_OK;
		}
	}

	if (ts->wireless_psy == data) {
		/* keep wlc mode after usb plug in during wlc online */
		if (ts->wlc_online == true &&
		    ts->usb_present == false &&
		    ktime_before(ktime_get(),
			ktime_add_ms(ts->usb_changed_timestamp, debounce)))
			ts->keep_wlc_mode = true;
	}

	if (ts->power_status == SEC_TS_STATE_POWER_ON)
		queue_work(ts->event_wq, &ts->charger_work);

	return NOTIFY_OK;
}

static struct notifier_block sec_ts_psy_nb = {
	.notifier_call = sec_ts_psy_cb,
};

#ifdef CONFIG_OF
static const struct of_device_id sec_ts_match_table[] = {
	{ .compatible = "sec,sec_ts",},
	{ },
};
#else
#define sec_ts_match_table NULL
#endif

#ifdef I2C_INTERFACE
static struct i2c_driver sec_ts_driver = {
	.probe		= sec_ts_probe,
	.remove		= sec_ts_remove,
	.shutdown	= sec_ts_shutdown,
	.id_table	= sec_ts_id,
	.driver = {
		.owner	= THIS_MODULE,
		.name	= SEC_TS_NAME,
#ifdef CONFIG_OF
		.of_match_table = sec_ts_match_table,
#endif
#ifdef CONFIG_PM
		.pm = &sec_ts_dev_pm_ops,
#endif
	},
};
#else
static struct spi_driver sec_ts_driver = {
	.probe    = sec_ts_probe,
	.remove   = sec_ts_remove,
	.shutdown   = sec_ts_shutdown,
	.driver   = {
		.owner  = THIS_MODULE,
		.name = SEC_TS_NAME,
#ifdef CONFIG_OF
		.of_match_table = sec_ts_match_table,
#endif
#ifdef CONFIG_PM
		.pm = &sec_ts_dev_pm_ops,
#endif
	},
};
#endif


static int __init sec_ts_init(void)
{
#ifdef CONFIG_BATTERY_SAMSUNG
	if (lpcharge == 1) {
		pr_err("%s %s: Do not load driver due to : lpm %d\n",
				SECLOG, __func__, lpcharge);
		return -ENODEV;
	}
#endif
	pr_err("%s %s\n", SECLOG, __func__);

#ifdef I2C_INTERFACE
	return i2c_add_driver(&sec_ts_driver);
#else
	return spi_register_driver(&sec_ts_driver);
#endif
}

static void __exit sec_ts_exit(void)
{

#ifdef I2C_INTERFACE
	i2c_del_driver(&sec_ts_driver);
#else
	spi_unregister_driver(&sec_ts_driver);
#endif
}

MODULE_AUTHOR("Hyobae, Ahn<hyobae.ahn@samsung.com>");
MODULE_DESCRIPTION("Samsung Electronics TouchScreen driver");
MODULE_LICENSE("GPL");

module_init(sec_ts_init);
module_exit(sec_ts_exit);

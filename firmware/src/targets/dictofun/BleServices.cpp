/*
 * Copyright (c) 2021 Roman Turkin 
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "BleServices.h"

#include <ble_services/ble_lbs/ble_lbs.h>
#include <ble/nrf_ble_qwr/nrf_ble_qwr.h>
#include <ble_file_transfer_service.h>
#include <boards/boards.h>
#include <nrf_log.h>
#include "spi_access.h"
#include "drv_audio.h"

#include "nrf_dfu_ble_svci_bond_sharing.h"
#include "nrf_svci_async_function.h"
#include "nrf_svci_async_handler.h"

#include "ble_dfu.h"
#include "nrf_bootloader_info.h"
#include "nrf_sdh.h"
#include "nrf_power.h"
#include "nrf_pwr_mgmt.h"
#include "ble_advertising.h"

#include "nrf_ble_gatt.h"
#include "nrf_ble_qwr.h"

NRF_BLE_QWR_DEF(m_qwr);
BLE_LBS_DEF(m_lbs);
BLE_FTS_DEF(m_fts, NRF_SDH_BLE_TOTAL_LINK_COUNT);

#define APP_ADV_INTERVAL                300
#define APP_ADV_DURATION                18000

BLE_ADVERTISING_DEF(m_advertising);

namespace ble
{

BleServices * BleServices::_instance{nullptr};

static void nrf_qwr_error_handler(uint32_t nrf_error)
{
    APP_ERROR_HANDLER(nrf_error);
}

static void led_write_handler(uint16_t conn_handle, ble_lbs_t * p_lbs, uint8_t led_state);

static void fts_data_handler(ble_fts_t * p_fts, uint8_t const * p_data, uint16_t length)
{
    BleServices::getInstance().handleFtsData(p_fts, p_data, length);
}

void BleServices::handleFtsData(ble_fts_t * p_fts, uint8_t const * p_data, uint16_t length)
{
    switch(p_data[0])
    {
        case CMD_GET_FILE:
            NRF_LOG_INFO("CMD_GET_FILE");
            _ble_cmd = (ble::BleCommands)p_data[0];
            break;
        case CMD_GET_FILE_INFO:
            NRF_LOG_INFO("CMD_GET_FILE_INFO");
            _ble_cmd = (ble::BleCommands)p_data[0];
            break;
        default:
            NRF_LOG_ERROR("Unknown command: %02x", p_data[0]);
            break;
    }
}

uint32_t BleServices::send_data(const uint8_t *data, uint32_t data_size)
{
    if (data_size > 0)
    {
        unsigned i = 0;

        uint32_t size_left = data_size;
        uint8_t *send_buffer = (uint8_t *)data;

        while (size_left)
        {
            uint8_t send_size = MIN(size_left, BLE_ITS_MAX_DATA_LEN);
            size_left -= send_size;

            uint32_t err_code = NRF_SUCCESS;
            while (true)
            {
                err_code = ble_fts_send_file_fragment(&m_fts, send_buffer, send_size);
                if (err_code == NRF_SUCCESS)
                {
                    break;
                }
                else if (err_code != NRF_ERROR_RESOURCES)
                {
                    NRF_LOG_ERROR("Failed to send file, err = %d", err_code);
                    return err_code;
                }
            }

            send_buffer += send_size;
        }
    }

    return NRF_SUCCESS;
}

// DFU-related stuff
static void buttonless_dfu_sdh_state_observer(nrf_sdh_state_evt_t state, void * p_context)
{
    if (state == NRF_SDH_EVT_STATE_DISABLED)
    {
        // Softdevice was disabled before going into reset. Inform bootloader to skip CRC on next boot.
        nrf_power_gpregret2_set(BOOTLOADER_DFU_SKIP_CRC);

        //Go to system off.
        nrf_pwr_mgmt_shutdown(NRF_PWR_MGMT_SHUTDOWN_GOTO_SYSOFF);
    }
}

/* nrf_sdh state observer. */
NRF_SDH_STATE_OBSERVER(m_buttonless_dfu_state_obs, 0) =
{
    .handler = buttonless_dfu_sdh_state_observer,
};

static void advertising_config_get(ble_adv_modes_config_t * p_config)
{
    memset(p_config, 0, sizeof(ble_adv_modes_config_t));

    p_config->ble_adv_fast_enabled  = true;
    p_config->ble_adv_fast_interval = APP_ADV_INTERVAL;
    p_config->ble_adv_fast_timeout  = APP_ADV_DURATION;
}

static void disconnect(uint16_t conn_handle, void * p_context)
{
    UNUSED_PARAMETER(p_context);

    ret_code_t err_code = sd_ble_gap_disconnect(conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    if (err_code != NRF_SUCCESS)
    {
        NRF_LOG_WARNING("Failed to disconnect connection. Connection handle: %d Error: %d", conn_handle, err_code);
    }
    else
    {
        NRF_LOG_DEBUG("Disconnected connection handle %d", conn_handle);
    }
}

static void ble_dfu_evt_handler(ble_dfu_buttonless_evt_type_t event)
{
    switch (event)
    {
        case BLE_DFU_EVT_BOOTLOADER_ENTER_PREPARE:
        {
            NRF_LOG_INFO("Device is preparing to enter bootloader mode.");

            // Prevent device from advertising on disconnect.
            ble_adv_modes_config_t config;
            advertising_config_get(&config);
            config.ble_adv_on_disconnect_disabled = true;
            ble_advertising_modes_config_set(&m_advertising, &config);

            // Disconnect all other bonded devices that currently are connected.
            // This is required to receive a service changed indication
            // on bootup after a successful (or aborted) Device Firmware Update.
            uint32_t conn_count = ble_conn_state_for_each_connected(disconnect, NULL);
            NRF_LOG_INFO("Disconnected %d links.", conn_count);
            break;
        }

        case BLE_DFU_EVT_BOOTLOADER_ENTER:
            // YOUR_JOB: Write app-specific unwritten data to FLASH, control finalization of this
            //           by delaying reset by reporting false in app_shutdown_handler
            NRF_LOG_INFO("Device will enter bootloader mode.");
            break;

        case BLE_DFU_EVT_BOOTLOADER_ENTER_FAILED:
            NRF_LOG_ERROR("Request to enter bootloader mode failed asynchroneously.");
            // YOUR_JOB: Take corrective measures to resolve the issue
            //           like calling APP_ERROR_CHECK to reset the device.
            break;

        case BLE_DFU_EVT_RESPONSE_SEND_ERROR:
            NRF_LOG_ERROR("Request to send a response to client failed.");
            // YOUR_JOB: Take corrective measures to resolve the issue
            //           like calling APP_ERROR_CHECK to reset the device.
            APP_ERROR_CHECK(false);
            break;

        default:
            NRF_LOG_ERROR("Unknown event from ble_dfu_buttonless.");
            break;
    }
}

BleServices::BleServices() 
{
    _instance = this;
}

void BleServices::init()
{
    ret_code_t         err_code;
    ble_lbs_init_t     lbs_init = {0};
    ble_fts_init_t     fts_init = {0};
    nrf_ble_qwr_init_t qwr_init = {0};
    ble_dfu_buttonless_init_t dfus_init = {0};

    // Initialize Queued Write Module.
    qwr_init.error_handler = nrf_qwr_error_handler;

    err_code = nrf_ble_qwr_init(&m_qwr, &qwr_init);
    APP_ERROR_CHECK(err_code);

    // Initialize LBS.
    lbs_init.led_write_handler = led_write_handler;

    err_code = ble_lbs_init(&m_lbs, &lbs_init);
    APP_ERROR_CHECK(err_code);

    // Initialize FTS.
    fts_init.data_handler = fts_data_handler;

    err_code = ble_fts_init(&m_fts, &fts_init);
    APP_ERROR_CHECK(err_code);

    // Initiaize DFU service
    dfus_init.evt_handler = ble_dfu_evt_handler;
    err_code = ble_dfu_buttonless_init(&dfus_init);
    APP_ERROR_CHECK(err_code);

}

nrf_ble_qwr_t * BleServices::getQwrHandle()
{
    return &m_qwr;
}

// TODO: assert nullptr on uuids, assert when max_uuids is not enough
size_t BleServices::setAdvUuids(ble_uuid_t * uuids, size_t max_uuids)
{
    uuids[0] = {LBS_UUID_SERVICE, m_lbs.uuid_type};
    return 1U;
}

#define LEDBUTTON_LED                   BSP_BOARD_LED_2

static void led_write_handler(uint16_t conn_handle, ble_lbs_t * p_lbs, uint8_t led_state)
{
    if (led_state)
    {
        NRF_LOG_INFO("Received LED ON!");
    }
    else
    {
        NRF_LOG_INFO("Received LED OFF!");
    }
}

void BleServices::cyclic()
{
    switch(_ble_cmd)
    {
        case CMD_GET_FILE:
        {
            if (_read_pointer == 0) // TODO change to file start
            {
                NRF_LOG_INFO("Starting file sending...");
            }

            if (!_file_size || _read_pointer >= _file_size)
            {
                _is_file_transmission_done = true;
                _ble_cmd = CMD_EMPTY;
                NRF_LOG_INFO("File have been sent.");
                break;
            }

            uint8_t buffer[SPI_READ_SIZE];
            spi_flash_trigger_read(_read_pointer, sizeof(buffer));
            while (spi_flash_is_spi_bus_busy());
            spi_flash_copy_received_data(buffer, sizeof(buffer));
            
            if(_read_pointer == 0)
            {
              drv_audio_wav_header_apply(buffer, _file_size / 2);
            }

            send_data(buffer, sizeof(buffer));

            _read_pointer += 2 * SPI_READ_SIZE;
            break;
        }
        case CMD_GET_FILE_INFO:
        {
            NRF_LOG_INFO("Sending file info, size %d (%X)", _file_size, _file_size);

            ble_fts_file_info_t file_info;
            file_info.file_size_bytes = _file_size / 2;

            ble_fts_file_info_send(&m_fts, &file_info);
            _ble_cmd = CMD_EMPTY;

            break;
        }
        default:
            _ble_cmd = CMD_EMPTY;
    }
}


}
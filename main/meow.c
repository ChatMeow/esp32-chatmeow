/* Record WAV file to SD Card

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_http_client.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "board.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "wav_encoder.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "periph_wifi.h"
#include "filter_resample.h"
#include "input_key_service.h"
#include "audio_idf_version.h"
#include "wav_decoder.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "MEOW_CHAT";

#define EXAMPLE_AUDIO_SAMPLE_RATE (16000)
#define EXAMPLE_AUDIO_BITS (16)
#define EXAMPLE_AUDIO_CHANNELS (1)

#define DEMO_EXIT_BIT (BIT0)

static EventGroupHandle_t EXIT_FLAG;

uint8_t isPlaying = 0;

audio_pipeline_handle_t play_pipeline, recording_pipeline;
audio_element_handle_t i2s_stream_reader, http_stream_writer;
audio_element_handle_t http_stream_reader, i2s_stream_writer, wav_decoder;
audio_event_iface_handle_t play_evt;

esp_err_t _http_stream_event_handle(http_stream_event_msg_t *msg)
{

    esp_http_client_handle_t http = (esp_http_client_handle_t)msg->http_client;
    char len_buf[16];
    static int total_write = 0;

    if (msg->event_id == HTTP_STREAM_PRE_REQUEST)
    {
        // set header
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_PRE_REQUEST, lenght=%d", msg->buffer_len);
        esp_http_client_set_method(http, HTTP_METHOD_POST);
        char dat[10] = {0};
        snprintf(dat, sizeof(dat), "%d", EXAMPLE_AUDIO_SAMPLE_RATE);
        esp_http_client_set_header(http, "x-audio-sample-rates", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", EXAMPLE_AUDIO_BITS);
        esp_http_client_set_header(http, "x-audio-bits", dat);
        memset(dat, 0, sizeof(dat));
        snprintf(dat, sizeof(dat), "%d", EXAMPLE_AUDIO_CHANNELS);
        esp_http_client_set_header(http, "x-audio-channel", dat);
        total_write = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_REQUEST)
    {
        // write data
        int wlen = sprintf(len_buf, "%x\r\n", msg->buffer_len);
        if (esp_http_client_write(http, len_buf, wlen) <= 0)
        {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, msg->buffer, msg->buffer_len) <= 0)
        {
            return ESP_FAIL;
        }
        if (esp_http_client_write(http, "\r\n", 2) <= 0)
        {
            return ESP_FAIL;
        }
        total_write += msg->buffer_len;
        printf("\033[A\33[2K\rTotal bytes written: %d\n", total_write);
        return msg->buffer_len;
    }

    if (msg->event_id == HTTP_STREAM_POST_REQUEST)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_POST_REQUEST, write end chunked marker");
        if (esp_http_client_write(http, "0\r\n\r\n", 5) <= 0)
        {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_REQUEST)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_REQUEST");
        char *buf = calloc(1, 64);
        assert(buf);
        int read_len = esp_http_client_read(http, buf, 64);
        if (read_len <= 0)
        {
            free(buf);
            return ESP_FAIL;
        }
        buf[read_len] = 0;
        // play
        free(buf);
        
        audio_element_set_uri(http_stream_reader, CONFIG_SERVER_OUTPUT_URI);
        audio_pipeline_run(play_pipeline);
        isPlaying = 1;
        while (1)
        {
            audio_event_iface_msg_t msg;
            esp_err_t ret = audio_event_iface_listen(play_evt, &msg, portMAX_DELAY);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
                continue;
            }

            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)wav_decoder && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
            {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(wav_decoder, &music_info);

                ESP_LOGI(TAG, "[ * ] Receive music info from wav decoder, sample_rates=%d, bits=%d, ch=%d",
                         music_info.sample_rates, music_info.bits, music_info.channels);

                i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
                continue;
            }

            /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *)i2s_stream_writer && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED)))
            {
                ESP_LOGW(TAG, "[ * ] Stop event received");
                break;
            }
        }
        audio_pipeline_stop(play_pipeline);
        audio_pipeline_wait_for_stop(play_pipeline);
        audio_pipeline_reset_ringbuffer(play_pipeline);
        audio_pipeline_reset_elements(play_pipeline);
        audio_pipeline_terminate(play_pipeline);
        isPlaying = 0;
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_ON_RESPONSE)
    {
        ESP_LOGI(TAG, "[ + ] HTTP client HTTP_STREAM_FINISH_RESPONSE");
        //------------------
        return ESP_OK;
    }

    return ESP_OK;
}

static esp_err_t input_key_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    if (isPlaying == 1)
    {
        return ESP_OK;
    }
    audio_element_handle_t http_stream_writer = (audio_element_handle_t)ctx;
    if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK)
    {
        switch ((int)evt->data)
        {
        case INPUT_KEY_USER_ID_MODE:
            ESP_LOGW(TAG, "[ * ] [Set] input key event, exit the demo ...");
            xEventGroupSetBits(EXIT_FLAG, DEMO_EXIT_BIT);
            break;
        case INPUT_KEY_USER_ID_REC:
            ESP_LOGE(TAG, "[ * ] [Rec] input key event, resuming pipeline ...");
            /*
             * There is no effect when follow APIs output warning message on the first time record
             */
            audio_pipeline_stop(recording_pipeline);
            audio_pipeline_wait_for_stop(recording_pipeline);
            audio_pipeline_reset_ringbuffer(recording_pipeline);
            audio_pipeline_reset_elements(recording_pipeline);
            audio_pipeline_terminate(recording_pipeline);

            audio_element_set_uri(http_stream_writer, CONFIG_SERVER_UPLOAD_URI);
            audio_pipeline_run(recording_pipeline);
            break;
        }
    }
    else if (evt->type == INPUT_KEY_SERVICE_ACTION_CLICK_RELEASE || evt->type == INPUT_KEY_SERVICE_ACTION_PRESS_RELEASE)
    {
        switch ((int)evt->data)
        {
        case INPUT_KEY_USER_ID_REC:
            ESP_LOGE(TAG, "[ * ] [Rec] key released, stop pipeline ...");
            /*
             * Set the i2s_stream_reader ringbuffer is done to flush the buffering voice data.
             */
            audio_element_set_ringbuf_done(i2s_stream_reader);
            break;
        }
    }

    return ESP_OK;
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    EXIT_FLAG = xEventGroupCreate();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    ESP_LOGI(TAG, "[ 1 ] Initialize Button Peripheral & Connect to wifi network");
    // Initialize peripherals management
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    periph_wifi_cfg_t wifi_cfg = {
        .ssid = CONFIG_WIFI_SSID,
        .password = CONFIG_WIFI_PASSWORD,
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);

    // Start wifi & button peripheral
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);

    ESP_LOGI(TAG, "[ 2 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for recording");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    recording_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(recording_pipeline);

    ESP_LOGI(TAG, "[3.1] Create http stream to post data to server");

    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    http_cfg.event_handle = _http_stream_event_handle;
    http_stream_writer = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_cfg.out_rb_size = 16 * 1024; // Increase buffer to avoid missing data in bad network conditions
    i2s_cfg.i2s_port = CODEC_ADC_I2S_PORT;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[3.3] Register all elements to audio pipeline");
    audio_pipeline_register(recording_pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(recording_pipeline, http_stream_writer, "http");

    ESP_LOGI(TAG, "[3.4] Link it together [codec_chip]-->i2s_stream->http_stream-->[http_server]");
    const char *link_tag_recording[2] = {"i2s", "http"};
    audio_pipeline_link(recording_pipeline, &link_tag_recording[0], 2);

    //---------------------------------------

    ESP_LOGI(TAG, "[3.5] Create audio pipeline for playback");
    play_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(play_pipeline);

    ESP_LOGI(TAG, "[3.6] Create http stream to read data");
    http_stream_cfg_t http_play_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_reader = http_stream_init(&http_play_cfg);

    ESP_LOGI(TAG, "[3.7] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_play_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_play_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_play_cfg);

    ESP_LOGI(TAG, "[3.8] Create wav decoder to decode WAV file");

    wav_decoder_cfg_t wav_cfg = DEFAULT_WAV_DECODER_CONFIG();
    wav_decoder = wav_decoder_init(&wav_cfg);

    ESP_LOGI(TAG, "[3.19] Register all elements to audio pipeline");
    audio_pipeline_register(play_pipeline, http_stream_reader, "http_play");
    audio_pipeline_register(play_pipeline, wav_decoder, "wav");
    audio_pipeline_register(play_pipeline, i2s_stream_writer, "i2s_play");

    ESP_LOGI(TAG, "[3.10] Link it together http_stream-->wav_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag_play[3] = {"http_play", "wav", "i2s_play"};
    audio_pipeline_link(play_pipeline, &link_tag_play[0], 3);

    //ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, wav as wav decoder, and default output is i2s)");


    // Initialize Button peripheral
    audio_board_key_init(set);
    input_key_service_info_t input_key_info[] = INPUT_KEY_DEFAULT_INFO();
    input_key_service_cfg_t input_cfg = INPUT_KEY_SERVICE_DEFAULT_CONFIG();
    input_cfg.handle = set;
    periph_service_handle_t input_ser = input_key_service_create(&input_cfg);
    input_key_service_add_key(input_ser, input_key_info, INPUT_KEY_NUM);
    periph_service_set_callback(input_ser, input_key_service_cb, (void *)http_stream_writer);

    i2s_stream_set_clk(i2s_stream_reader, EXAMPLE_AUDIO_SAMPLE_RATE, EXAMPLE_AUDIO_BITS, EXAMPLE_AUDIO_CHANNELS);

    // play evt
    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t play_evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    play_evt = audio_event_iface_init(&play_evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(play_pipeline, play_evt);

    // ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    // audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), play_evt);

    // end

    ESP_LOGI(TAG, "[ 4 ] Press [Rec] button to record, Press [Mode] to exit");
    xEventGroupWaitBits(EXIT_FLAG, DEMO_EXIT_BIT, true, false, portMAX_DELAY);

    ESP_LOGI(TAG, "[ 5 ] Stop audio_pipeline");
    audio_pipeline_stop(recording_pipeline);
    audio_pipeline_wait_for_stop(recording_pipeline);
    audio_pipeline_terminate(recording_pipeline);

    audio_pipeline_unregister(recording_pipeline, http_stream_writer);
    audio_pipeline_unregister(recording_pipeline, i2s_stream_reader);

    /* Terminal the pipeline before removing the listener */
    audio_pipeline_remove_listener(recording_pipeline);

    /* Stop all periph before removing the listener */
    esp_periph_set_stop_all(set);

    /* Release all resources */
    audio_pipeline_deinit(recording_pipeline);
    audio_element_deinit(http_stream_writer);
    audio_element_deinit(i2s_stream_reader);
    esp_periph_set_destroy(set);
}

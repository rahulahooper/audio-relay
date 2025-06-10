/* BSD Socket API Example


   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "driver/dac_continuous.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "protocol_examples_common.h"
#include "example_audio_file.h"

// -------- Local definitions and macros -------- //

#define PORT CONFIG_EXAMPLE_PORT
#define EXAMPLE_ESP_WIFI_SSID       "AudioRelayNetwork"
#define EXAMPLE_ESP_WIFI_PASS       "AudioRelayNetworkPassword"
#define EXAMPLE_ESP_WIFI_CHANNEL    1
#define EXAMPLE_MAX_STA_CONN        5

#define WIFI_STATION_CONNECT_MAXIMUM_RETRIES  10
#define WIFI_EVENT_AP_STACONNECTED_BIT        BIT0     // a wifi station connected to this device
#define WIFI_EVENT_AP_STADISCONNECTED_BIT     BIT1     // a wifi station disconnected from this device

#define ESP_CORE_0      0       // physical core 0
#define ESP_CORE_1      1       // physical core 1
#define AUDIO_PACKET_MAX_SAMPLES 996     // maximum amount of audio data we can receive from client at a time

// #define DEBUG
#ifdef DEBUG
    #define PRINTF_DEBUG( msg ) ESP_LOGI msg
#else
    #define PRINTF_DEBUG( msg )
#endif

typedef enum ReceiveTaskState_t
{
    RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER,        // Setup Wifi driver
    RECEIVE_TASK_STATE_SETUP_SERVER,             // Set up UDP socket server
    RECEIVE_TASK_STATE_WAIT_FOR_CLIENT,          // Wait for a client to connect
    RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT,       // Stream data from the client
    RECEIVE_TASK_STATE_STREAM_FROM_BUFFER,       // (For debug) Stream data from a buffer
} ReceiveTaskState_t;

static ReceiveTaskState_t gReceiveTaskState;     // global state object

typedef struct ReceiveTaskConfig_t
{
    bool             streamFromBuffer;        // get from audio data from file
    const uint8_t*   buffer;                  // buffer to get audio from (ignored if .streamFromBuffer is false)
    uint32_t         bufferSize;              // size of buffer

    uint8_t  maxPacketTimeoutsPerConnection;        // number of timeouts before we assume the client disconnected

} ReceiveTaskConfig_t;

typedef struct PlaybackTaskConfig_t
{
    uint32_t sampleRate;            // sampling rate for digital-to-analog converter (Hz)
    bool     useExternalDac;        // if false, configures the ESP32 built-in DAC
    uint32_t dataBitWidth;          // number of bits per audio sample
} PlaybackTaskConfig_t;

typedef struct AudioPacket_t
{
    uint16_t seqnum;
    bool     echo;                  // client requested an echo from server
    uint16_t numSamples;            // number of 16-bit samples in payload
    uint16_t payloadStart;
    uint16_t checksum;              // crc-16
    uint16_t payload[AUDIO_PACKET_MAX_SAMPLES];
} AudioPacket_t;

static const char *TAG = "wifi_ap";

/* FreeRTOS event group to signal when a client is connected or disconnected */
static EventGroupHandle_t s_wifi_event_group;

#define PLAYBACK_TASK_REQ_SAMPLES         AUDIO_PACKET_MAX_SAMPLES
#define PLAYBACK_TASK_DESIRED_SAMPLES     (uint16_t)(PLAYBACK_TASK_REQ_SAMPLES * 1.5)
#define SHARED_BUFFER_MAX_SAMPLES         (4 * PLAYBACK_TASK_REQ_SAMPLES)     // SharedBuffer is guaranteed to be able to accommodate this many samples

typedef struct SharedBuffer
{
    uint16_t payloadStart;
    uint16_t numSamples;                   // number of audio samples within the payload (TODO: rename to numSamples)
    uint16_t sampleSizeBytes;              // the size of each sample within the payload, in bytes
    uint8_t* payload;
} SharedBuffer;

static SharedBuffer gSharedBuffer[2];
static SharedBuffer* activeBuffer;               // transmitting task transmits this packet
static SharedBuffer* backBuffer;           // sampling task fills this packet

static const UBaseType_t playbackDoneNotifyIndex = 0;      // set by the playback task when it is done processing audio data
static const UBaseType_t dataReadyNotifyIndex;             // set by the receive task when there is new data available for the playback task

static TaskHandle_t receiveTaskHandle = NULL;
static TaskHandle_t playbackTaskHandle = NULL;


////////////////////////////////////////////////////////////////////
// wifi_setup_driver()
//
////////////////////////////////////////////////////////////////////
esp_err_t wifi_setup_driver(wifi_init_config_t* cfg)
{
    ESP_LOGI(TAG, "%s: Setting up WiFi driver\n", __func__);        

    esp_netif_create_default_wifi_sta();            // Setup wifi station for SNTP connection
    esp_netif_create_default_wifi_ap();             // Setup wifi access point

    ESP_ERROR_CHECK(esp_wifi_init(cfg));

    gReceiveTaskState = RECEIVE_TASK_STATE_SETUP_SERVER;

    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// wifi_event_handler()
//
////////////////////////////////////////////////////////////////////
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base != WIFI_EVENT && event_base != IP_EVENT)
    {
        ESP_LOGE(TAG, "%s received an event that it isn't supposed to handle: %s\n", __func__, event_base);
        return;
    }

    ESP_LOGI(TAG, "%s Handling event %ld\n", __func__, event_id);
    switch (event_id)
    {
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                    MAC2STR(event->mac), event->aid);

            xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_AP_STACONNECTED_BIT);
            break;
        } 
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d, reason=%d",
                    MAC2STR(event->mac), event->aid, event->reason);

            gReceiveTaskState = RECEIVE_TASK_STATE_WAIT_FOR_CLIENT;
            
            // TODO: Need to check if there are any resources we need to clean up when
            //       a client disconnects (ie. socket descriptors, Wifi resources, etc.)
            xEventGroupSetBits(s_wifi_event_group, WIFI_EVENT_AP_STADISCONNECTED_BIT);
            break;
        }
        case WIFI_EVENT_STA_START:
        {
            ESP_ERROR_CHECK(esp_wifi_connect());
            break;
        }
    }
}


////////////////////////////////////////////////////////////////////
// get_system_time()
//
////////////////////////////////////////////////////////////////////
esp_err_t get_system_time(int64_t* time_us)
{
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    *time_us = (int64_t)tv_now.tv_sec * 1000000L + (int64_t)tv_now.tv_usec;
    
    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// _wifi_softap_start()
//
////////////////////////////////////////////////////////////////////
esp_err_t _wifi_softap_start()
{
    // esp_netif_create_default_wifi_ap();

    // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_HUNT_AND_PECK,
#else
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
        },
    };

    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "%s finished. SSID:%s password:%s channel:%d",
            __func__, EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
    
    return ESP_OK;
}

////////////////////////////////////////////////////////////////////
// crc16()
//
////////////////////////////////////////////////////////////////////
uint16_t crc16(uint8_t* data, uint32_t len)
{
    // Compute a crc16 using polynomial 0x1021 and seed value 0xFFFF
    const uint16_t seed       = 0xFFFF;
    const uint16_t polynomial = 0x1021;

    uint16_t crc = seed;

    for (int i = 0; i < len; i++)
    {
        uint8_t byte = data[i];

        for (int j = 0; j < 8; j++)
        {
            if ((byte & 0x80) != 0)
            {
                crc ^= (polynomial << 8);
            }

            crc = (crc << 1) & 0xFFFF;
            byte <<= 1;
        }
    }

    return crc;
}


////////////////////////////////////////////////////////////////////
// setup_server()
//
////////////////////////////////////////////////////////////////////
esp_err_t setup_server(int addr_family, struct sockaddr_in* dest_addr, int* sockFd)
{

    ESP_ERROR_CHECK(_wifi_softap_start());

    // Set up socket address information
    if (addr_family != AF_INET) {
        ESP_LOGE(TAG, "%s:%u: Only supporting IPV4 addresses. Received %u\n", __func__, __LINE__, addr_family);
        return ESP_ERR_INVALID_ARG;
    }

    dest_addr->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr->sin_family = AF_INET;
    dest_addr->sin_port = htons(PORT);
    int ip_protocol = IPPROTO_IP;

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);

    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    }

    ESP_LOGI(TAG, "Socket created!!\n");

#if defined(CONFIG_LWIP_NETBUF_RECVINFO) && !defined(CONFIG_EXAMPLE_IPV6)
    int enable = 1;
    lwip_setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &enable, sizeof(enable));
#endif

#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
    if (addr_family == AF_INET6) {
        // Note that by default IPV6 binds to both protocols, it is must be disabled
        // if both protocols used at the same time (used in CI)
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
    }
#endif

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    int err = bind(sock, (struct sockaddr *)dest_addr, sizeof(struct sockaddr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d, err %d", errno, err);
        return -1;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    *sockFd = sock;
    gReceiveTaskState = RECEIVE_TASK_STATE_WAIT_FOR_CLIENT;
    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// copy_to_back_buffer()
//
// Helper function for unwrapping received audio data and 
// copying to the background buffer.
////////////////////////////////////////////////////////////////////
void copy_audio_packet_to_back_buffer(const AudioPacket_t* recvPacket, SharedBuffer* backBuffer)
{
    // The back buffer and received audio packet should both hold 16-bit audio data,
    // so no need to zero-extend or truncate the payload samples
    assert(backBuffer->sampleSizeBytes == sizeof(recvPacket->payload[0])); 

    assert(backBuffer->payloadStart == 0);

    // The back buffer should be able to store all the data in the received audio packet
    // assert(recvPacket->numSamples < SHARED_BUFFER_MAX_SAMPLES);

    // Packets sent by the client should be aligned to 2-byte (16-bit) boundary
    assert((recvPacket->payloadStart & 0x1) == 0);

    // Clear out the background buffer if the recvPacket will cause it to overflow
    // This shouldn't technically be possible since the playback task should drain 
    // this buffer long before it overflows
    if (recvPacket->numSamples + backBuffer->numSamples >= SHARED_BUFFER_MAX_SAMPLES)
    {
        ESP_LOGI(TAG, "%s background buffer would overflow. Clearing buffer.\n", __func__);
        backBuffer->numSamples = 0;
        backBuffer->payloadStart = 0;
    }

    // The recvpacket payload may have overflowed on the client-side. If this happens, the 
    // first audio sample will start at a non-zero offset into recvPacket->payload, and the audio stream will 
    // wrap around to the beginning of recvPacket->payload.
    //
    //         Normal Case:                              Wraparound case:
    //
    //            -----------------------------------     -----------------------------------
    //            |          Payload Array          |     |          Payload Array          |
    //            -----------------------------------     -----------------------------------
    //            |<---------------->|                    |<------------->| |<-------------->|
    //               ^ first and only chunk                        ^                 ^ 
    //               of audio data                          second chunk      first chunk of
    //                                                      of packet         audio data
    //
    uint32_t numSamplesFirstChunk = (recvPacket->payloadStart == 0) ? recvPacket->numSamples : ((AUDIO_PACKET_MAX_SAMPLES - recvPacket->payloadStart) >> 1);
    // uint32_t numSamplesFirstChunk = MIN((AUDIO_PACKET_MAX_SAMPLES - recvPacket->payloadStart) >> 1, recvPacket->numSamples);
    uint32_t numSamplesSecondChunk = (recvPacket->payloadStart == 0) ? 0 : (recvPacket->numSamples - numSamplesFirstChunk);

    if (recvPacket->payloadStart != 0)
    {
        ESP_LOGI(TAG, "%s recvPacket size = %u, seqnum = %u, start = %u, back buffer size = %u, first chunk = %lu, second chunk = %lu\n",
            __func__, recvPacket->numSamples, recvPacket->seqnum, recvPacket->payloadStart, 
            backBuffer->numSamples, numSamplesFirstChunk, numSamplesSecondChunk);
    }

    memcpy(&backBuffer->payload[backBuffer->numSamples * backBuffer->sampleSizeBytes],      // append to the existing samples in the back buffer
            &recvPacket->payload[recvPacket->payloadStart],                                 // copy from wherever the data starts in the received audio packet
            numSamplesFirstChunk * sizeof(uint16_t));                                       // copy the first chunk of data

    backBuffer->numSamples += numSamplesFirstChunk;

    memcpy(&backBuffer->payload[backBuffer->numSamples * backBuffer->sampleSizeBytes],      // append to the existing samples in the back buffer
            &recvPacket->payload[0], 
            numSamplesSecondChunk * sizeof(uint16_t));

    backBuffer->numSamples += numSamplesSecondChunk;

    // We've already confirmed this, but just to double check, 
    // ensure that the back buffer hasn't overflowed
    
    assert(backBuffer->numSamples < SHARED_BUFFER_MAX_SAMPLES);
}


////////////////////////////////////////////////////////////////////
// copy_back_buffer_to_active_buffer()
//
// Transfers contents of back buffer to active buffer.
// (the function name is kind of misleading)
////////////////////////////////////////////////////////////////////
void copy_back_buffer_to_active_buffer()
{
    assert(backBuffer->payloadStart == 0);

    // if the active buffer is too full to accommodate the back buffer,
    // clear the contents of the active buffer
    if ((activeBuffer->numSamples + backBuffer->numSamples) > SHARED_BUFFER_MAX_SAMPLES)
    {
        ESP_LOGE(__func__, "back buffer too large to copy into active buffer, resetting active buffer");
        ESP_LOGE(__func__, "active buffer size = %u, back buffer size = %u, limit = %u\n",
            activeBuffer->numSamples, backBuffer->numSamples, SHARED_BUFFER_MAX_SAMPLES);

        activeBuffer->payloadStart = 0;
        activeBuffer->numSamples = 0;
    }

    // Now there should be enough space to accommodate the back buffer

    assert((activeBuffer->numSamples + backBuffer->numSamples) <= SHARED_BUFFER_MAX_SAMPLES);

    // Move any leftover data in the active buffer to the beginning of the active buffer

    PRINTF_DEBUG((__func__, "BEFORE: active payload start = %u, size = %u, back buffer size = %u\n", 
        activeBuffer->payloadStart, activeBuffer->numSamples, backBuffer->numSamples));

    memmove(&activeBuffer->payload[0], 
            &activeBuffer->payload[activeBuffer->payloadStart], 
            activeBuffer->numSamples * activeBuffer->sampleSizeBytes);

    activeBuffer->payloadStart = 0;

    // Copy background buffer data into the active buffer, 
    // zero-extending the data if necessary

    assert(backBuffer->sampleSizeBytes == sizeof(uint16_t));              // 2 bytes per sample

    for (int i = 0; i < backBuffer->numSamples; i++)
    {
        uint8_t* dst = &activeBuffer->payload[activeBuffer->numSamples + i * activeBuffer->sampleSizeBytes];
        uint16_t* src = (uint16_t*)&backBuffer->payload[backBuffer->payloadStart + i * backBuffer->sampleSizeBytes];

        memset(dst, 0, activeBuffer->sampleSizeBytes);       // this ensures that the active buffer samples are zero-extended 
                                                             // if activeBuffer->sampleSizeBytes > backBuffer->sampleSizeBytes 
        memcpy(dst, src, MIN(activeBuffer->sampleSizeBytes, backBuffer->sampleSizeBytes));
    }

    // memcpy(&activeBuffer->payload[activeBuffer->numSamples], &backBuffer->payload[backBuffer->payloadStart], backBuffer->numSamples);
    activeBuffer->numSamples += backBuffer->numSamples;

    backBuffer->numSamples = 0;
    backBuffer->payloadStart = 0;

    PRINTF_DEBUG((__func__, "AFTER: active payload start = %u, size = %u, back buffer size = %u\n", 
        activeBuffer->payloadStart, activeBuffer->numSamples, backBuffer->numSamples));
}


////////////////////////////////////////////////////////////////////
// validate_packet()
//
////////////////////////////////////////////////////////////////////
void validate_audio_packet(AudioPacket_t* packet, uint16_t* expectedSeqnum, bool* isValid)
{
    uint16_t checksum = crc16((uint8_t*)packet->payload, packet->numSamples * sizeof(packet->payload[0]));

    *isValid = (checksum == packet->checksum);

    if (!(*isValid))
    {
        ESP_LOGE(TAG, "%s: Invalid checksum. Got 0x%x, expected 0x%x\n", __func__, checksum, packet->checksum);
        ESP_LOGE(TAG, "%s: checksum 0x%x, seqnum %u, payload size %u\n", 
            __func__, checksum, packet->seqnum, packet->numSamples);
        return;
    }

    // Check if a packet was duplicated or dropped 
    if (*expectedSeqnum < packet->seqnum)
    {
        // A packet got dropped in transit
        ESP_LOGI(__func__, "SERVER BEHIND: expected seqnum %u, received packet seqnum %u\n",
            *expectedSeqnum, packet->seqnum);
        *expectedSeqnum = packet->seqnum + 1;
    }
    else if (*expectedSeqnum > packet->seqnum)
    {
        if (*expectedSeqnum - packet->seqnum > UINT16_MAX / 2)
        {
            // On the client size, the seqnum overflowed
            ESP_LOGI(__func__, "SERVER BEHIND / CLIENT WRAPPED: expected seqnum %u, received packet seqnum %u\n",
                *expectedSeqnum, packet->seqnum);
            *expectedSeqnum = packet->seqnum + 1;
        }
        else
        {
            // Client sent a packet that the server has already seen
            ESP_LOGI(__func__, "SERVER AHEAD: expected seqnum %u, received packet seqnum %u\n",
                *expectedSeqnum, packet->seqnum);
            *isValid = false;
        }
    }
    else
    {
        *expectedSeqnum += 1;
    }
}

////////////////////////////////////////////////////////////////////
// stream_from_buffer()
//
// For debugging purposes. Instead of streaming data from a client
// over Wifi, just stream data from a local buffer that lives on
// this ESP32.
////////////////////////////////////////////////////////////////////
esp_err_t stream_from_buffer(const uint8_t* buffer, const uint32_t bufferSize)
{
    // Wait for the playback task to come up

    while (playbackTaskHandle == NULL)
    {
        ESP_LOGI(TAG, "%s Waiting for playback task to come up\n", __func__);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Initialize background buffer

    backBuffer->numSamples = 0;
    backBuffer->payloadStart = 0;

    // Read data from buffer in a continuous loop

    uint32_t idx = 0;

    while (true)
    {

        // Copy sample data into background buffer, extending sample data 
        // from 8-bits to 16-bits in the process

        assert(backBuffer->sampleSizeBytes == sizeof(uint16_t));

        for (int i = 0; i < PLAYBACK_TASK_DESIRED_SAMPLES; i++)
        {
            backBuffer->payload[2*i] = buffer[idx];
            backBuffer->payload[2*i+1] = 0x0;
            idx = (idx + 1) % bufferSize;
        }

        backBuffer->numSamples = PLAYBACK_TASK_DESIRED_SAMPLES;

        // Wait until the playback task signals that it is ready for new data

        while(!ulTaskNotifyTakeIndexed(playbackDoneNotifyIndex, pdTRUE, 0))
        {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        // At this point the playback task is waiting for new data. 
        // Copy sample data into the active buffer
        copy_back_buffer_to_active_buffer();
    
        assert(backBuffer->numSamples == 0);       // background buffer should now be empty
        assert(backBuffer->payloadStart == 0);      
        assert(activeBuffer->payloadStart == 0);    // data in active buffer should start at index 0

        // Signal to the playback task that new data is available
        xTaskNotifyGiveIndexed(playbackTaskHandle, dataReadyNotifyIndex);
    }

    // We should never get here

    return ESP_OK;
}

////////////////////////////////////////////////////////////////////
// stream_from_client()
//
////////////////////////////////////////////////////////////////////
esp_err_t stream_from_client(const int sock, const int maxPacketTimeouts)
{
    struct sockaddr_storage client_addr;
    socklen_t sockaddr_len = sizeof(client_addr);
    struct AudioPacket_t recvPacket;

    bool isFirstPacket = true;      // are we waiting on the first packet from the client?
    bool isFirstBufSwap = true;     // are we about to swap buffers with the playback task for the first time?
    uint16_t expectedSeqnum = 0;

    char client_addr_str[128];
    memset(client_addr_str, 0, sizeof(client_addr_str));

    uint32_t numPacketTimeouts = 0;

    while (playbackTaskHandle == NULL)
    {
        ESP_LOGI(TAG, "%s Waiting for playback task to come up\n", __func__);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool error = false;
    while (!error)
    {

        uint32_t minSamplesNeededBeforeSwap = isFirstBufSwap ? PLAYBACK_TASK_DESIRED_SAMPLES : PLAYBACK_TASK_REQ_SAMPLES;

        // Keep receiving data into the background buffer until 
        //   a) the receive task has accumulated sufficient data
        //   b) the playback task signals that it is ready for new data
        while((backBuffer->numSamples < minSamplesNeededBeforeSwap) || !ulTaskNotifyTakeIndexed(playbackDoneNotifyIndex, pdTRUE, 0))
        {
            // try to receive a packet

            memset(&recvPacket, 0, sizeof(recvPacket));
            int len = recvfrom(sock, &recvPacket, sizeof(recvPacket), 0, (struct sockaddr*)&client_addr, &sockaddr_len);

            // check if we failed to receive the packet
            // if this occurs enough times, we assume the client got disconnected

            if (len < 0)
            {
                if (errno == EWOULDBLOCK && ++numPacketTimeouts < maxPacketTimeouts)
                {
                    ESP_LOGE(TAG, "%s recvfrom timed out but continuing\n", __func__);
                    expectedSeqnum = 0;
                    continue;
                }
                else
                {
                    ESP_LOGE(TAG, "%s recvfrom failed: errno %d (%s)", __func__, errno, strerror(errno));
                    error = true;
                    break;
                }
            }    

            numPacketTimeouts = 0;
        
            // If this is the first packet, cache the client IP address in case we want to echo the packet back
            // TODO: Can we get the IP address in wait_for_client()?
            if (isFirstPacket)
            {
                isFirstPacket = false;

                // We found a new client, print out the IP address
                inet_ntoa_r(((struct sockaddr_in *)&client_addr)->sin_addr, client_addr_str, sizeof(client_addr_str) - 1);
                client_addr_str[sizeof(client_addr_str)-1] = 0;

                ESP_LOGI(TAG, "%s Connected to client with IP address %s\n", __func__, client_addr_str);
            }

            int64_t timerecv;
            get_system_time(&timerecv);

            // validate packet crc
            bool isValid = false;
            validate_audio_packet(&recvPacket, &expectedSeqnum, &isValid);

            if (!isValid) 
            {
                ESP_LOGE(__func__, "Received packet with expected seq num %u was invalid\n", expectedSeqnum-1);
                continue;
            }

            PRINTF_DEBUG((TAG, "%s: Successfully received packet with checksum 0x%x, seqnum %u, payload size %u\n", 
                __func__, recvPacket.checksum, recvPacket.seqnum, recvPacket.numSamples));

            // Copy packet into the background buffer
            copy_audio_packet_to_back_buffer(&recvPacket, backBuffer);

            PRINTF_DEBUG((TAG, "%s back buffer size = %u\n", __func__, backBuffer->numSamples));

            // if the client requested it, echo the packet back
            // TODO: Defer this to immediately after we have 
            //       provided new data to the playback task
            if (recvPacket.echo)
            {

                ESP_LOGI(TAG, "%s: Echoing back packet with checksum 0x%x, seqnum %u, payload size %u.\n", 
                    __func__, recvPacket.checksum, recvPacket.seqnum, recvPacket.numSamples);

                int err = sendto(sock, (void*)&recvPacket, sizeof(recvPacket), 0, (struct sockaddr*)&client_addr, sockaddr_len);

                if (err < 0)
                {
                    ESP_LOGE(TAG, "%s: Error sending sending response to client at address %s. errno = %s\n", __func__, client_addr_str, strerror(errno));
                    error = true;
                }
            }
        }

        if (error) 
        {
            break;
        }

        // At this point the playback task is blocked waiting for new data. 
        copy_back_buffer_to_active_buffer();
    
        assert(backBuffer->numSamples == 0);       // background buffer should now be empty
        assert(backBuffer->payloadStart == 0);      
        assert(activeBuffer->payloadStart == 0);    // data in active buffer should start at index 0

        isFirstBufSwap = false;

        // Signal to the playback task that new data is available
        xTaskNotifyGiveIndexed(playbackTaskHandle, dataReadyNotifyIndex);
    }

    // We should only be here if the client got disconnected

    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// receive_task_main()
//
////////////////////////////////////////////////////////////////////
static void receive_task_main(void *pvParameters)
{
    ReceiveTaskConfig_t receiveTaskConfig = *(ReceiveTaskConfig_t*)pvParameters;

    int addr_family = AF_INET;
    struct sockaddr_in dest_addr;       // server IP address

    int sock;                           // socket file descriptor

    s_wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_AP_STACONNECTED_BIT | WIFI_EVENT_AP_STADISCONNECTED_BIT);

    // Initialize receive task state machine
    if (receiveTaskConfig.streamFromBuffer)
    {
        gReceiveTaskState = RECEIVE_TASK_STATE_STREAM_FROM_BUFFER;
    }
    else
    {
        gReceiveTaskState = RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER;     
    }

    // Handle receive task state machine

    while (1) 
    {

        switch(gReceiveTaskState)
        {
            case RECEIVE_TASK_STATE_SETUP_WIFI_DRIVER:
            {
                wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                wifi_setup_driver(&cfg);
                break;
            } 
            case RECEIVE_TASK_STATE_SETUP_SERVER:
            {
                ESP_ERROR_CHECK( setup_server(addr_family, &dest_addr, &sock) );
                break;
            }
            case RECEIVE_TASK_STATE_WAIT_FOR_CLIENT:
            {
                ESP_LOGI(TAG, "%s Waiting for client to connect...\n", __func__);
                EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, 
                                                    WIFI_EVENT_AP_STACONNECTED_BIT,
                                                    pdTRUE, 
                                                    pdFALSE, 
                                                    1000 / portTICK_PERIOD_MS);
                
                if (bits & WIFI_EVENT_AP_STACONNECTED_BIT)
                {
                    ESP_LOGI(TAG, "%s Client connected!\n", __func__);
                    gReceiveTaskState = RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT;
                }
                break;
            }
            case RECEIVE_TASK_STATE_RECEIVE_FROM_CLIENT:
            {
                // This only returns if a client disconnects
                ESP_ERROR_CHECK(stream_from_client(sock, receiveTaskConfig.maxPacketTimeoutsPerConnection));
                
                gReceiveTaskState = RECEIVE_TASK_STATE_WAIT_FOR_CLIENT;
                break;
            }

            // for debug
            case RECEIVE_TASK_STATE_STREAM_FROM_BUFFER:
            {
                const uint8_t* buffer = receiveTaskConfig.buffer;
                const uint32_t bufferSize = receiveTaskConfig.bufferSize;

                assert(buffer != NULL);
                assert(bufferSize > 0);

                ESP_ERROR_CHECK(stream_from_buffer(buffer, bufferSize));
                break;
            }
            default:
            {
                ESP_LOGE(TAG, "%s Entered unknown state %u\n", __func__, gReceiveTaskState);
                break;
            }
        }
    }
    vTaskDelete(NULL);
}


////////////////////////////////////////////////////////////////////
// dac_on_convert_done_callback()
//
////////////////////////////////////////////////////////////////////
static bool IRAM_ATTR  dac_on_convert_done_callback(dac_continuous_handle_t handle, const dac_event_data_t *event, void *user_data)
{
    QueueHandle_t que = (QueueHandle_t)user_data;
    BaseType_t need_awoke;
    /* When the queue is full, drop the oldest item */
    if (xQueueIsQueueFullFromISR(que)) {
        dac_event_data_t dummy;
        xQueueReceiveFromISR(que, &dummy, &need_awoke);
    }
    /* Send the event from callback */
    xQueueSendFromISR(que, event, &need_awoke);
    return need_awoke;
}


////////////////////////////////////////////////////////////////////
// setup_dac()
//
////////////////////////////////////////////////////////////////////
void setup_dac(QueueHandle_t* queue, dac_continuous_handle_t* dacHandle, const uint32_t sampleRate)
{
    // Allocate resources for queue
    *queue = xQueueCreate(10, sizeof(dac_event_data_t));
    assert(*queue);

    // Allocate resources for DAC
    dac_continuous_config_t dacConfig = 
    {
        .chan_mask  = DAC_CHANNEL_MASK_CH0,
        .desc_num   = 4,
        .buf_size   = PLAYBACK_TASK_REQ_SAMPLES,    // each sample is 1 byte
        .freq_hz    = sampleRate,
        .offset     = 0,
        .clk_src    = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode  = DAC_CHANNEL_MODE_SIMUL,        // not necessary for our purposes
    };

    ESP_ERROR_CHECK(dac_continuous_new_channels(&dacConfig, dacHandle));

    // Register a callback for when the DAC has converted previously loaded data
    dac_event_callbacks_t dacCallback =
    {
        .on_convert_done = dac_on_convert_done_callback,
        .on_stop         = NULL,
    };

    ESP_ERROR_CHECK(dac_continuous_register_event_callback(*dacHandle, &dacCallback, *queue));
    ESP_ERROR_CHECK(dac_continuous_enable(*dacHandle));
    ESP_ERROR_CHECK(dac_continuous_start_async_writing(*dacHandle));
}


////////////////////////////////////////////////////////////////////
// i2s_on_sent_callback()
//
////////////////////////////////////////////////////////////////////
// static bool IRAM_ATTR i2s_on_sent_callback(i2s_chan_handle_t i2sHandle, i2s_event_data_t* event, void* user_ctx)
// {
//     QueueHandle_t que = (QueueHandle_t)user_ctx;
//     BaseType_t need_awoke;

//     /* When the queue is full, drop the oldest item */
//     if (xQueueIsQueueFullFromISR(que)) {
//         i2s_event_data_t dummy;
//         xQueueReceiveFromISR(que, &dummy, &need_awoke);
//     }

//     /* Send the event from callback */
//     xQueueSendFromISR(que, event, &need_awoke);
//     return need_awoke;
// }

////////////////////////////////////////////////////////////////////
// setup_i2s()
//
////////////////////////////////////////////////////////////////////
void setup_i2s(QueueHandle_t* queue, i2s_chan_handle_t* i2sHandle, const uint32_t sampleRate)
{
    // Allocate resources for queue

    *queue = xQueueCreate(10, sizeof(i2s_event_data_t));
    assert(*queue);

    // Configure the I2S channel

    i2s_chan_config_t i2sChanConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

    i2s_new_channel(&i2sChanConfig, i2sHandle, NULL);

    i2s_std_config_t i2sStdConfig = 
    {
        .clk_cfg = 
        {
            .sample_rate_hz = sampleRate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_384,
        },

        .slot_cfg =
        {
            .data_bit_width = I2S_DATA_BIT_WIDTH_24BIT,   // The PCM1753 defaults to 24-bit left-justified I2S. Therefore,
                                                          //  any data we load into the I2S buffer must be zero-extended to be 24-bits
            .slot_bit_width = I2S_DATA_BIT_WIDTH_24BIT, 
            .slot_mode      = I2S_SLOT_MODE_MONO,         // The PCM1753 expects two channels, but we only have one
                                                          //  channel's worth of data. The I2S TX buffer will duplicate
                                                          //  the data onto the second channel.
            .slot_mask      = I2S_STD_SLOT_BOTH,          // The PCM1753 expects two channels
            .ws_width       = I2S_DATA_BIT_WIDTH_24BIT,   // WS should be high for the entirety of a slot
            .bit_shift      = false,                      // The PCM1753 defaults to 24-bit left-justified I2S, which as no bit shift.
            .msb_right      = false,                      // ¯\_(ツ)_/¯

        },

        .gpio_cfg = 
        {
            .mclk = GPIO_NUM_0,
            .bclk = GPIO_NUM_13,
            .ws   = GPIO_NUM_27,
            .dout = GPIO_NUM_12,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags =
            {
                .bclk_inv = false,
                .mclk_inv = false,
                .ws_inv   = true,                       // The PCM1753 assets WS for the left channel and 
                                                        //  de-asserts WS for the right channel
            }
        }
    };

    // (Unclear if this is even necessary)
    // Register an interrupt that will fire whenever the I2S has finished sending all data to the DMA buffer

    // i2s_event_callbacks_t i2sCallbacks;
    // memset(&i2sCallbacks, 0, sizeof(i2sCallbacks));
    // i2sCallbacks.on_sent = i2s_on_sent_callback;
    // i2s_channel_register_event_callback(i2sHandle, &i2sCallbacks, queue);

    // Initialize and start the channel

    i2s_channel_init_std_mode(*i2sHandle, &i2sStdConfig);
    i2s_channel_enable(*i2sHandle);         

    // The I2S is now running and sending data to the PCM1753
}

////////////////////////////////////////////////////////////////////
// destroy_dac()
//
////////////////////////////////////////////////////////////////////
void destroy_dac(dac_continuous_handle_t handle, QueueHandle_t queue)
{
    ESP_LOGI(__func__, "Not implemented yet\n");

    // Stop dac conversions

    // Flush out the dac queue

    // Free dac resources

    // Free queue resouces
}


////////////////////////////////////////////////////////////////////
// playback_task_main()
//
////////////////////////////////////////////////////////////////////
void playback_task_main(void* pvParameters)
{
    PlaybackTaskConfig_t playbackTaskConfig = *(PlaybackTaskConfig_t*)pvParameters;

    while (receiveTaskHandle == NULL)
    {
        ESP_LOGI(TAG, "%s Waiting for receive task to come up\n", __func__);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Setup the digital-to-analog converter (dac)

    QueueHandle_t dacQueue;                 // shared between intenal and external DACs
    dac_continuous_handle_t dacHandle;      // handle for internal DAC
    i2s_chan_handle_t i2sHandle;            // handle for external DAC (PCM1753)

    if (playbackTaskConfig.useExternalDac)
    {
        setup_i2s(&dacQueue, &i2sHandle, playbackTaskConfig.sampleRate);
    }
    else
    {
        setup_dac(&dacQueue, &dacHandle, playbackTaskConfig.sampleRate);
    }

    bool error = false;

    while (!error)
    {
        // Notify the receive task that we are waiting for new data

        PRINTF_DEBUG((TAG, "%s Notifying receive task of playback done\n", __func__));
        xTaskNotifyGiveIndexed(receiveTaskHandle, playbackDoneNotifyIndex);

        // Wait for new data

        while (!ulTaskNotifyTakeIndexed(dataReadyNotifyIndex, pdTRUE, pdMS_TO_TICKS(1000)))
        {
            vTaskDelay(1);
        }

        PRINTF_DEBUG((__func__, "Got data ready notification.\n"));

        // Verify the new data
        //  1. Check whether the playback task has provided enough data to start playback
        //  2. Ensure that the data within the active buffer starts at index 0

        if (activeBuffer->numSamples < PLAYBACK_TASK_REQ_SAMPLES)
        {
            ESP_LOGE(__func__, "Received insufficient data from receive_task: %u / %u bytes\n",
                activeBuffer->numSamples, PLAYBACK_TASK_REQ_SAMPLES);
            continue;
        } 
        else if (activeBuffer->payloadStart != 0)
        {
            ESP_LOGE(__func__, "active buffer data doesn't start at idx 0 (%u). Not playing data\n",
                activeBuffer->payloadStart);
            activeBuffer->numSamples = 0;
            activeBuffer->payloadStart = 0;
            continue;
        }

        // Send data from active buffer to DAC

        if (playbackTaskConfig.useExternalDac)
        {
            i2s_event_data_t eventData;
            (void)eventData;
            assert(activeBuffer->sampleSizeBytes == 3 * sizeof(uint8_t));

            while (activeBuffer->numSamples > PLAYBACK_TASK_REQ_SAMPLES)
            {
                // xQueueReceive(dacQueue, &eventData, portMAX_DELAY);
                // size_t loadedBytes = 0;
                
                // // copy data into the i2s tx dma buffer
                // memcpy(eventData.dma_buf, activeBuffer + activeBuffer->payloadStart, eventData.size);
                // activeBuffer->payloadStart += MIN(loadedBytes, activeBuffer->numSamples);
                // activeBuffer->numSamples  -= MIN(loadedBytes, activeBuffer->numSamples);

                size_t bytesWritten;
                uint32_t timeoutMs = 1;

                i2s_channel_write(i2sHandle, activeBuffer + activeBuffer->payloadStart, activeBuffer->numSamples, &bytesWritten, timeoutMs);
                activeBuffer->payloadStart += bytesWritten;
                activeBuffer->numSamples  -= bytesWritten / 3;
            }
        }
        else
        {
            // Use the internal ESP32 DAC

            dac_event_data_t eventData;
            assert(activeBuffer->sampleSizeBytes == sizeof(uint8_t));

            while (activeBuffer->numSamples > PLAYBACK_TASK_REQ_SAMPLES)
            {
                // ESP_LOGI(__func__, "%u\n", activeBuffer->payload[activeBuffer->payloadStart]);
                xQueueReceive(dacQueue, &eventData, portMAX_DELAY);
                size_t loadedBytes = 0;
                ESP_ERROR_CHECK(dac_continuous_write_asynchronously(dacHandle, eventData.buf, eventData.buf_size,
                                                                    &activeBuffer->payload[activeBuffer->payloadStart],
                                                                    activeBuffer->numSamples * activeBuffer->sampleSizeBytes, 
                                                                    &loadedBytes));

                activeBuffer->payloadStart += MIN(loadedBytes, activeBuffer->numSamples);
                activeBuffer->numSamples  -= MIN(loadedBytes, activeBuffer->numSamples);
            }
        }
    }

    // We should never reach here

    destroy_dac(dacHandle, dacQueue);
    vTaskDelete(NULL);
}

////////////////////////////////////////////////////////////////////
// init_shared_buffers()
//
////////////////////////////////////////////////////////////////////
void init_shared_buffers(bool useExternalDac)
{
    // Initialize the active buffer

    activeBuffer = &gSharedBuffer[0];
    activeBuffer->payloadStart = 0;
    activeBuffer->numSamples = 0;

    // The external dac requires 24-bit samples, the internal dac requires 8-bit samples
    activeBuffer->sampleSizeBytes = useExternalDac ? 3 * sizeof(uint8_t) : sizeof(uint8_t);

    activeBuffer->payload = (uint8_t*)malloc(SHARED_BUFFER_MAX_SAMPLES * activeBuffer->sampleSizeBytes);
    memset(activeBuffer->payload, 0, SHARED_BUFFER_MAX_SAMPLES * activeBuffer->sampleSizeBytes);

    // Initialize the back buffer

    backBuffer = &gSharedBuffer[1];
    backBuffer->payloadStart = 0;
    backBuffer->numSamples = 0;

    // The samples we send over Wifi are 16-bit
    backBuffer->sampleSizeBytes = sizeof(uint16_t);

    backBuffer->payload = (uint8_t*)malloc(SHARED_BUFFER_MAX_SAMPLES * backBuffer->sampleSizeBytes);
    memset(backBuffer->payload, 0, SHARED_BUFFER_MAX_SAMPLES * backBuffer->sampleSizeBytes);
}


////////////////////////////////////////////////////////////////////
// app_main()
//
////////////////////////////////////////////////////////////////////
void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(nvs_flash_init());

    #ifdef CONFIG_LWIP_DHCP_GET_NTP_SRV
    ESP_LOGI(TAG, "%s LWIP config'd\n", __func__);
    #endif

    bool useExternalDac = false;

    init_shared_buffers(useExternalDac);

    // Create "receive" task
    // 
    // The receive task is responsible for receiving audio
    // packets from a client. The receive task will populate
    // a back buffer that is invisible to the playback
    // task until the playback task signals that it needs new data.
    // At this point, the receive task moves data from the back
    // buffer to the front buffer and signals to the receive task
    // that new data is available.

    ReceiveTaskConfig_t receiveTaskConfig = {
        .streamFromBuffer = true,
        .buffer           = audio_table_sawtooth,
        .bufferSize       = audio_table_sawtooth_size,
        .maxPacketTimeoutsPerConnection = 3, 
    };

    BaseType_t receiveTaskStatus = xTaskCreatePinnedToCore(receive_task_main, "receive_task", 8192, &receiveTaskConfig, 5, &receiveTaskHandle, ESP_CORE_0);

    if (receiveTaskStatus != pdPASS)
    {
        ESP_LOGE(TAG, "%s Failed to create receive task!\n", __func__);
        return;
    }

    // Create "playback" task
    //
    // The playback task is responsible for converting the
    // audio data in a front buffer to analog and driving
    // the amplifier circuit. When it is out of data, the 
    // playback task will notify the receive task and block
    // until there is new data available.

    PlaybackTaskConfig_t playbackTaskConfig = {
        .sampleRate = 48000,
        .useExternalDac = useExternalDac,
    };

    BaseType_t playbackTaskStatus = xTaskCreatePinnedToCore(playback_task_main, "playback_task", 8192, &playbackTaskConfig, 5, &playbackTaskHandle, ESP_CORE_1);

    if (playbackTaskStatus != pdPASS)
    {
        ESP_LOGE(TAG, "%s Failed to create transmit task!\n", __func__);
        return;
    }

}

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
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_types_generic.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "esp_sntp.h"
#include "esp_netif_sntp.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

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
#define PAYLOAD_MAX_LEN 996     // maximum amount of audio data we can receive from client at a time

#define DEBUG 0
#if DEBUG
    #define PRINTF_DEBUG( msg ) ESP_LOGI msg
#else
    #define PRINTF_DEBUG( msg )
#endif

typedef enum State_t
{
    SETUP_WIFI_DRIVER,
    SETUP_SERVER,
    WAIT_FOR_CLIENT,
    STREAM_FROM_CLIENT,
} State_t;

static State_t gState;     // global state object

typedef struct AudioPacket_t
{
    uint16_t seqnum;
    bool     echo;                  // client requested an echo from server
    uint16_t payloadSize;
    uint16_t payloadStart;
    uint16_t checksum;              // crc-16
    uint8_t  payload[PAYLOAD_MAX_LEN];
} AudioPacket_t;

typedef struct ResponsePacket_t
{
    uint16_t seqnum;
    char     response[5];   // large enough to hold the "NACK" string
} ResponsePacket_t;

struct IPPacket_t
{
    uint32_t ipv4;
    uint16_t checksum;
};

static const char *TAG = "wifi_ap";

/* FreeRTOS event group to signal when a client is connected or disconnected */
static EventGroupHandle_t s_wifi_event_group;

#define PLAYBACK_TASK_REQ_DATA_SIZE         PAYLOAD_MAX_LEN
#define PLAYBACK_TASK_DESIRED_DATA_SIZE     (uint16_t)(PAYLOAD_MAX_LEN * 1.5)
#define SHARED_BUFFER_LEN                   (4 * PAYLOAD_MAX_LEN)

typedef struct SharedBuffer_t
{
    uint16_t payloadStart;
    uint16_t payloadSize;
    uint8_t  payload[SHARED_BUFFER_LEN];
} SharedBuffer_t;

static SharedBuffer_t gSharedBuffer[2];
static SharedBuffer_t* activeBuffer;               // transmitting task transmits this packet
static SharedBuffer_t* backBuffer;           // sampling task fills this packet

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

    gState = SETUP_SERVER;

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

            gState = WAIT_FOR_CLIENT;
            
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
    gState = WAIT_FOR_CLIENT;
    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// copy_to_back_buffer()
//
// Helper function for unwrapping received audio data and 
// copying to the background buffer.
////////////////////////////////////////////////////////////////////
void copy_recv_data_to_back_buffer(const AudioPacket_t* recvPacket, SharedBuffer_t* backBuffer)
{

    assert(backBuffer->payloadStart == 0);
    assert(recvPacket->payloadSize < SHARED_BUFFER_LEN);

    // Clear out the background buffer if the recvPacket will cause it to overflow
    // This shouldn't technically be possible since the playback task should drain 
    // this buffer long before it overflows
    if (recvPacket->payloadSize + backBuffer->payloadSize >= SHARED_BUFFER_LEN)
    {
        ESP_LOGI(TAG, "%s background buffer would overflow. Clearing buffer.\n", __func__);
        backBuffer->payloadSize = 0;
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
    uint32_t numBytesFirstChunk = MIN(PAYLOAD_MAX_LEN - recvPacket->payloadStart, recvPacket->payloadSize);
    uint32_t numBytesSecondChunk = recvPacket->payloadSize - numBytesFirstChunk;

    if (recvPacket->payloadStart != 0)
    {
        ESP_LOGI(TAG, "%s recvPacket size = %u, start = %u, back buffer size = %u, first chunk = %lu, second chunk = %lu\n",
            __func__, recvPacket->payloadSize, recvPacket->payloadStart, backBuffer->payloadSize, 
            numBytesFirstChunk, numBytesSecondChunk);
    }
    memcpy(&backBuffer->payload[backBuffer->payloadStart], &recvPacket->payload[recvPacket->payloadStart], numBytesFirstChunk);
    backBuffer->payloadSize += numBytesFirstChunk;

    memcpy(&backBuffer->payload[backBuffer->payloadStart], &recvPacket->payload[0], numBytesSecondChunk);
    backBuffer->payloadSize += numBytesSecondChunk;

    assert(backBuffer->payloadSize < SHARED_BUFFER_LEN);
}


////////////////////////////////////////////////////////////////////
// copy_back_buffer_to_active_buffer()
//
// Drains the back buffer into the active buffer.
////////////////////////////////////////////////////////////////////
void copy_back_buffer_to_active_buffer()
{
    assert(backBuffer->payloadStart == 0);

    if (activeBuffer->payloadSize + backBuffer->payloadSize > SHARED_BUFFER_LEN)
    {
        ESP_LOGE(__func__, "back buffer too large to copy into active buffer, resetting active buffer");
        ESP_LOGE(__func__, "active buffer size = %u, back buffer size = %u, limit = %u\n",
            activeBuffer->payloadSize, backBuffer->payloadSize, SHARED_BUFFER_LEN);

        memcpy(&activeBuffer->payload[0], &backBuffer->payload[backBuffer->payloadStart], backBuffer->payloadSize);
        activeBuffer->payloadSize = backBuffer->payloadSize;
        activeBuffer->payloadStart = 0;
        backBuffer->payloadSize = 0;
        backBuffer->payloadStart = 0;
        return;
    }

    // Move any leftover data in the active buffer to the beginning of the active buffer
    PRINTF_DEBUG((__func__, "BEFORE: active payload start = %u, size = %u, back buffer size = %u\n", 
        activeBuffer->payloadStart, activeBuffer->payloadSize, backBuffer->payloadSize));
    memmove(&activeBuffer->payload[0], &activeBuffer->payload[activeBuffer->payloadStart], activeBuffer->payloadSize);
    activeBuffer->payloadStart = 0;

    // Copy background buffer data into the active buffer
    memcpy(&activeBuffer->payload[activeBuffer->payloadSize], &backBuffer->payload[backBuffer->payloadStart], backBuffer->payloadSize);
    activeBuffer->payloadSize += backBuffer->payloadSize;

    backBuffer->payloadSize = 0;
    backBuffer->payloadStart = 0;

    PRINTF_DEBUG((__func__, "AFTER: active payload start = %u, size = %u, back buffer size = %u\n", 
        activeBuffer->payloadStart, activeBuffer->payloadSize, backBuffer->payloadSize));
}

////////////////////////////////////////////////////////////////////
// stream_from_client()
//
////////////////////////////////////////////////////////////////////
esp_err_t stream_from_client(const int sock)
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
    const uint32_t MAX_PACKET_TIMEOUTS = 3;

    while (playbackTaskHandle == NULL)
    {
        ESP_LOGI(TAG, "%s Waiting for playback task to come up\n", __func__);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool error = false;
    while (!error)
    {

        uint32_t minPayloadSizeBeforeSwap = isFirstBufSwap ? PLAYBACK_TASK_DESIRED_DATA_SIZE : PLAYBACK_TASK_REQ_DATA_SIZE;

        // Keep receiving data into the background buffer until 
        //   a) (at startup only) the receive task has accumulated sufficient data
        //   b) the playback task signals that it is ready for new data
        while((backBuffer->payloadSize < minPayloadSizeBeforeSwap) || !ulTaskNotifyTakeIndexed(playbackDoneNotifyIndex, pdTRUE, 0))
        {
            // receive packet
            memset(&recvPacket, 0, sizeof(recvPacket));
            int len = recvfrom(sock, &recvPacket, sizeof(recvPacket), 0, (struct sockaddr*)&client_addr, &sockaddr_len);

            if (len < 0)
            {
                if (errno == EWOULDBLOCK && ++numPacketTimeouts < MAX_PACKET_TIMEOUTS)
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
        
            // Cache the client IP address in case we want to echo back a packet
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

            // validate packet
            uint16_t checksum = crc16(recvPacket.payload, recvPacket.payloadSize);

            if (checksum != recvPacket.checksum)
            {
                ESP_LOGE(TAG, "%s: Invalid checksum. Got 0x%x, expected 0x%x\n", __func__, checksum, recvPacket.checksum);
                ESP_LOGE(TAG, "%s: checksum 0x%x, seqnum %u, payload size %u\n", 
                    __func__, checksum, recvPacket.seqnum, recvPacket.payloadSize);
                continue;
            }

            // Check if a packet was duplicated or dropped 
            if (expectedSeqnum < recvPacket.seqnum)
            {
                // A packet got dropped in transit
                ESP_LOGI(__func__, "SERVER BEHIND: expected seqnum %u, received packet seqnum %u\n",
                    expectedSeqnum, recvPacket.seqnum);
                expectedSeqnum = recvPacket.seqnum + 1;
            }
            else if (expectedSeqnum > recvPacket.seqnum)
            {
                if (expectedSeqnum - recvPacket.seqnum > UINT16_MAX / 2)
                {
                    // On the client size, the seqnum overflowed
                    ESP_LOGI(__func__, "SERVER BEHIND / CLIENT WRAPPED: expected seqnum %u, received packet seqnum %u\n",
                        expectedSeqnum, recvPacket.seqnum);
                    expectedSeqnum = recvPacket.seqnum + 1;
                }
                else
                {
                    // Client sent a packet that the server has already seen
                    ESP_LOGI(__func__, "SERVER AHEAD: expected seqnum %u, received packet seqnum %u\n",
                        expectedSeqnum, recvPacket.seqnum);
                }
            }
            else
            {
                expectedSeqnum++;
            }

            PRINTF_DEBUG((TAG, "%s: Successfully received packet with checksum 0x%x, seqnum %u, payload size %u\n", 
                __func__, checksum, recvPacket.seqnum, recvPacket.payloadSize));

            // Copy into background buffer
            copy_recv_data_to_back_buffer(&recvPacket, backBuffer);

            PRINTF_DEBUG((TAG, "%s back buffer size = %u\n", __func__, backBuffer->payloadSize));

            // echo the packet
            // TODO: Defer this to immediately after we have 
            //       provided new data to the playback task
            if (recvPacket.echo)
            {

                ESP_LOGI(TAG, "%s: Echoing back packet with checksum 0x%x, seqnum %u, payload size %u.\n", 
                    __func__, checksum, recvPacket.seqnum, recvPacket.payloadSize);

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
    
        assert(backBuffer->payloadSize == 0);
        assert(backBuffer->payloadStart == 0);
        assert(activeBuffer->payloadStart == 0);

        isFirstBufSwap = false;

        // Signal to the playback task that new data is available
        xTaskNotifyGiveIndexed(playbackTaskHandle, dataReadyNotifyIndex);
    }

    // We should only be here if the client got disconnected

    return ESP_OK;
}


////////////////////////////////////////////////////////////////////
// receive_task()
//
////////////////////////////////////////////////////////////////////
static void receive_task(void *pvParameters)
{
    int addr_family = AF_INET;
    struct sockaddr_in dest_addr;       // server IP address

    int sock;                           // socket file descriptor

    s_wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_event_group, WIFI_EVENT_AP_STACONNECTED_BIT | WIFI_EVENT_AP_STADISCONNECTED_BIT);

    gState = SETUP_WIFI_DRIVER;     // initialize global state object

    while (1) 
    {

        switch(gState)
        {
            case SETUP_WIFI_DRIVER:
            {
                wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
                wifi_setup_driver(&cfg);
                break;
            } 
            case SETUP_SERVER:
            {
                ESP_ERROR_CHECK( setup_server(addr_family, &dest_addr, &sock) );
                break;
            }
            case WAIT_FOR_CLIENT:
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
                    gState = STREAM_FROM_CLIENT;
                }
                break;
            }
            case STREAM_FROM_CLIENT:
            {
                // This only returns if a client disconnects
                ESP_ERROR_CHECK(stream_from_client(sock));
                gState = WAIT_FOR_CLIENT;
                break;
            }
            default:
            {
                ESP_LOGE(TAG, "%s Entered unknown state %u\n", __func__, gState);
                break;
            }
        }
    }
    vTaskDelete(NULL);
}


////////////////////////////////////////////////////////////////////
// playback_task()
//
////////////////////////////////////////////////////////////////////
void playback_task(void* pvParameters)
{

    const uint32_t SAMPLE_RATE = 44100; //hz    
    const uint32_t MS_PER_SAMPLE = (uint32_t)(1.0f / SAMPLE_RATE * 1000);

    while (receiveTaskHandle == NULL)
    {
        ESP_LOGI(TAG, "%s Waiting for receive task to come up\n", __func__);
        vTaskDelay(pdMS_TO_TICKS(5));
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
        if (activeBuffer->payloadSize < PLAYBACK_TASK_REQ_DATA_SIZE)
        {
            ESP_LOGE(__func__, "Received insufficient data from receive_task: %u / %u bytes\n",
                activeBuffer->payloadSize, PLAYBACK_TASK_REQ_DATA_SIZE);
            continue;
        } 
        else if (activeBuffer->payloadStart != 0)
        {
            ESP_LOGE(__func__, "active buffer data doesn't start at idx 0 (%u). Not playing data\n",
                activeBuffer->payloadStart);
            activeBuffer->payloadSize = 0;
            activeBuffer->payloadStart = 0;
            continue;
        }


        // Convert data in active buffer (ESP32 has an asynchronous DMA for streaming audio data to a DAC)
        while (activeBuffer->payloadSize > PLAYBACK_TASK_REQ_DATA_SIZE)
        {
            vTaskDelay(pdMS_TO_TICKS(MS_PER_SAMPLE * PLAYBACK_TASK_REQ_DATA_SIZE));
        
            PRINTF_DEBUG((__func__, "BEFORE: active payload start = %u, size = %u\n", 
                activeBuffer->payloadStart, activeBuffer->payloadSize));

            activeBuffer->payloadSize -= PLAYBACK_TASK_REQ_DATA_SIZE;
            activeBuffer->payloadStart  = (activeBuffer->payloadStart + PLAYBACK_TASK_REQ_DATA_SIZE) % SHARED_BUFFER_LEN;

            PRINTF_DEBUG((__func__, "AFTER: active payload start = %u, size = %u\n", 
                activeBuffer->payloadStart, activeBuffer->payloadSize));
        }
    }

    ESP_LOGE(__func__, "An unrecoverable error occurred. Killing task...\n");
    vTaskDelete(NULL);
}


void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(nvs_flash_init());

    #ifdef CONFIG_LWIP_DHCP_GET_NTP_SRV
    ESP_LOGI(TAG, "%s LWIP config'd\n", __func__);
    #endif

    activeBuffer = &gSharedBuffer[0];
    backBuffer = &gSharedBuffer[1];

    memset(activeBuffer, 0, sizeof(struct SharedBuffer_t));
    memset(backBuffer, 0, sizeof(struct SharedBuffer_t));

    // Create "receive" and "playback" tasks
    //
    // The receive task is responsible for receiving audio
    // packets from a client. The receive task will populate
    // an back buffer that is invisible to the playback
    // task. The receive task will populate the back buffer
    // until the playback task signals that it needs new data.
    //
    // The playback task is responsible for converting the
    // audio data in a front buffer to analog and driving
    // the amplifier circuit. When it is out of data, the 
    // playback task will notify the receive task and block
    // and until there is new data available.
    ESP_LOGI(TAG, "%s Creating tasks\n", __func__);
    BaseType_t status;

    status = xTaskCreatePinnedToCore(receive_task, "receive_task", 8192, NULL, 5, &receiveTaskHandle, ESP_CORE_0);

    if (status != pdPASS)
    {
        ESP_LOGE(TAG, "%s Failed to create receive task!\n", __func__);
        return;
    }
    
    status = xTaskCreatePinnedToCore(playback_task, "playback_task", 8192, NULL, 5, &playbackTaskHandle, ESP_CORE_1);

    if (status != pdPASS)
    {
        ESP_LOGE(TAG, "%s Failed to create transmit task!\n", __func__);
        return;
    }

}

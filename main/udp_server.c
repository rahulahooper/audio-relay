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

#define PAYLOAD_LEN 996

typedef enum State_t
{
    SETUP_WIFI,
    SETUP_SERVER,
    WAIT_FOR_CLIENT,
    STREAM_FROM_CLIENT,
} State_t;

static State_t gState = SETUP_WIFI;     // global state object

struct AudioPacket_t
{
    uint16_t seqnum;
    uint8_t  payload[PAYLOAD_LEN];
    uint16_t checksum;              // crc-16
};

struct IPPacket_t
{
    uint32_t ipv4;
    uint16_t checksum;
};

static const char *TAG = "wifi_ap";

/* FreeRTOS event group to signal when a client is connected or disconnected */
static EventGroupHandle_t s_wifi_event_group;

#define WIFI_STA_CONNECTED_BIT     BIT0
#define WIFI_STA_DISCONNECTED_BIT  BIT1

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) 
    {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);

        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT);
    } 
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) 
    {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);

        gState = WAIT_FOR_CLIENT;
        
        // TODO: Need to check if there are any resources we need to clean up when
        //       a client disconnects (ie. socket descriptors, Wifi resources, etc.)
        xEventGroupSetBits(s_wifi_event_group, WIFI_STA_DISCONNECTED_BIT);
    }
}

void _wifi_softap_start()
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

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
    
    gState = SETUP_SERVER;
}

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

esp_err_t setupServer(int addr_family, struct sockaddr_in* dest_addr, int* sockFd)
{

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
    timeout.tv_sec = 10;
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

esp_err_t streamFromClient(const int sock)
{
    struct sockaddr_storage client_addr;
    socklen_t sockaddr_len = sizeof(client_addr);
    struct AudioPacket_t dummyPacket;
    bool alreadyConnected = false;

    char client_addr_str[128];
    memset(client_addr_str, 0, sizeof(client_addr_str));

    while (true)
    {
        // receive packet
        int len = recvfrom(sock, &dummyPacket, sizeof(dummyPacket), 0, (struct sockaddr*)&client_addr, &sockaddr_len);

        if (len < 0)
        {
            ESP_LOGE(TAG, "recvfrom failed: errno %d (%s)", errno, strerror(errno));
            break;
        }    
        
        if (!alreadyConnected)
        {
            alreadyConnected = true;

            // We found a new client, print out the IP address
            inet_ntoa_r(((struct sockaddr_in *)&client_addr)->sin_addr, client_addr_str, sizeof(client_addr_str) - 1);
            client_addr_str[sizeof(client_addr_str)-1] = 0;

            ESP_LOGI(TAG, "%s Connected to client with IP address %s\n", __func__, client_addr_str);
        }

        // validate packet
        uint16_t checksum = crc16(dummyPacket.payload, PAYLOAD_LEN);

        const char* message;
        if (checksum != dummyPacket.checksum)
        {
            ESP_LOGE(TAG, "%s: Invalid checksum. Got 0x%x, expected 0x%x\n", __func__, checksum, dummyPacket.checksum);
            message = "NACK";
        }
        else
        {
            ESP_LOGI(TAG, "%s: Successfully received packet with checksum 0x%x, seqnum %u\n", __func__, checksum, dummyPacket.seqnum);
            message = "ACK";
        }

        int err = sendto(sock, message, strlen(message), 0, (struct sockaddr*)&client_addr, sockaddr_len);
        if (err < 0)
        {
            ESP_LOGE(TAG, "%s: Error sending sending response (%s) to client at address %s\n", __func__, message, client_addr_str);
        }
    }

    // We should only break out of here if the client got disconnected
    gState = WAIT_FOR_CLIENT;

    return ESP_OK;
}

static void udp_server_task(void *pvParameters)
{
    int addr_family = (int)pvParameters;
    struct sockaddr_in dest_addr;       // server IP address

    int sock;                           // socket file descriptor

    s_wifi_event_group = xEventGroupCreate();
    xEventGroupClearBits(s_wifi_event_group, WIFI_STA_CONNECTED_BIT | WIFI_STA_DISCONNECTED_BIT);

    while (1) 
    {

        if (gState == SETUP_WIFI)
        {
            _wifi_softap_start();
        }
        else if (gState == SETUP_SERVER)
        {
            ESP_ERROR_CHECK( setupServer(addr_family, &dest_addr, &sock) );
        }
        else if (gState == WAIT_FOR_CLIENT)
        {
            ESP_LOGI(TAG, "%s Waiting for client to connect...\n", __func__);
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, 
                                                   WIFI_STA_CONNECTED_BIT, 
                                                   pdTRUE, 
                                                   pdFALSE, 
                                                   1000 / portTICK_PERIOD_MS);
            
            if (bits & WIFI_STA_CONNECTED_BIT)
            {
                ESP_LOGI(TAG, "%s Client connected!\n", __func__);
                gState = STREAM_FROM_CLIENT;
            }
        }
        else if (gState == STREAM_FROM_CLIENT)
        {
            // This only returns if a client disconnects
            ESP_ERROR_CHECK( streamFromClient(sock) );
        }
        else
        {
            ESP_LOGE(TAG, "%s Entered unknown state %u\n", __func__, gState);
            break;
        }

    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(udp_server_task, "udp_server", 4096, (void*)AF_INET, 5, NULL);

}

/*
 * Wifi.cpp
 *
 *  Created on: 29.04.2020
 *      Author: galuschka
 */

#include "Wifi.h"

#include <string.h>         // strncpy()

#include "esp_wifi.h"       // esp_wifi_init(), ...
#include "nvs.h"            // nvs_open(), ...
#include "esp_ota_ops.h"    // esp_ota_get_app_description()
#include "esp_log.h"        // ESP_LOGI()

const char *s_keySsid = "ssid";
const char *s_keyPassword = "password";

static const char *TAG = "Wifi";

Wifi::Wifi() :
        mIpAddr { 0 }, mSsid { "" }, mPassword { "" }, mMode { MODE_IDLE }
{
    mConnectEventGroup = xEventGroupCreate();

    ReadParam();
}

Wifi& Wifi::Instance()
{
    static Wifi inst { };
    return inst;
}

void Wifi::ReadParam()
{
    ESP_LOGI( TAG, "Reading Wifi configuration" );

    nvs_handle my_handle;
    if (nvs_open( "wifi", NVS_READONLY, &my_handle ) == ESP_OK) {
        size_t len = sizeof(mSsid);
        nvs_get_str( my_handle, s_keySsid, mSsid, &len );
        len = sizeof(mPassword);
        nvs_get_str( my_handle, s_keyPassword, mPassword, &len );

        nvs_close( my_handle );
    }
    // if (mSsid[0])     ESP_LOGI( TAG, "SSID:     %s", mSsid     );
    // if (mPassword[0]) ESP_LOGI( TAG, "password: %s", mPassword );
}

bool Wifi::SetParam( const char * ssid, const char * password )
{
    nvs_handle my_handle;
    if (nvs_open( "wifi", NVS_READWRITE, &my_handle ) != ESP_OK)
        return false;

    esp_err_t esp = nvs_set_str( my_handle, s_keySsid, ssid );
    if (esp == ESP_OK)
        esp = nvs_set_str( my_handle, s_keyPassword, password );
    nvs_close( my_handle );

    if (esp == ESP_OK) {
        strncpy( mSsid, ssid, sizeof(mSsid) );
        strncpy( mPassword, password, sizeof(mPassword) );
    }
    return esp == ESP_OK;
}

extern "C" void wifi_event( void * wifi, esp_event_base_t event_base,
        int32_t event_id, void * event_data )
{
    ((Wifi*) wifi)->Event( event_base, event_id, event_data );
}

void Wifi::Event( esp_event_base_t event_base, int32_t event_id,
        void * event_data )
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event =
                (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI( TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
                event->aid );
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event =
                (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI( TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
                event->aid );
    }
}

extern "C" void got_ip( void * wifi, esp_event_base_t event_base,
        int32_t event_id, void * event_data )
{
    ((Wifi*) wifi)->GotIp( (ip_event_got_ip_t*) event_data );
}

void Wifi::GotIp( ip_event_got_ip_t * event )
{
    memcpy( &mIpAddr, &event->ip_info.ip, sizeof(ip4_addr_t) );
    xEventGroupSetBits( mConnectEventGroup, GOT_IPV4_BIT );
}

bool Wifi::ModeSta( int connTimoInSecs )
{
    mMode = MODE_CONNECTING;

    ESP_LOGI( TAG, "Connecting to \"%s\" ...", mSsid );

    ESP_ERROR_CHECK(
            esp_event_handler_register( IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip,
                    this ) );

    wifi_config_t wifi_config;
    memset( &wifi_config, 0, sizeof(wifi_config_t) );

    strncpy( (char*) &wifi_config.sta.ssid, mSsid, sizeof(mSsid) );
    strncpy( (char*) &wifi_config.sta.password, mPassword, sizeof(mPassword) );

    ESP_ERROR_CHECK( esp_wifi_set_storage( WIFI_STORAGE_RAM ) );
    ESP_ERROR_CHECK( esp_wifi_set_mode( WIFI_MODE_STA ) );
    ESP_ERROR_CHECK( esp_wifi_set_config( ESP_IF_WIFI_STA, &wifi_config ) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_connect() );

    if (!xEventGroupWaitBits( mConnectEventGroup, GOT_IPV4_BIT, true, true,
    configTICK_RATE_HZ * connTimoInSecs )) {
        mMode = MODE_CONNECTFAILED;
        ESP_LOGW( TAG, "Connection to %s timed out - setup AP", mSsid );
        return false;
    }
    mMode = MODE_STATION;
    ESP_LOGI( TAG, "Connected to %s", mSsid );
    ESP_LOGI( TAG, "IPv4 address: " IPSTR, IP2STR( & mIpAddr ) );
    return true;
}

void Wifi::ModeAp()
{
    wifi_config_t wifi_config;
    memset( &wifi_config, 0, sizeof(wifi_config_t) );
    {
        uint8_t mac[6];
        esp_read_mac( mac, ESP_MAC_WIFI_SOFTAP );
        wifi_config.ap.ssid_len = snprintf( (char*) wifi_config.ap.ssid,
                sizeof(wifi_config.ap.ssid), "%s-%02x-%02x-%02x",
                esp_ota_get_app_description()->project_name, mac[3], mac[4],
                mac[5] );
    }
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    // wifi_config.ap.authmode     = WIFI_AUTH_WPA_WPA2_PSK;
    // memcpy( wifi_config.ap.password, ???, sizeof(wifi_config.ap.password) );

    ESP_LOGI( TAG, "Setup AP \"%s\" ...", wifi_config.ap.ssid );
    ESP_ERROR_CHECK( esp_wifi_set_mode( WIFI_MODE_AP ) );
    ESP_ERROR_CHECK( esp_wifi_set_config( ESP_IF_WIFI_AP, &wifi_config ) );
    ESP_ERROR_CHECK( esp_wifi_start() );

    mMode = MODE_ACCESSPOINT;
}

void Wifi::Init( int connTimoInSecs )
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT()
    ;

    ESP_ERROR_CHECK( esp_wifi_init( &wifi_init_config ) );
    ESP_ERROR_CHECK(
            esp_event_handler_register( WIFI_EVENT, ESP_EVENT_ANY_ID, & wifi_event, this ) );

    if (!mSsid[0]) {
        ModeAp();
    } else if (!ModeSta( connTimoInSecs )) {
        ModeAp();
    }
}

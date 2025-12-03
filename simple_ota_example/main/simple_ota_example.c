/* Evaluación SA
   Matrícula: 2023171041
   Sensor: Infrarrojo/Fotoeléctrico
*/

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h"
#include "secrets.h"

// ----- VERSION DEL FIRMWARE 
#define FIRMWARE_VERSION "v4.0.0"  // CASA v2.0.0 y ESCUELA v3.0.0 

static const char *TAG = "SA_IoT";

// Configuración de hardware
#define LED_PIN GPIO_NUM_2
#define IR_SENSOR_PIN GPIO_NUM_21  // Sensor infrarrojo

// Certificado MQTT
extern const uint8_t emqxsl_ca_pem_start[] asm("_binary_emqxsl_ca_pem_start");

// Estructura para datos del sensor
typedef struct {
    bool object_detected;  // true = objeto detectado, false = sin objeto
    int detection_count;   // Contador de detecciones
} sensor_data_t;

// Variables globales
static esp_mqtt_client_handle_t mqtt_client = NULL;
static bool led_state = false;
static bool manual_control = false;
static char device_uuid[37];
static bool ota_requested = false;
static int total_detections = 0;

// ----- Configurar GPIO -----
static void configure_gpio(void) {
    // Configurar LED como salida
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_PIN, 0);
    
    gpio_config_t ir_conf = {
        .pin_bit_mask = (1ULL << IR_SENSOR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,   
        .pull_down_en = GPIO_PULLDOWN_DISABLE,  
    };
    gpio_config(&ir_conf);
    
    ESP_LOGI(TAG, "GPIO configurado - LED:GPIO%d, Sensor IR:GPIO%d", LED_PIN, IR_SENSOR_PIN);
}

// ----- Leer sensor infrarrojo -----
static sensor_data_t read_ir_sensor(void) {
    sensor_data_t result = {0};
    
    // Leer estado del pin
    int sensor_value = gpio_get_level(IR_SENSOR_PIN);
    
    // LOW (0) = detecta objeto 
    if (sensor_value == 0) {
        result.object_detected = true;
        total_detections++;
        result.detection_count = total_detections;
        ESP_LOGI(TAG, "Sensor IR: OBJETO DETECTADO (detección #%d)", total_detections);
    } else {
        result.object_detected = false;
        result.detection_count = total_detections;
        ESP_LOGD(TAG, "Sensor IR: Sin objeto");
    }
    
    return result;
}

// ----- Generar UUID unico del dispositivo -----
static void generate_uuid(void) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_uuid, sizeof(device_uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             mac[0], mac[1], mac[2], mac[3]);
}

// ----- Tarea de actualización OTA -----
static void ota_task(void *pvParameter) {
    ESP_LOGI(TAG, "----- Iniciando OTA -----");
    ESP_LOGI(TAG, "Version actual: %s", FIRMWARE_VERSION);
    
    esp_http_client_config_t config = {
        .url = "https://roc2023171041-215597987518.us-central1.run.app/firmware/latest",
        .crt_bundle_attach = esp_crt_bundle_attach, 
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    
    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    
    ESP_LOGI(TAG, "Descargando firmware...");
    esp_err_t ret = esp_https_ota(&ota_config);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA exitoso, reiniciando...");
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA fallido: %s", esp_err_to_name(ret));
    }
    
    ota_requested = false;
    vTaskDelete(NULL);
}

// ----- Manejador de eventos MQTT -----
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT conectado");
        esp_mqtt_client_subscribe(client, "/class/idgs12/evaluacion/2023171041", 1);
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "Comando recibido: %.*s", event->data_len, event->data);
        
        if (strncmp(event->data, "ON", event->data_len) == 0) {
            gpio_set_level(LED_PIN, 1);
            led_state = true;
            manual_control = true;
            ESP_LOGI(TAG, "LED ON (manual)");
            
        } else if (strncmp(event->data, "OFF", event->data_len) == 0) {
            gpio_set_level(LED_PIN, 0);
            led_state = false;
            manual_control = true;
            ESP_LOGI(TAG, "LED OFF (manual)");
            
        } else if (strncmp(event->data, "AUTO", event->data_len) == 0) {
            manual_control = false;
            // Apagar LED al volver a automático para que el sensor tome control limpio
            gpio_set_level(LED_PIN, 0);
            led_state = false;
            ESP_LOGI(TAG, "Modo AUTOMÁTICO activado - control por sensor (LED reseteado)");
            
        } else if (strncmp(event->data, "OTA", event->data_len) == 0) {
            ESP_LOGI(TAG, "Comando OTA recibido");
            if (!ota_requested) {
                ota_requested = true;
                xTaskCreate(&ota_task, "ota_task", 8192, NULL, 5, NULL);
            }
        }
        break;
        
    default:
        break;
    }
}

// ----- Iniciar MQTT -----
static void mqtt_start(void) {
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = MQTT_URI, // <--- CAMBIO AQUÍ
            .verification.certificate = (const char *)emqxsl_ca_pem_start
        },
        .credentials = {
            .username = MQTT_USER,       // <--- CAMBIO AQUÍ
            .authentication.password = MQTT_PASS, // <--- CAMBIO AQUÍ
            .client_id = MQTT_CLIENT_ID   // <--- CAMBIO AQUÍ
        }
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "Cliente MQTT iniciado");
}

// ----- Tarea principal de sensores -----
static void sensor_task(void *pvParameter) {
    int reading_count = 0;
    
    ESP_LOGI(TAG, "----- Tarea de sensores iniciada -----");
    ESP_LOGI(TAG, "Leyendo sensor cada 2 segundos");
    
    while (1) {
        reading_count++;
        
        // Leer sensor infrarrojo
        sensor_data_t data = read_ir_sensor();
        
        // Control automático del LED 
        if (!manual_control) {
            if (data.object_detected && !led_state) {
                // Objeto detectado -> Encender LED
                gpio_set_level(LED_PIN, 1);
                led_state = true;
                ESP_LOGW(TAG, "OBJETO DETECTADO - LED ON (auto)");
                
            } else if (!data.object_detected && led_state) {
                // Sin objeto -> Apagar LED
                gpio_set_level(LED_PIN, 0);
                led_state = false;
                ESP_LOGI(TAG, "SIN OBJETO - LED OFF (auto)");
            }
        }
        
        // Publicar por MQTT (cada lectura o solo cuando hay cambios)
        if (mqtt_client != NULL) {
            int64_t timestamp = esp_timer_get_time() / 1000000;
            char payload[256];
            
            // Formato:
            // tt = timestamp
            // d = detección (1=sí, 0=no) 
            // c = contador de detecciones 
            // a = estado actuador (LED)
            // v = versión
            // uid = UUID
            snprintf(payload, sizeof(payload),
                     "tt%lld|d%d|c%d|a%d|v%s|uid%s",
                     timestamp, 
                     data.object_detected ? 1 : 0,  // Detección actual
                     data.detection_count,           // Total de detecciones
                     led_state ? 1 : 0,              // Estado LED
                     FIRMWARE_VERSION, 
                     device_uuid);
            
            esp_mqtt_client_publish(mqtt_client, "/class/idgs12/evaluacion/2023171041",
                                  payload, 0, 1, 1);
            ESP_LOGI(TAG, "[%d] Publicado: %s", reading_count, payload);
        }
        
        // Esperar 2 segundos entre lecturas
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

// ----- Función principal -----
void app_main(void) {
    ESP_LOGI(TAG, "---------------------------------------------");
    ESP_LOGI(TAG, "Sistema IoT - SA");
    ESP_LOGI(TAG, "Matricula: 2023171041");
    ESP_LOGI(TAG, "Sensor: Infrarrojo/Fotoelectrico");
    ESP_LOGI(TAG, "Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "---------------------------------------------");
    
    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Configurar GPIO
    configure_gpio();
    
    // Generar UUID
    generate_uuid();
    ESP_LOGI(TAG, "UUID: %s", device_uuid);
    
    // Inicializar red
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Conectar WiFi
    ESP_LOGI(TAG, "Conectando WiFi...");
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "WiFi conectado");
    
    // Iniciar MQTT
    mqtt_start();
    
    // Iniciar tarea de sensores
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Sistema iniciado - esperando detecciones...");
}
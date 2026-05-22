#include "contiki.h"
#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#include "sys/node-id.h"
#include "sys/log.h"

#include "firmware_data.h"
#include "lib/crc16.h"  // crc16 hesabı için sistemin kendi kütüphanesi kullanıldı

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT    8765
#define UDP_SERVER_PORT    5678

#define SEND_INTERVAL         (10 * CLOCK_SECOND)
#define OTA_PAYLOAD_SIZE 64
#define MAX_RETRY 5

#define PACKET_TYPE_METADATA 0x01
#define PACKET_TYPE_DATA     0x02

// FSM DURUMLARI
typedef enum {
    STATE_INIT,           // Ağ bağlantısı bekleniyor
    STATE_SEND_METADATA,  // Metadata gönderilip onayı bekleniyor
    STATE_SEND_DATA,      // Bloklar gönderiliyor
    STATE_DONE,           // Aktarım bitti
    STATE_ERROR           // Hata durumu
} ota_client_state_t;

static ota_client_state_t current_state = STATE_INIT;  // Anlık durum kontrol değişkeni

static uint8_t retry_count = 0;
static uint16_t total_firmware_blocks = 0;

// Matedata paket yapısı
typedef struct __attribute__((packed)) {
    uint8_t packet_type;
    uint16_t total_blocks;
    uint32_t file_size;
    uint32_t total_crc;
    uint16_t meta_crc16;
} ota_metadata_packet_t;

// Data paket yapısı
typedef struct __attribute__((packed)) {
    uint8_t packet_type;
    uint16_t block_num;
    uint8_t payload_len;
    uint16_t block_crc16;
    uint8_t data[OTA_PAYLOAD_SIZE];
} ota_data_packet_t;

// Geri bildirim mekanizması paket yapısı
typedef struct __attribute__((packed)) {
    uint16_t ack_block_num;
    uint8_t is_nack;
} ota_ack_t;

static struct simple_udp_connection udp_conn;
static uint16_t current_block_to_send = 0;
static bool ack_received = true;
static uint32_t expected_total_crc = 0;
static struct etimer periodic_timer;

// CRC32 hesabı yapan fonksiyon
static uint32_t calculate_crc32(const uint8_t *data, uint16_t len, uint32_t current_crc) {
    uint32_t crc = current_crc;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return crc;
}

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client");
AUTOSTART_PROCESSES(&udp_client_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c, const uip_ipaddr_t *sender_addr,
         uint16_t sender_port, const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
         const uint8_t *data, uint16_t datalen) {
    
    if(datalen == sizeof(ota_ack_t)) {
        ota_ack_t reply;
        memcpy(&reply, data, sizeof(ota_ack_t));
        
        // Metadata Onayı
        if(reply.ack_block_num == 0xFFFF) {
            if(current_state == STATE_SEND_METADATA) {
                LOG_INFO("Metadata basariyla gonderildi. Veri aktarimi basliyor!\n");
                current_state = STATE_SEND_DATA;
                etimer_set(&periodic_timer, 1);
            }
            return;
        }
        
        if(current_state == STATE_SEND_DATA) {
            if(reply.is_nack == 1) {
                LOG_INFO("UYARI: Sunucudan ret geldi! Blok %u yeniden gonderilecek.\n", reply.ack_block_num);
                if(reply.ack_block_num == current_block_to_send) {
                    etimer_set(&periodic_timer, 5);
                }
            }
            else {
                LOG_INFO("Sunucudan blok %u icin onay geldi!\n", reply.ack_block_num);
                if(reply.ack_block_num == current_block_to_send) {
                    ack_received = true;
                    current_block_to_send++;
                    etimer_set(&periodic_timer, 5);
                }
            }
        }
    }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data){
    uip_ipaddr_t dest_ipaddr;

    PROCESS_BEGIN();
    
    total_firmware_blocks = (FIRMWARE_PAYLOAD_LEN + OTA_PAYLOAD_SIZE - 1) / OTA_PAYLOAD_SIZE;
    
    expected_total_crc = 0xFFFFFFFF;
    expected_total_crc = calculate_crc32(new_firmware_z1, FIRMWARE_PAYLOAD_LEN, expected_total_crc);
    expected_total_crc ^= 0xFFFFFFFF;

    LOG_INFO("Firmware bellege alindi! Boyut: %u byte, Blok Sayisi: %u, CRC: 0x%08X\n",
             FIRMWARE_PAYLOAD_LEN, total_firmware_blocks, (unsigned int)expected_total_crc);

    simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL, UDP_SERVER_PORT, udp_rx_callback);
    etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
    
    while(1) {
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));
        
        if(node_id != 2) continue; // Sadece Node 2 göndersin
        
        // FSM ANA DÖNGÜ
        switch(current_state) {
            
            case STATE_INIT:
                if(NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
                    LOG_INFO("Ag baglantisi kuruldu!\n");
                    current_state = STATE_SEND_METADATA;
                    etimer_set(&periodic_timer, 1);
                } else {
                    LOG_INFO("Henuz ulasılabilir degil!\n");
                    etimer_set(&periodic_timer, SEND_INTERVAL);
                }
                break;

            case STATE_SEND_METADATA:
                if(NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
                    LOG_INFO("Metadata paketi gonderiliyor!\n");
                    ota_metadata_packet_t meta_packet;
                    meta_packet.packet_type = PACKET_TYPE_METADATA;
                    meta_packet.total_blocks = total_firmware_blocks;
                    meta_packet.file_size = FIRMWARE_PAYLOAD_LEN;
                    meta_packet.total_crc = expected_total_crc;
                    
                    // Metadata'nın kendi iç CRC'si (Son 2 byte hariç kısmın CRC'si)
                    meta_packet.meta_crc16 = crc16_data((uint8_t *)&meta_packet, sizeof(ota_metadata_packet_t) - 2, 0);
                    
                    simple_udp_sendto(&udp_conn, &meta_packet, sizeof(ota_metadata_packet_t), &dest_ipaddr);
                    etimer_set(&periodic_timer, 2 * CLOCK_SECOND);
                }
                break;

            case STATE_SEND_DATA:
                if(current_block_to_send >= total_firmware_blocks && total_firmware_blocks > 0) {
                    LOG_INFO("Tum bloklar basariyla gonderildi!\n");
                    current_state = STATE_DONE;
                    break;
                }

                if(!ack_received) {
                    retry_count++;
                    if(retry_count >= MAX_RETRY) {
                        LOG_INFO("HATA: %u blogu %u denemeye ragmen iletilemedi. Guncelleme iptal ediliyor!\n", current_block_to_send, MAX_RETRY);
                        current_state = STATE_ERROR;
                        break;
                    }
                    LOG_INFO("UYARI: %u blogunun onayi gelmedi, tekrar gonderiliyor!\n", current_block_to_send);
                } else {
                    retry_count = 0;
                    LOG_INFO("Blok %u gonderiliyor!\n", current_block_to_send);
                    ack_received = false;
                }
                
                if(NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
                    ota_data_packet_t data_packet;
                    memset(&data_packet, 0, sizeof(ota_data_packet_t));
                    
                    data_packet.packet_type = PACKET_TYPE_DATA;
                    data_packet.block_num = current_block_to_send;
                    
                    uint32_t offset = (uint32_t)current_block_to_send * OTA_PAYLOAD_SIZE;
                    uint32_t remaining_bytes = FIRMWARE_PAYLOAD_LEN - offset;
                    uint8_t current_payload_len = (remaining_bytes >= OTA_PAYLOAD_SIZE) ? OTA_PAYLOAD_SIZE : (uint8_t)remaining_bytes;
                    
                    data_packet.payload_len = current_payload_len;
                    memcpy(data_packet.data, &new_firmware_z1[offset], current_payload_len);
                    // 1. Önce başlık kısmının (ilk 4 byte) CRC'sini hesapla
                    uint16_t header_crc = crc16_data((uint8_t *)&data_packet, 4, 0);
                    
                    // 2. Başlığın CRC'sini başlangıç değeri olarak verip, verinin CRC'sini üstüne ekle
                    data_packet.block_crc16 = crc16_data(data_packet.data, data_packet.payload_len, header_crc);
                    simple_udp_sendto(&udp_conn, &data_packet, sizeof(ota_data_packet_t), &dest_ipaddr);
                    
                    etimer_set(&periodic_timer, SEND_INTERVAL - CLOCK_SECOND + (random_rand() % (2 * CLOCK_SECOND)));
                }
                break;

            case STATE_DONE:
                break;

            case STATE_ERROR:
                break;
        }
    }

    PROCESS_END();
}

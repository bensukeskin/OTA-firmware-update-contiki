#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"

#include "cfs/cfs.h"
#include "lib/crc16.h"  // crc16 hesabı için sistemin kendi kütüphanesi kullanıldı

#include <stdbool.h>

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT    8765
#define UDP_SERVER_PORT    5678
#define OTA_PAYLOAD_SIZE 64

#define MAX_TOTAL_BLOCKS 2048
#define SET_BIT(array, k)     ( array[(k)/8] |= (1 << ((k)%8)) )
#define CHECK_BIT(array, k)   ( array[(k)/8] & (1 << ((k)%8)) )

#define PACKET_TYPE_METADATA 0x01
#define PACKET_TYPE_DATA     0x02

// FSM DURUMLARI
typedef enum {
    STATE_IDLE,         // Metadata bekleniyor
    STATE_DOWNLOADING,  // Metadata alındı, bloklar bekleniyor
    STATE_VALIDATING,   // Tüm bloklar geldi, CRC32 kontrol ediliyor
    STATE_DONE,         // Doğrulama başarılı, işlem bitti
    STATE_ERROR         // Hata durumu
} ota_server_state_t;

static ota_server_state_t current_state = STATE_IDLE; // Anlık durumu takip ettiğimiz değişken

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

static uint16_t expected_total_blocks = 0;
static uint32_t expected_file_size = 0;
static uint32_t expected_final_crc = 0;

static struct simple_udp_connection udp_conn;
static uint8_t received_blocks[MAX_TOTAL_BLOCKS / 8];
static uint16_t unique_blocks_received = 0;
static int ota_fd = -1;

// CRC32 hesaplayan fonksiyon
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
PROCESS(udp_server_process, "UDP server");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c, const uip_ipaddr_t *sender_addr,
         uint16_t sender_port, const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
         const uint8_t *data, uint16_t datalen) {
    
    if(datalen == 0) return;
    uint8_t type = data[0];

    // METADATA İŞLEME
    if(type == PACKET_TYPE_METADATA) {
        if(datalen != sizeof(ota_metadata_packet_t)) return;
        
        ota_metadata_packet_t meta;
        memcpy(&meta, data, sizeof(ota_metadata_packet_t));
        
        // Metadata CRC Kontrolü
        uint16_t calc_meta_crc = crc16_data((uint8_t *)&meta, sizeof(ota_metadata_packet_t) - 2, 0);
        if(calc_meta_crc != meta.meta_crc16) {
            LOG_INFO("HATA: Metadata paketi yolda bozulmus, yeniden gonderme bekleniyor!\n");
            return;
        }
        
        // IDLE -> DOWNLOADING
        if(current_state == STATE_IDLE) {
            expected_total_blocks = meta.total_blocks;
            expected_file_size = meta.file_size;
            expected_final_crc = meta.total_crc;
            current_state = STATE_DOWNLOADING;
            
            LOG_INFO("Metadata Bilgileri Alindi! Blok Sayisi: %u, Boyut: %lu, CRC: 0x%08X\n",
                     expected_total_blocks, (unsigned long)expected_file_size, (unsigned int)expected_final_crc);
        }
        
        if(current_state == STATE_DOWNLOADING) {
            ota_ack_t ack;
            ack.ack_block_num = 0xFFFF;
            ack.is_nack = 0;
            simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
        }
        return;
    }

    // DATA İŞLEME
    if(type == PACKET_TYPE_DATA) {
        if(current_state != STATE_DOWNLOADING) return;
        if(datalen != sizeof(ota_data_packet_t)) return;
        
        ota_data_packet_t data_packet;
        memcpy(&data_packet, data, sizeof(ota_data_packet_t));
        
        if(data_packet.block_num >= MAX_TOTAL_BLOCKS || data_packet.payload_len > OTA_PAYLOAD_SIZE) return;

        // Önce başlığın CRC'si alınır
        uint16_t calc_crc = crc16_data((uint8_t *)&data_packet, 4, 0);
        
        // Üzerine veri kısmını eklenir
        calc_crc = crc16_data(data_packet.data, data_packet.payload_len, calc_crc);
        if(calc_crc != data_packet.block_crc16) {
            LOG_INFO("HATA: Blok %u yolda bozulmus, yeniden gonderme bekleniyor!\n", data_packet.block_num);
            ota_ack_t nack;
            nack.ack_block_num = data_packet.block_num;
            nack.is_nack = 1;
            simple_udp_sendto(&udp_conn, &nack, sizeof(ota_ack_t), sender_addr);
            return;
        }

        if(CHECK_BIT(received_blocks, data_packet.block_num)) {
            ota_ack_t ack;
            ack.ack_block_num = data_packet.block_num;
            ack.is_nack = 0;
            simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
            return;
        }

        SET_BIT(received_blocks, data_packet.block_num);
        unique_blocks_received++;
        LOG_INFO("Blok %u alindi! Ilerleme: (%u/%u)\n", data_packet.block_num, unique_blocks_received, expected_total_blocks);

        if(ota_fd < 0) ota_fd = cfs_open("new-firmware.z1", CFS_WRITE);
        if(ota_fd >= 0) {
            cfs_seek(ota_fd, data_packet.block_num * OTA_PAYLOAD_SIZE, CFS_SEEK_SET);
            cfs_write(ota_fd, data_packet.data, data_packet.payload_len);
        }

        // DOWNLOADING -> VALIDATING
        if(unique_blocks_received == expected_total_blocks) {
            current_state = STATE_VALIDATING;
            process_poll(&udp_server_process);
        }

        ota_ack_t ack;
        ack.ack_block_num = data_packet.block_num;
        ack.is_nack = 0;
        simple_udp_sendto(&udp_conn, &ack, sizeof(ota_ack_t), sender_addr);
    }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data) {
    PROCESS_BEGIN();
    
    cfs_remove("new-firmware.z1");
    NETSTACK_ROUTING.root_start();
    simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL, UDP_CLIENT_PORT, udp_rx_callback);
    
    while(1) {
        PROCESS_YIELD();

        // VALIDATING
        if(current_state == STATE_VALIDATING) {
            if(ota_fd >= 0) {
                cfs_close(ota_fd);
                ota_fd = -1;
            }
            
            LOG_INFO("\n=================================================\n");
            LOG_INFO("Diskteki imaj dogrulaniyor!\n");
            
            int read_fd = cfs_open("new-firmware.z1", CFS_READ);
            if(read_fd >= 0) {
                uint32_t final_crc = 0xFFFFFFFF;
                uint8_t read_buf[OTA_PAYLOAD_SIZE];
                int bytes_read;
                
                while((bytes_read = cfs_read(read_fd, read_buf, sizeof(read_buf))) > 0) {
                    final_crc = calculate_crc32(read_buf, bytes_read, final_crc);
                }
                cfs_close(read_fd);
                
                final_crc ^= 0xFFFFFFFF;
                
                LOG_INFO("Hesaplanan CRC32: 0x%08X\n", (unsigned int)final_crc);
                LOG_INFO("Beklenen CRC32:   0x%08X\n", (unsigned int)expected_final_crc);
                
                if(final_crc == expected_final_crc) {
                    current_state = STATE_DONE; // VALIDATING -> DONE
                    LOG_INFO("Dogrulama Basarili!\n");
                }
                else {
                    current_state = STATE_ERROR; // VALIDATING -> ERROR
                    LOG_INFO("HATA: CRC uyusmazligi!\n");
                }
                LOG_INFO("=================================================\n");
            }
            else {
                current_state = STATE_ERROR;
                LOG_INFO("HATA: Kaydedilen imaj okunurken disk hatasi olustu!\n");
            }
        }
    }

    PROCESS_END();
}

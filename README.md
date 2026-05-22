## Problemin Tanımı

- Wireless Sensor Networks, genellikle ulaşması güç bölgelerde konuşlandırılan, enerji ve bellek kısıtlarına sahip çokça gömülü sistemden oluşur. Bu cihazların yazılımlarının fiziksel bir kablo bağlantısı ile güncellenmesi pratik değildir. Bu bağlamda OTA mimarisi, WSN cihazlarının uzaktan, güvenilir ve denetimli bir şekilde yeni firmware imajlarıyla güncellenmesini sağlayan bir çözümüdür.
- Bu proje kapsamında; kısıtlı kaynaklara sahip MSP430 tabanlı Z1 düğümleri üzerinde, Contiki-NG işletim sistemi ve Cooja simülatörü kullanılarak bir OTA firmware aktarım mekanizması tasarlanmış ve gerçeklenmiştir.

## Rol Dağılımı ve Ağ Topolojisi
- Gönderici Düğüm (Node 2): Firmware imajını belleğe alır, parçalara böler ve gönderimi başlatır.
- Aracı Düğüm (Node 3): RPL yönlendirme mekanizması ile gönderici düğümden gelen paketleri alıcı düğüme iletir.
- Alıcı Düğüm (Node 1 - Root): Paketleri karşılar, doğrulama yapar, kayıpları bildirir ve nihai firmware'i diske yazar.


## Sistem Mimarisi ve Durum Yönetimi
- Bu proje, ağdaki senkronizasyonu sağlamak, okunurluğu artırmak ve hata yönetimini kolaylaştırmak amacıyla hem alıcı hem de gönderici düğümler için deterministik bir sonlu durum makinesi (FSM) mimarisiyle tasarlanmıştır.

### İstemci FSM
```c
typedef enum {
    STATE_INIT,          
    STATE_SEND_METADATA, 
    STATE_SEND_DATA,     
    STATE_DONE,          
    STATE_ERROR          
} ota_client_state_t;
```
- STATE_INIT: Sistem açıldığında routing protokolünün hedef IP adresini çözümlemesini beklediği durumdur.
- STATE_SEND_METADATA: Metadata paketinin gönderildiği ve sunucu tarafından bu paketin onaylanmasının beklendiği durumdur. Zaman aşımı yöntemiyle korunur.
- STATE_SEND_DATA: Metadata için onay geldikten sonra bu duruma geçilir. Dosya okuma/gönderme işlemi bu durum içerisinde yapılır.
- STATE_DONE: Aktarımın başarıyla tamamlanması durumunda geçilen durumdur.
- STATE_ERROR: Hata olması durumunda bu duruma gelinir ve aktarım iptal olur.

### Sunucu FSM
```c
typedef enum {
    STATE_IDLE,    
    STATE_DOWNLOADING,
    STATE_VALIDATING,
    STATE_DONE,  
    STATE_ERROR      
} ota_server_state_t;
```
- STATE_IDLE: Metadata paketinin beklendiği durumdur.
- STATE_DOWNLOADING: Gelen blokların CFS ile diske yazıldığı indirme durumudur.
- STATE_VALIDATING: Tüm bloklar geldikten sonra diske yazılan imajın doğrulamasının yapıldığı durumdur.
- STATE_DONE: Doğrulamanın başarılı olması durumunda gelinen durumdur.
- STATE_ERROR: Hata olması durumunda gelinen durumdur.

## Paket Yapıları

### Metadata Paketi

- Veri aktarımına başlamadan önce, alıcı düğüme gönderilecek dosyanın parametrelerini bildiren öncü pakettir.
```c
typedef struct __attribute__((packed)) {
    uint8_t packet_type;
    uint16_t total_blocks;
    uint32_t file_size;
    uint32_t total_crc;
    uint16_t meta_crc16;
} ota_metadata_packet_t;
```
- packet_type: Gönderilecek paket türünü belirtir. Bu değer metedata için 0x01 olur.
- total_blocks: Firmware’in kaç parçaya bölündüğünü belirtir.
- file_size: Dosyanın kaç byte olduğunu belirtir.
- total_crc: Tüm imaj doğrulamasında kullanılacak CRC-32 değerini belirtir.
- meta_crc16: Yukarıda açıklanan parametrelerin bütünlüğünün iletim sırasında korunması için kullanılan CRC-16 değerini belirtir.

### Data Paketi

- Asıl dosya verisinin parçalara bölünüp taşındığı pakettir. 
```c
typedef struct __attribute__((packed)) {
    uint8_t packet_type;
    uint16_t block_num;
    uint8_t payload_len;
    uint16_t block_crc16;
    uint8_t data[OTA_PAYLOAD_SIZE];
} ota_data_packet_t;
```
- packet_type: Paketin türünü belirtir. Data için bu değer 0x02 olur.
- block_num: Parçanın dosya içindeki sıra numarasını belirtir. Offset hesabında kullanılır.
- payload_len: Paketteki geçerli veri uzunluğunu belirtir. Dosyalar normalde sabit 64 byte uzunluğunda bölünmüş olsa da son blok 64’ten küçük olabilir.
- block_crc_16: Hem yukarıda açıklanan parametreleri hem de gönderilecek bloğu doğrulamada kullanılacak kümülatif CRC-16 değeri.
- data[OTA_PAYLOAD_SIZE]: Firmware verisini taşıyan dizi.

### Geri Bildirim Paketi

- Alıcı düğümün, gelen paketlerin durumunu göndericiye bildirdiği paket yapısıdır.
```c
typedef struct __attribute__((packed)) {
    uint16_t ack_block_num;
    uint8_t is_nack;
} ota_ack_t;
```
- ack_block_num: Hangi bloğun yanıtlandığını belirtir. 
- is_nack: 0 olması durumunda ACK yani onay anlamında, 1 olması durumunda NACK yani ret/hata anlamındadır. Yeniden gönderim için kullanılan ‘Stop and Wait’ mekanizmasına ek olarak hatalı bir veri geldiğinde zaman aşımı beklenmeden gönderimin gerçekleşmesi için eklenmiştir.

## Hata Kontrolü ve Bütünlük

- Telsiz duyarga ağlarında kablosuz ortamın gürültülü doğası gereği, paketlerin iletimi sırasında fiziksel kırpılmalar (truncation) veya bit terslenmeleri (bit-flip) yaşanması kaçınılmazdır. Yanlış bir bitin kalıcı belleğe yazılması, tüm işletim sisteminin veya uygulamanın çökmesine sebep olabilir. Bu projede, ağ katmanı ve dosya sistemi arasında tam bir güvenlik bariyeri kurmak amacıyla iki aşamalı bir CRC mimarisi tasarlanmıştır: Aktarım sırasında blok bazlı CRC-16 ve aktarım sonunda tüm imaj için CRC-32.

### Uçtan Uca ve Kümülatif Blok Doğrulaması (CRC-16)

- Aktarım esnasında parçalara ayrılan her bir paketin yolda bozulup bozulmadığını anlık olarak denetlemek için Contiki-NG sistem kütüphanesinde yer alan (lib/crc16.h) standart CRC-16 algoritması kullanılmıştır. Sistem kütüphanesinden alınan kaynak kodu aşağıda verilmiştir.
```c
unsigned short
crc16_add(unsigned char b, unsigned short acc)
{
  acc ^= b;
  acc  = (acc >> 8) | (acc << 8);
  acc ^= (acc & 0xff00) << 4;
  acc ^= (acc >> 8) >> 4;
  acc ^= (acc & 0xff00) >> 5;
  return acc;
}
```
```c
unsigned short
crc16_data(const unsigned char *data, int len, unsigned short acc)
{
  int i;
  
  for(i = 0; i < len; ++i) {
    acc = crc16_add(*data, acc);
    ++data;
  }
  return acc;
}
```
- Geleneksel tabanlı (lookup table) CRC yöntemlerinin aksine, bu fonksiyon her bir baytı sırayla alır (crc16_add) ve acc (accumulator) değişkeni üzerinde bit kaydırma (shift) ve XOR işlemleri uygulayarak sonucu hesaplar. Bu yaklaşım, Z1 gibi kısıtlı RAM'e sahip sistemlerde bellek israfını önlemektedir.

#### Metadata Doğrulaması

Metadata paketinin içerdiği file_size ve total_crc gibi kritik veriler nedeniyle doğru aktarılması oldukça önemlidir.
Paketin gönderilmeden önce son iki baytlık kendi CRC alanı hariç tüm yapısı fonksiyona sokulur ve bütünlük değeri pakete eklenir.
Sunucu, bu değer uyuşmazsa timeout olarak hatalı paketi reddeder.

#### Data Paketi ve Kümülatif Doğrulama

- Gelen paketin yazılacağı yer blok numarasına bağlı olduğu için bu değerin doğru ulaşması sistem açısından oldukça kritiktir. Bu yüzden veri paketlerinde yalnızca yükün doğrulanması büyük bir zaafiyet doğurur. Yalnızca yükün doğrulandığı bir senaryoda eğer paketin sadece blok numarası havada değişirse veri kısmı sağlam olduğu için CRC doğrulamasından geçer ancak sunucu veriyi diskin yanlış bir bölgesine yazmış olur. Bu zaafiyeti önlemek için projede kümülatif CRC yaklaşımı tasarlanmıştır.
```c
uint16_t header_crc = crc16_data((uint8_t *)&data_packet, 4, 0);

data_packet.block_crc16 = crc16_data(data_packet.data, data_packet.payload_len, header_crc);
```
- Önce başlık kısmının ilk 4 byte’ının yani tip, blok numarası ve veri boyutunun CRC değeri hesaplanır. 
- Başlığın CRC değeri verinin CRC hesabına eklenir.

### Tüm İmaj Doğrulaması (CRC-32)

- Tüm parçalar hatasız şekilde CFS üzerinden diske yazıldıktan sonra, parçaların birleşim sırasının ve dosya bütünlüğünün nihai onayı için 32-bitlik bir CRC işlemi gerçekleştirilir.
- Z1 donanımının sınırlı kaynakları göz önüne alınarak, tablo gerektirmeyen, doğrudan bit seviyesinde çalışan bir CRC-32 algoritması tasarlanmıştır.
- Dosya diskin üzerinden 64 byte’lık parçalar halinde sırayla okunur. calculate_crc32 fonksiyonu, her parçayı bir önceki parçanın ara durum değeriyle birleştirir.
- Tüm dosya okunduğunda, elde edilen nihai değer ana döngü içerisinde final_crc ^= 0xFFFFFFFF; komutu ile terslenir.
```c
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
```
- Fonksiyon doğrulanacak verinin bellekteki başlangıç adresi (const uint8_t *data), işleme sokulacak verinin byte uzunluğu (uint16_t len) ve önceki parçadan kalan mevcut CRC değeri (uint32_t current_crc) parametrelerini alır.
- Gönderilen veririnin her bir byte’ının okunması için bir döngü başlatılır.
- O an okunan veri byte’ı, mevcut 32 bitlik CRC değerinin en alt 8 bitine XOR’lanır.
- XOR’lanan byte’ın her bir bitini tek tek işlemek için 8 adımlık bir iç döngü başlatılır.
- Mevcut 32 bitlik CRC’nin LSB değeri kontrol edilir.
- Eğer bu bit 1 ise if bloğuna girilir. Tüm sayı 1 bit sağa kaydırılır, kaydırılan bu değer 0xEDB88320 ile XOR’lanır.
- Eğer bu bit 0 ise sayı sadece 1 bit sağa kaydırılır. Bu işlem ile bir sonraki bit sıraya alınmış olur.
- Tüm veri byte’ları işlendikten sonra hesaplanan 32 bitlik crc değeri döndürülür.

## Aktarım Stratejisi ve Parçalama

- Veri yükü ağ sınırları gözetilerek OTA_PAYLOAD_SIZE ile sabit 64 byte olarak belirlenmiştir.
- Gönderici düğüm, toplam gönderilecek blok sayısını ceiling division formülüyle dinamik olarak hesaplar.
```c
total_firmware_blocks = (FIRMWARE_PAYLOAD_LEN + OTA_PAYLOAD_SIZE - 1) / OTA_PAYLOAD_SIZE;
```
- Gönderim sırasında sıradaki bloğun ana dosyanın belleğinde tam olarak hangi noktaya denk geldiği, bir offset hesabı ile bulunur. 
- Dosyanın tam 64'ün katı olmaması durumunda, son paketin içine çöp veri girmemesi için geçerli byte uzunluğu dinamik hesaplanıp pakete eklenir.
```c
uint32_t offset = (uint32_t)current_block_to_send * OTA_PAYLOAD_SIZE;
uint32_t remaining_bytes = FIRMWARE_PAYLOAD_LEN - offset;
uint8_t current_payload_len = (remaining_bytes >= OTA_PAYLOAD_SIZE) ? OTA_PAYLOAD_SIZE : (uint8_t)remaining_bytes;
```
## Stop and Wait ve ACK/NACK Yaklaşımı

- Veri iletiminin senkronizasyonu için donanım kaynaklarını en az yoran Stop-and-Wait algoritması tercih edilmiştir.
- Bu yaklaşımda istemci bir paketi gönderir, zamanlayıcısını başlatır ve onay gelene kadar yeni paket göndermez. Bu onay bekleme mekanizması sistemi önemli derecede yavaşlatır.
- Bu projede, süreci hızlandırmak için sunucu tarafına hızlı tekrar gönderim yeteneği kazandıran bir NACK (ret) mekanizması eklenmiştir.
```c
if(reply.is_nack == 1) {
    if(reply.ack_block_num == current_block_to_send) {
        etimer_set(&periodic_timer, 5);
    }
}
```
- Paket sağlam ulaşırsa ACK döner ve sistem sıradaki bloğa geçer.
- Ancak paket CRC doğrulamasından geçemezse yani hatalıysa sunucu anında NACK gönderir.
- İstemci bu NACK'i aldığında zaman aşımını beklemez, döngüyü milisaniyeler içinde tetikleyerek bozuk paketi tekrar yollar.
- Toplam deneme sayısı MAX_RETRY (5) değeri aşılırsa sistem STATE_ERROR durumuna geçerek aktarımı iptal eder.

## Blok Yönetimi ve CFS Entegrasyonu

### Blok Takibi

- Sunucunun çok sayıda bloğun gelip gelmediğini takip etmesi gerekir. Her bir blok için standart bir dizi kullanmak, kısıtlı Z1 RAM'ini hızla tüketecektir.
- Bunun yerine, her bir bloğun durumu tek bir bit (0 veya 1) ile ifade edilecek şekilde bir bitmap tasarlanmıştır.
```c
#define SET_BIT(array, k)     ( array[(k)/8] |= (1 << ((k)%8)) )
#define CHECK_BIT(array, k)   ( array[(k)/8] & (1 << ((k)%8)) )
```
static uint8_t received_blocks[MAX_TOTAL_BLOCKS / 8];

- Sunucu, bir paketi işleme almadan önce CHECK_BIT ile ilgili alana bakar. Eğer blok daha önce alınmışsa, diski yormadan anında ACK gönderip paketi atlar.

### Diske Yazma

- Onaydan geçen ve ilk defa gelen bloklar, Contiki-NG'nin yerleşik dosya sistemi olan CFS (Coffee File System) kullanılarak kalıcı Flash belleğe doğrudan yazılır.
- Paketlerin sırasız gelme ihtimaline karşı veriler doğrudan sona eklenmez, bloğun sıra numarasına göre diskte spesifik bir adres (offset) hesaplanarak cfs_seek ile o noktaya gidilir ve yazım işlemi cfs_write ile gerçekleştirilir.
- Bu mimari sayede, paketler ağ üzerinden karmaşık bir sırada da gelse diskte birleştirilen imaj doğru sıralamaya sahip olur.
```c
if(ota_fd >= 0) {
    cfs_seek(ota_fd, data_packet.block_num * OTA_PAYLOAD_SIZE, CFS_SEEK_SET);
    cfs_write(ota_fd, data_packet.data, data_packet.payload_len);
}
```

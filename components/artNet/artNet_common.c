// ***************************************************************************
// TITLE
//     Art-Net: сокет, разбор ArtDMX / ArtPoll, ответ ArtPollReply
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_mac.h"

#include "lwip/err.h"
#include "lwip/sockets.h"

#include "stateConfig.h"
#include "artNet.h"
#include <mbdebug.h>

/* CONFIG_LOG_MAXIMUM_LEVEL в проекте = INFO, поэтому ESP_LOGD включается
   локально - так же, как в остальных модулях. */
#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define TAG "artNet"

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define ARTNET_OP_POLL          0x2000
#define ARTNET_OP_POLLREPLY     0x2100
#define ARTNET_OP_DMX           0x5000
#define ARTNET_OP_SYNC          0x5200

#define ARTNET_HEADER_LEN       12          /* ID + OpCode + ProtVer            */
#define ARTNET_DMX_HEADER_LEN   18          /* до начала DMX-данных             */
#define ARTNET_POLLREPLY_LEN    239
#define ARTNET_RX_BUF_LEN       700         /* максимальный ArtDMX - 530 байт   */

/* Кадры перестали приходить дольше этого - нумерация Sequence сбрасывается.
   Перезапуск пульта не должен блокировать приём "старыми" номерами. */
#define ARTNET_SEQ_RESET_US     (2 * 1000 * 1000)

static const char ARTNET_ID[8] = { 'A', 'r', 't', '-', 'N', 'e', 't', 0 };

// ---------------------------------------------------------------------------
// -------------------------------- VARIABLES --------------------------------
// -----|-------------------|-------------------------------------------------

extern configuration me_config;
extern stateStruct   me_state;

static artnet_universe_t   universes[ARTNET_MAX_UNIVERSES];
static SemaphoreHandle_t   registryLock = NULL;
static bool                artnetReady  = false;   /* реестр создан            */
static int                 artnetSocket = -1;

static uint32_t            statPollsRx   = 0;
static uint32_t            statRepliesTx = 0;
static uint32_t            statFramesRx  = 0;
static uint32_t            statDropped   = 0;      /* кадры чужих юниверсов    */

static char                statusString[48];

// ---------------------------------------------------------------------------
// -------------------------------- REGISTRY ---------------------------------
// -----|-------------------|-------------------------------------------------

bool artnet_is_enabled(void)
{
	return artnetReady && me_config.artNet_enable;
}

static artnet_universe_t * findUniverse(uint16_t universe)
{
	for (int i = 0; i < ARTNET_MAX_UNIVERSES; i++)
	{
		if (universes[i].inUse && (universes[i].universe == universe))
			return &universes[i];
	}
	return NULL;
}

/* Завести юниверс в реестре. Вызывать под registryLock. */
static artnet_universe_t * claimUniverse(uint16_t universe)
{
	artnet_universe_t * u = findUniverse(universe);
	if (u)
		return u;

	for (int i = 0; i < ARTNET_MAX_UNIVERSES; i++)
	{
		if (!universes[i].inUse)
		{
			u = &universes[i];
			memset(u, 0, sizeof(*u));
			u->universe = universe;
			u->inUse    = 1;
			u->lock     = xSemaphoreCreateMutex();
			for (int s = 0; s < ARTNET_MAX_SUBS; s++)
				u->subSlot[s] = -1;

			if (!u->lock)
			{
				u->inUse = 0;
				ESP_LOGE(TAG, "no memory for universe %d mutex", universe);
				return NULL;
			}
			return u;
		}
	}

	ESP_LOGE(TAG, "universe registry is full (%d), universe %d ignored", ARTNET_MAX_UNIVERSES, universe);
	mblog(E, "artNet: universe registry is full (%d), universe %d ignored", ARTNET_MAX_UNIVERSES, universe);
	return NULL;
}

artnet_universe_t * artnet_subscribe(uint16_t universe, int slot_num, TaskHandle_t notify_task)
{
	if (!artnet_is_enabled())
		return NULL;

	xSemaphoreTake(registryLock, portMAX_DELAY);

	artnet_universe_t * u = claimUniverse(universe);

	if (u)
	{
		int free_idx = -1;
		int mine_idx = -1;

		for (int s = 0; s < ARTNET_MAX_SUBS; s++)
		{
			if (u->subSlot[s] == slot_num) mine_idx = s;
			else if ((u->subSlot[s] < 0) && (free_idx < 0)) free_idx = s;
		}

		int idx = (mine_idx >= 0) ? mine_idx : free_idx;

		if (idx < 0)
		{
			ESP_LOGE(TAG, "universe %d already has %d subscribers, slot_%d rejected", universe, ARTNET_MAX_SUBS, slot_num);
			u = NULL;
		}
		else
		{
			u->subSlot[idx] = slot_num;
			u->subTask[idx] = notify_task;
			ESP_LOGI(TAG, "slot_%d subscribed to universe %d", slot_num, universe);
		}
	}

	xSemaphoreGive(registryLock);
	return u;
}

void artnet_unsubscribe(uint16_t universe, int slot_num)
{
	if (!artnetReady)
		return;

	xSemaphoreTake(registryLock, portMAX_DELAY);

	artnet_universe_t * u = findUniverse(universe);

	if (u)
	{
		for (int s = 0; s < ARTNET_MAX_SUBS; s++)
		{
			if (u->subSlot[s] == slot_num)
			{
				u->subSlot[s] = -1;
				u->subTask[s] = NULL;
			}
		}
	}

	xSemaphoreGive(registryLock);
}

bool artnet_is_alive(const artnet_universe_t *u)
{
	if (!u || !u->last_rx_us)
		return false;

	return (esp_timer_get_time() - u->last_rx_us) < ((int64_t)me_config.artNet_timeout * 1000);
}

const char * artnetGetStatusString(void)
{
	if (!artnet_is_enabled())
	{
		snprintf(statusString, sizeof(statusString), "ArtNet off");
	}
	else
	{
		int live = 0, total = 0;

		for (int i = 0; i < ARTNET_MAX_UNIVERSES; i++)
		{
			if (universes[i].inUse)
			{
				total++;
				if (artnet_is_alive(&universes[i])) live++;
			}
		}

		snprintf(statusString, sizeof(statusString), "ArtNet %d/%du %luf %lup",
				live, total, (unsigned long)statFramesRx, (unsigned long)statPollsRx);
	}

	return statusString;
}

// ---------------------------------------------------------------------------
// --------------------------- NETWORK INTERFACE -----------------------------
// -----|-------------------|-------------------------------------------------

/* Интерфейс с валидным адресом: сначала Ethernet, затем WiFi STA, затем AP. */
static esp_netif_t * activeNetif(void)
{
	static const char * keys[] = { "ETH_DEF", "WIFI_STA_DEF", "WIFI_AP_DEF" };

	for (int i = 0; i < 3; i++)
	{
		esp_netif_t * netif = esp_netif_get_handle_from_ifkey(keys[i]);
		esp_netif_ip_info_t ip;

		if (netif && (esp_netif_get_ip_info(netif, &ip) == ESP_OK) && ip.ip.addr)
			return netif;
	}

	return NULL;
}

// ---------------------------------------------------------------------------
// ------------------------------ ARTPOLLREPLY -------------------------------
// -----|-------------------|-------------------------------------------------

/* Версия прошивки в два байта: "3.70" -> VersInfoH = 3, VersInfoL = 70. */
static void versionBytes(uint8_t *hi, uint8_t *lo)
{
	const char * dot = strchr(VERSION, '.');

	*hi = (uint8_t)atoi(VERSION);
	*lo = dot ? (uint8_t)atoi(dot + 1) : 0;
}

/* Собрать один ArtPollReply. group[] - юниверсы одного набора (совпадают
   старшие 11 бит Port-Address), их не больше ARTNET_PORTS_PER_REPLY. */
static void buildPollReply(uint8_t *buf,
                           esp_netif_t *netif,
                           const artnet_universe_t * const *group,
                           int groupCount,
                           uint8_t bindIndex)
{
	esp_netif_ip_info_t ip;
	uint8_t             mac[6] = { 0 };
	uint8_t             verHi, verLo;
	char                text[72];

	memset(buf, 0, ARTNET_POLLREPLY_LEN);
	memset(&ip, 0, sizeof(ip));

	if (netif)
	{
		esp_netif_get_ip_info(netif, &ip);
		esp_netif_get_mac(netif, mac);
	}

	versionBytes(&verHi, &verLo);

	uint16_t portAddress = groupCount ? group[0]->universe : 0;

	memcpy(buf + 0, ARTNET_ID, 8);                      /* ID                       */
	buf[8]  = ARTNET_OP_POLLREPLY & 0xFF;               /* OpCode, младший первым   */
	buf[9]  = ARTNET_OP_POLLREPLY >> 8;
	memcpy(buf + 10, &ip.ip.addr, 4);                   /* IP Address               */
	buf[14] = ARTNET_PORT & 0xFF;                       /* Port, младший первым     */
	buf[15] = ARTNET_PORT >> 8;
	buf[16] = verHi;                                    /* VersInfoH                */
	buf[17] = verLo;                                    /* VersInfoL                */
	buf[18] = (portAddress >> 8) & 0x7F;                /* NetSwitch                */
	buf[19] = (portAddress >> 4) & 0x0F;                /* SubSwitch                */
	buf[20] = me_config.artNet_oem >> 8;                /* OemHi                    */
	buf[21] = me_config.artNet_oem & 0xFF;              /* OemLo                    */
	buf[22] = 0;                                        /* UbeaVersion              */

	/* Status1: индикаторы в нормальном режиме (11), адреса портов заданы
	   локально конфигурацией, а не по сети (01). */
	buf[23] = 0xD0;

	buf[24] = me_config.artNet_estaMan & 0xFF;          /* EstaManLo                */
	buf[25] = me_config.artNet_estaMan >> 8;            /* EstaManHi                */

	strncpy((char*)(buf + 26), me_config.artNet_shortName, 17);   /* ShortName 18 б */
	strncpy((char*)(buf + 44), me_config.artNet_longName, 63);    /* LongName  64 б */

	snprintf(text, sizeof(text), "#0001 [%04lu] %s", (unsigned long)(statRepliesTx % 10000), me_config.deviceName);
	strncpy((char*)(buf + 108), text, 63);              /* NodeReport 64 б          */

	buf[172] = 0;                                       /* NumPortsHi               */
	buf[173] = groupCount;                              /* NumPortsLo               */

	for (int i = 0; i < groupCount; i++)
	{
		buf[174 + i] = 0x80;                            /* PortTypes: выход DMX512  */
		buf[178 + i] = 0x00;                            /* GoodInput                */
		buf[182 + i] = artnet_is_alive(group[i]) ? 0x80 : 0x00;  /* GoodOutput      */
		buf[186 + i] = 0x00;                            /* SwIn                     */
		buf[190 + i] = group[i]->universe & 0x0F;       /* SwOut                    */
		buf[213 + i] = 0x80;                            /* GoodOutputB: RDM выключен*/
	}

	buf[200] = 0x00;                                    /* Style: StNode            */
	memcpy(buf + 201, mac, 6);                          /* MAC                      */
	memcpy(buf + 207, &ip.ip.addr, 4);                  /* BindIp                   */
	buf[211] = bindIndex;                               /* BindIndex, с единицы     */

	/* Status2: 15-битные Port-Address (Art-Net 3/4), DHCP поддерживается,
	   бит 1 - адрес действительно получен по DHCP. */
	buf[212] = 0x0C | ((me_config.LAN_DHCP || me_config.WIFI_DHCP) ? 0x02 : 0x00);

	buf[217] = 0x00;                                    /* Status3                  */
	buf[226] = 0;                                       /* RefreshRate hi           */
	buf[227] = 44;                                      /* RefreshRate lo           */
}

/* Разослать ArtPollReply по всем обслуживаемым юниверсам. Порты, у которых
   различаются старшие биты Port-Address, уезжают отдельными пакетами.

   unicastTo != NULL - адрес приславшего ArtPoll: копия ответа уходит ещё и
   ему лично. Одного бродкаста мало: направленный бродкаст считается от нашей
   маски, и если у пульта маска уже (частый случай - нода получила по DHCP /16,
   а пульт настроен на /24), его стек отбрасывает пакет как чужой. */
static void sendPollReply(const struct sockaddr_in *unicastTo)
{
	if (artnetSocket < 0)
		return;

	esp_netif_t *       netif = activeNetif();
	esp_netif_ip_info_t ip;
	struct sockaddr_in  dest;
	static uint8_t      buf[ARTNET_POLLREPLY_LEN];

	memset(&ip, 0, sizeof(ip));
	if (netif)
		esp_netif_get_ip_info(netif, &ip);

	/* Ответ по протоколу широковещательный. Направленный бродкаст подсети
	   не покидает её пределов и не мешает соседним сегментам. */
	memset(&dest, 0, sizeof(dest));
	dest.sin_family      = AF_INET;
	dest.sin_port        = htons(ARTNET_PORT);
	dest.sin_addr.s_addr = (ip.ip.addr && ip.netmask.addr)
	                       ? (ip.ip.addr | ~ip.netmask.addr)
	                       : htonl(INADDR_BROADCAST);

	xSemaphoreTake(registryLock, portMAX_DELAY);

	const artnet_universe_t * group[ARTNET_PORTS_PER_REPLY];
	uint8_t                   bindIndex = 1;
	uint8_t                   done[ARTNET_MAX_UNIVERSES] = { 0 };
	int                       announced = 0;

	for (int i = 0; i < ARTNET_MAX_UNIVERSES; i++)
	{
		if (!universes[i].inUse || done[i])
			continue;

		int      count = 0;
		uint16_t hi    = universes[i].universe >> 4;

		for (int j = i; j < ARTNET_MAX_UNIVERSES; j++)
		{
			if (universes[j].inUse && !done[j] && ((universes[j].universe >> 4) == hi)
				&& (count < ARTNET_PORTS_PER_REPLY))
			{
				group[count++] = &universes[j];
				done[j] = 1;
			}
		}

		buildPollReply(buf, netif, group, count, bindIndex);
		sendto(artnetSocket, buf, ARTNET_POLLREPLY_LEN, 0, (struct sockaddr*)&dest, sizeof(dest));

		if (unicastTo && (unicastTo->sin_addr.s_addr != dest.sin_addr.s_addr))
			sendto(artnetSocket, buf, ARTNET_POLLREPLY_LEN, 0, (struct sockaddr*)unicastTo, sizeof(*unicastTo));

		statRepliesTx++;
		bindIndex++;
		announced += count;
	}

	/* Ни одного юниверса не объявлено - нода всё равно обязана отозваться,
	   иначе пульт просто не увидит её в списке. */
	if (!announced)
	{
		buildPollReply(buf, netif, NULL, 0, 1);
		sendto(artnetSocket, buf, ARTNET_POLLREPLY_LEN, 0, (struct sockaddr*)&dest, sizeof(dest));

		if (unicastTo && (unicastTo->sin_addr.s_addr != dest.sin_addr.s_addr))
			sendto(artnetSocket, buf, ARTNET_POLLREPLY_LEN, 0, (struct sockaddr*)unicastTo, sizeof(*unicastTo));

		statRepliesTx++;
	}

	xSemaphoreGive(registryLock);

	static bool firstReply = true;

	if (firstReply)
	{
		firstReply = false;
		ESP_LOGI(TAG, "ArtPollReply -> %s%s, ports:%d", inet_ntoa(dest.sin_addr),
				unicastTo ? " (+unicast)" : "", announced);
	}
	else
	{
		ESP_LOGD(TAG, "ArtPollReply sent, ports:%d packets:%d", announced, announced ? (bindIndex - 1) : 1);
	}
}

// ---------------------------------------------------------------------------
// -------------------------------- ARTDMX -----------------------------------
// -----|-------------------|-------------------------------------------------

static void handleDmx(const uint8_t *pkt, int len)
{
	if (len < ARTNET_DMX_HEADER_LEN)
		return;

	uint8_t  seq      = pkt[12];
	uint16_t universe = ((uint16_t)pkt[15] << 8) | pkt[14];      /* Net<<8 | SubUni  */
	uint16_t dmxLen   = ((uint16_t)pkt[16] << 8) | pkt[17];      /* big-endian       */

	if (dmxLen > ARTNET_UNIVERSE_SIZE)
		dmxLen = ARTNET_UNIVERSE_SIZE;

	if (dmxLen > (len - ARTNET_DMX_HEADER_LEN))
		dmxLen = len - ARTNET_DMX_HEADER_LEN;

	if (dmxLen < 2)
		return;

	artnet_universe_t * u = findUniverse(universe);

	if (!u)
	{
		statDropped++;
		return;
	}

	int64_t now = esp_timer_get_time();

	/* Sequence == 0 - нумерация отключена отправителем. Долгая тишина
	   означает перезапуск источника, счётчик принимаем как есть. */
	if (seq && u->seq && ((now - u->last_rx_us) < ARTNET_SEQ_RESET_US))
	{
		if ((int8_t)(seq - u->seq) <= 0)
			return;
	}

	xSemaphoreTake(u->lock, portMAX_DELAY);
	memcpy(u->data, pkt + ARTNET_DMX_HEADER_LEN, dmxLen);
	u->length = dmxLen;
	xSemaphoreGive(u->lock);

	u->seq        = seq;
	u->last_rx_us = now;
	u->frames++;
	statFramesRx++;

	if (u->frames == 1)
		ESP_LOGI(TAG, "universe %d: stream started, %d channels", universe, dmxLen);

	for (int s = 0; s < ARTNET_MAX_SUBS; s++)
	{
		if ((u->subSlot[s] >= 0) && u->subTask[s])
			xTaskNotifyGive(u->subTask[s]);
	}

	if (me_config.artNet_dmxLog)
	{
		static int64_t lastLog = 0;

		if ((now - lastLog) > 1000000)
		{
			lastLog = now;
			ESP_LOGI(TAG, "u%d len:%d seq:%d ch1-8: %d %d %d %d %d %d %d %d",
					universe, dmxLen, seq,
					u->data[0], u->data[1], u->data[2], u->data[3],
					u->data[4], u->data[5], u->data[6], u->data[7]);
		}
	}
}

// ---------------------------------------------------------------------------
// --------------------------------- TASK ------------------------------------
// -----|-------------------|-------------------------------------------------

static void artNet_task(void *arg)
{
	int64_t             replyDueUs = 0;      /* когда отправить ArtPollReply    */
	struct sockaddr_in  replyTo;             /* кому продублировать юникастом   */
	bool                replyToValid = false;
	uint8_t *           buf = malloc(ARTNET_RX_BUF_LEN);
	struct sockaddr_in  bindAddr;
	struct sockaddr_in  srcAddr;
	socklen_t           srcLen = sizeof(srcAddr);
	int                 opt = 1;

	if (!buf)
	{
		mblog(E, "artNet: no memory for rx buffer");
		vTaskDelete(NULL);
		return;
	}

	artnetSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (artnetSocket < 0)
	{
		mblog(E, "artNet: socket() failed, errno %d", errno);
		free(buf);
		vTaskDelete(NULL);
		return;
	}

	setsockopt(artnetSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	setsockopt(artnetSocket, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

	memset(&bindAddr, 0, sizeof(bindAddr));
	bindAddr.sin_family      = AF_INET;
	bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	bindAddr.sin_port        = htons(ARTNET_PORT);

	if (bind(artnetSocket, (struct sockaddr*)&bindAddr, sizeof(bindAddr)) < 0)
	{
		mblog(E, "artNet: bind on port %d failed, errno %d", ARTNET_PORT, errno);
		close(artnetSocket);
		artnetSocket = -1;
		free(buf);
		vTaskDelete(NULL);
		return;
	}

	/* Приём просыпается четыре раза в секунду даже в тишине: так отрабатывают
	   таймауты потоков и уходит отложенный ArtPollReply. */
	struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
	setsockopt(artnetSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	ESP_LOGI(TAG, "listening on UDP %d", ARTNET_PORT);
	mblog(I, "artNet started on port %d", ARTNET_PORT);

	/* Нода объявляет себя при включении, не дожидаясь ArtPoll. Случайная
	   задержка - требование протокола, чтобы пачка нод не отвечала разом. */
	if (me_config.artNet_pollReply)
	{
		vTaskDelay(pdMS_TO_TICKS(200 + (esp_random() % 800)));
		sendPollReply(NULL);
	}

	while (1)
	{
		int len = recvfrom(artnetSocket, buf, ARTNET_RX_BUF_LEN, 0, (struct sockaddr*)&srcAddr, &srcLen);

		/* Отложенный ответ на ArtPoll. Спать прямо в обработчике нельзя:
		   пульт опрашивает сеть в среднем раз в 2-5 с, и полусекундная пауза
		   в приёме - это два десятка потерянных кадров DMX. */
		if (replyDueUs && (esp_timer_get_time() >= replyDueUs))
		{
			replyDueUs = 0;
			sendPollReply(replyToValid ? &replyTo : NULL);
			replyToValid = false;
		}

		if (len < ARTNET_HEADER_LEN)
			continue;

		if (memcmp(buf, ARTNET_ID, 8) != 0)
			continue;

		uint16_t opCode  = ((uint16_t)buf[9] << 8) | buf[8];     /* little-endian    */
		uint16_t protVer = ((uint16_t)buf[10] << 8) | buf[11];   /* big-endian       */

		if (protVer < 14)
			continue;

		switch (opCode)
		{
			case ARTNET_OP_DMX:
				handleDmx(buf, len);
				break;

			case ARTNET_OP_POLL:
				statPollsRx++;
				ESP_LOGD(TAG, "ArtPoll from %s", inet_ntoa(srcAddr.sin_addr));

				/* Случайная задержка ответа - требование протокола: иначе
				   десяток нод отвечает одновременно и пульт теряет пакеты. */
				if (me_config.artNet_pollReply && !replyDueUs)
				{
					replyDueUs   = esp_timer_get_time() + (esp_random() % 500) * 1000;
					replyTo      = srcAddr;
					replyTo.sin_port = htons(ARTNET_PORT);
					replyToValid = true;
				}
				break;

			case ARTNET_OP_SYNC:
				/* ArtSync - этап 4, пока молча игнорируем. */
				break;

			default:
				break;
		}
	}
}

// ---------------------------------------------------------------------------
// ----------------------------- INITIALIZATION ------------------------------
// -----|-------------------|-------------------------------------------------

esp_err_t init_artNet(void)
{
	if (!me_config.artNet_enable)
	{
		ESP_LOGI(TAG, "artNet protocol is disabled in config");
		return ESP_ERR_INVALID_STATE;
	}

	registryLock = xSemaphoreCreateMutex();

	if (!registryLock)
	{
		mblog(E, "artNet: no memory for registry mutex");
		return ESP_ERR_NO_MEM;
	}

	memset(universes, 0, sizeof(universes));

	/* Имена ноды: пусто в конфиге - берём deviceName. ShortName пульты
	   показывают в списке устройств, LongName - в свойствах. */
	if (!me_config.artNet_shortName || !*me_config.artNet_shortName)
	{
		free(me_config.artNet_shortName);
		me_config.artNet_shortName = strdup(me_config.deviceName);
	}

	if (!me_config.artNet_longName || !*me_config.artNet_longName)
	{
		char tmp[80];
		snprintf(tmp, sizeof(tmp), "moduleBox %s", me_config.deviceName);
		free(me_config.artNet_longName);
		me_config.artNet_longName = strdup(tmp);
	}

	artnetReady = true;

	/* Список обслуживаемых юниверсов не задаётся отдельным ключом: он целиком
	   складывается из artnet_subscribe() слот-модулей. Второй источник правды
	   рано или поздно разошёлся бы с реальной конфигурацией слотов. */
	ESP_LOGI(TAG, "registry ready: '%s' / '%s', waiting for slot subscriptions",
			me_config.artNet_shortName, me_config.artNet_longName);

	return ESP_OK;
}

void start_artNet_task(void)
{
	if (!artnet_is_enabled())
	{
		ESP_LOGI(TAG, "artNet is not enabled, receiver not started");
		return;
	}

	xTaskCreatePinnedToCore(artNet_task, "task_artNet", 1024 * 4, NULL, configMAX_PRIORITIES - 4, NULL, 0);
}

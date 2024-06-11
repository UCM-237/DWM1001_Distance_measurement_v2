/**
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ch.h"
#include "hal.h"
#include "led.h"
#include "chprintf.h"

#include "dw1000_hal.h"
#include "LR-WPANs_MAC.h"

#define THREAD_STACK_SIZE	2048
#define NEIGHBOUR_NUM		2
#define MIN_D_MEASURES		10
#define CONN_MSG_TMO		10
#define CONN_MSG_TMO_MAX	1

#define MCPLOCK_E	(EVENT_MASK(1))
#define MESYNCR_E	(EVENT_MASK(2))
#define MAAT_E		(EVENT_MASK(3))
#define MTXFRB_E	(EVENT_MASK(4))
#define MTXPRS_E	(EVENT_MASK(5))
#define MTXPHS_E	(EVENT_MASK(6))
#define MTXFRS_E	(EVENT_MASK(7))
#define MRXPRD_E	(EVENT_MASK(8))
#define MRXFSDD_E	(EVENT_MASK(9))
#define MLDEDONE_E	(EVENT_MASK(10))
#define MRXPHD_E	(EVENT_MASK(11))
#define MRXPHE_E	(EVENT_MASK(12))
#define MRXDFR_E	(EVENT_MASK(13))
#define MRXFCG_E	(EVENT_MASK(14))
#define MRXFCE_E	(EVENT_MASK(15))
#define MRXRFSL_E	(EVENT_MASK(16))
#define MRXRFTO_E	(EVENT_MASK(17))
#define MLDEERR_E	(EVENT_MASK(18))
#define MRXOVRR_E	(EVENT_MASK(20))
#define MRXPTO_E	(EVENT_MASK(21))
#define MGPIOIRQ_E	(EVENT_MASK(22))
#define MSLP2INIT_E	(EVENT_MASK(23))
#define MRFPLLLL_E	(EVENT_MASK(24))
#define MCPLLLL_E	(EVENT_MASK(25))
#define MRXSFDTO_E	(EVENT_MASK(26))
#define MHPDWARN_E	(EVENT_MASK(27))
#define MTXBERR_E	(EVENT_MASK(28))
#define MAFFREJ_E	(EVENT_MASK(29))

#define DW_COMM_OK_E	(EVENT_MASK(30))

#define MRXERR_E	(MRXPHE_E | MRXFCE_E | MRXRFSL_E | MRXSFDTO_E | MAFFREJ_E | MLDEERR_E)

#define TX_TIMEOUT	TIME_MS2I(10)
#define CH_TIMEOUT	TIME_S2I(5)
#define DW_ERR_THRESH	10
#define PEER_CONN_TTL	16

extern thread_reference_t irq_evt;

extern SPIConfig spi_cfg;
extern SerialConfig serial_cfg;

extern thread_t* dw_thread;
extern thread_t* peer_conn_thread;
extern thread_t* peer_disc_thread;
extern thread_t* comm_thread;

typedef enum loc_state
{
	LOC_STANDBY,
	LOC_INIT,
	LOC_COMM,
	LOC_TWR,
	LOC_ERR
} loc_state_t;

typedef enum twr_state
{
	TWR_REQ_SENT,		// start TWR
	TWR_REQ_RECVD,		// start TWR send ack and wait for init
	TWR_REQ_ACK_RECVD,	// send init and wait for resp
	TWR_INIT_RECVD, 	// resp sent automatically
	TWR_RESP_RECVD,		// send res and wait res_ack
	TWR_NO_TWR,			
	TWR_FAIL
} twr_state_t;

typedef enum dw_ctrl_req
{
	DW_RECV,
	DW_SEND,
	DW_SEND_W4R,
	DW_SEND_DLY,
	DW_TRX_ERR,
	DW_RESET,
	DW_RECV_TMO,
	DW_CTRL_YIELD
} dw_ctrl_req_t;

typedef enum comm_state
{
	COMM_RECV,
	COMM_SEND,
	COMM_ERR,
	COMM_IDLE
} comm_state_t;

typedef enum conn_state
{
	CONN_SYN_RECV,
	CONN_SYN_SEND,
	CONN_SYN_A_SEND,
	CONN_SYN_A_RECV,
	CONN_RECV,
	CONN_SEND,
	CONN_SEND_ACK,
	CONN_MNT,
	CONN_DIS,
	CONN_ERR,
	CONN_IDLE
} conn_state_t;

typedef enum message_types_dis_loc
{
	MT_BROADCAST	= 0x01,
	MT_SYN			= 0x11,
	MT_SYN_ACK		= 0x12,
	MT_ACK			= 0x13,
	MT_DISCONN		= 0x14,
	MT_D_REQ		= 0x21,
	MT_D_REQ_ACK	= 0x22,
	MT_D_INIT		= 0x23,
	MT_D_RESP		= 0x24,
	MT_D_FAIL		= 0x25,
	MT_D_RES		= 0x26,
	MT_D_RES_ACK	= 0x27,
	MT_MCONN		= 0x31,
	MT_OTHER		= 0xFE
} message_t;

typedef enum loc_action
{
	LOC_RESP,
	LOC_RESP_BTMO,
	LOC_RESP_NOW,
	LOC_NO_RESP,
	LOC_ACT_ERR,
	LOC_STOP
} loc_action_t;

typedef struct send_msg_meta
{
	int32_t wtime;
	uint32_t dlytime;
	uint8_t size;
	uint8_t seq_ack_num;
	message_t type;
	uint16_t addr;
} send_msg_meta_t;

typedef struct connection_peer
{
	uint16_t peer_addr;
	uint8_t seq_ack_n;				// 0b	000 	ack	000	seq
	uint8_t ttl;
	virtual_timer_t tmo_timer;
	uint8_t last_message[120];
	uint8_t last_message_size;
	message_t last_message_type;
	message_t last_cmd_type;
} peer_connection_t;

typedef struct peer_loc
{
	uint8_t peer_id;
	peer_connection_t* conn;
	float calc_distance;
	float recvd_distance;
	uint8_t d_measures;
} peer_info_t;

typedef struct euclidean_d_m
{
	uint16_t addrs[NEIGHBOUR_NUM+1];
	float distances[NEIGHBOUR_NUM+1][NEIGHBOUR_NUM+1];
} euclidean_d_m_t;

void ISR_wrapper(void * arg);

void _dw_power_on(void);
void _dw_power_off(void);
void spi1_lock(void);
void spi1_unlock(void);
void spi1_set_cs(void);
void spi1_clear_cs(void);
void spi1_send(size_t count, const uint8_t* buf);
void spi1_recv(size_t count, const uint8_t* buf);

static THD_WORKING_AREA(DW_IRQ_THREAD, 256);
extern THD_FUNCTION(DW_IRQ_HANDLER, arg);

static THD_WORKING_AREA(DW_CONTROLLER_THREAD, THREAD_STACK_SIZE);
extern THD_FUNCTION(DW_CONTROLLER, arg);

static THD_WORKING_AREA(COMMS_THREAD, 4098);
extern THD_FUNCTION(COMMS, arg);

// static THD_WORKING_AREA(DIS_LOC_THREAD, THREAD_STACK_SIZE);
// extern THD_FUNCTION(DIS_LOC, arg);

static THD_WORKING_AREA(SYSTEM_STATUS_THREAD, 256);
extern THD_FUNCTION(SYSTEM_STATUS, arg);

void dw_reset(void);

void set_fast_spi_freq(void);
void set_slow_spi_freq(void);

uint64_t get_hardware_id(void);

void spi_hal_init(void);

// TODO document must not be preempted
// TODO solve magic number for size
void load_lde(void);

uint64_t load_ldotune(void);

void set_irq_vector(void);

void dw_setup(void);

void read_frame(void);

void clean_recvd(void);
void clean_send(void);

int32_t get_message(void);
void prepare_message(void);

int8_t _get_address_index(uint16_t addr);
float get_distance(uint16_t addr1, uint16_t addr2);
void set_distance(uint16_t addr1, uint16_t addr2, float distance);

void init_peers(void);
peer_connection_t* create_new_peer(uint16_t addr);
peer_connection_t* get_peer(uint16_t addr);
peer_info_t* get_peer_info(uint16_t addr);
peer_connection_t* get_unconn_peer(void);
peer_connection_t* get_conn_peer(void);

void peer_tmo_cb(virtual_timer_t* vtp, void* arg);
void connect_peer(peer_connection_t* peer);
void disconnect_peer(peer_connection_t* peer);

void send_syn(void);
void send_broad(void);
void send_ack(peer_connection_t* peer);
void send_last_message(peer_connection_t* peer);
void send_d_req(void);
void send_d_req_ack(peer_connection_t* peer);
void send_conn_msg(peer_connection_t* peer, uint8_t size, message_t type);
void send_w4r_msg(peer_connection_t* peer, uint8_t size, message_t type);

void process_req(void);
void handle_twr_fail(void);
void twr_handle(peer_connection_t* peer);
void conn_handle(peer_connection_t* peer);

void no_resp_action(void);
void process_message(void);

int8_t respond_if_twr(void);

void compute_distance(void);

void CPLOCK_handler(void);
void ESYNCR_handler(void);
void AAT_handler(void);
void TXFRB_handler(void);
void TXPRS_handler(void);
void TXPHS_handler(void);
void TXFRS_handler(void);
void RXPRD_handler(void);
void RXFSDD_handler(void);
void LDEDONE_handler(void);
void RXPHD_handler(void);
void RXPHE_handler(void);
void RXDFR_handler(void);
void RXFCG_handler(void);
void RXFCE_handler(void);
void RXRFSL_handler(void);
void RXRFTO_handler(void);
void LDEERR_handler(void);
void RXOVRR_handler(void);
void RXPTO_handler(void);
void GPIOIRQ_handler(void);
void SLP2INIT_handler(void);
void RFPLLLL_handler(void);
void CPLLLL_handler(void);
void RXSFDTO_handler(void);
void HPDWARN_handler(void);
void TXBERR_handler(void);
void AFFREJ_handler(void);
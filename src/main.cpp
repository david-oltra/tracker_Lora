#include <Arduino.h>
#include <LoRaWan-Arduino.h>
#include <SPI.h>
#include <TinyGPSPlus.h>
#include <HardwareSerial.h>
#include <string>

// GPS setup definitions
static const int RXPin = 13, TXPin = 12;
static const uint32_t GPSBaud = 9600;

// The TinyGPSPlus object
TinyGPSPlus gps;

// The serial connection to the GPS device
HardwareSerial ss(1);

// LoRaWan setup definitions
#define SCHED_MAX_EVENT_DATA_SIZE APP_TIMER_SCHED_EVENT_DATA_SIZE // Maximum size of scheduler events
#define SCHED_QUEUE_SIZE 60	// Maximum number of events in the scheduler queue

/**< Maximum number of events in the scheduler queue  */
#define LORAWAN_APP_DATA_BUFF_SIZE 256 // Size of the data to be transmitted
#define LORAWAN_APP_TX_DUTYCYCLE 60000 // Defines the application data transmission duty cycle. 30s, value in [ms]
#define APP_TX_DUTYCYCLE_RND 1000 // Defines a random delay for application data transmission duty cycle. 1s, value in [ms]
#define JOINREQ_NBTRIALS 3	

bool doOTAA = true;
hw_config hwConfig;

#ifdef ESP32
// ESP32 - SX126x pin configuration
int PIN_LORA_RESET = 4;	 // LORA RESET
int PIN_LORA_NSS = 5;	 // LORA SPI CS
int PIN_LORA_SCLK = 18;	 // LORA SPI CLK
int PIN_LORA_MISO = 19;	 // LORA SPI MISO
int PIN_LORA_MOSI = 23;	 // LORA SPI MOSI
int PIN_LORA_BUSY = 22;	 // LORA SPI BUSY
int PIN_LORA_DIO_1 = 21; // LORA DIO_1
int RADIO_TXEN = 26;	 // LORA ANTENNA TX ENABLE
int RADIO_RXEN = 27;	 // LORA ANTENNA RX ENABLE
#endif

// Define Helium or TTN OTAA keys. All msb (big endian).
uint8_t nodeDeviceEUI[8] = {0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x06, 0x4C, 0x6A};
uint8_t nodeAppEUI[8] = {0x70, 0xB3, 0xD5, 0x14, 0x03, 0x19, 0x86, 0x12};
uint8_t nodeAppKey[16] = {0x69, 0x51, 0x56, 0x4A, 0xD3, 0xA9, 0xBA, 0x57, 0x66, 0xD8, 0x56, 0x31, 0xC2, 0xF7, 0x8F, 0xAE};

// Foward declaration
/** LoRaWAN callback when join network finished */
static void lorawan_has_joined_handler(void);
/** LoRaWAN callback when join network failed */
static void lorawan_join_fail_handler(void);
/** LoRaWAN callback when data arrived */
static void lorawan_rx_handler(lmh_app_data_t *app_data);
/** LoRaWAN callback after class change request finished */
static void lorawan_confirm_class_handler(DeviceClass_t Class);
/** LoRaWAN callback after class change request finished */
static void lorawan_unconfirm_tx_finished(void);
/** LoRaWAN callback after class change request finished */
static void lorawan_confirm_tx_finished(bool result);
/** LoRaWAN Function to send a package */
static void send_lora_frame(void);
static uint32_t timers_init(void);

// APP_TIMER_DEF(lora_tx_timer_id);	 // LoRa tranfer timer instance.
TimerEvent_t appTimer;	 // LoRa tranfer timer instance.
static uint8_t m_lora_app_data_buffer[LORAWAN_APP_DATA_BUFF_SIZE]; // Lora user application data buffer.
static lmh_app_data_t m_lora_app_data = {m_lora_app_data_buffer, 0, 0, 0, 0};	 // Lora user application data structure.

/**@brief Structure containing LoRaWan parameters, needed for lmh_init()
 */
static lmh_param_t lora_param_init = {LORAWAN_ADR_OFF, DR_3, LORAWAN_PUBLIC_NETWORK,
										JOINREQ_NBTRIALS, LORAWAN_DEFAULT_TX_POWER, LORAWAN_DUTYCYCLE_OFF};

static lmh_callback_t lora_callbacks = {BoardGetBatteryLevel, BoardGetUniqueId, BoardGetRandomSeed,
										lorawan_rx_handler, lorawan_has_joined_handler, 
										lorawan_confirm_class_handler, lorawan_join_fail_handler,
										lorawan_unconfirm_tx_finished, lorawan_confirm_tx_finished};

void setup()
{
	// Initialize Serial for debug output
	Serial.begin(115200);

	Serial.println("=====================================");
	Serial.println("GPS -> TTN  test");
	Serial.println("=====================================");

	// Initialize Serial GPS
	ss.begin(GPSBaud,SERIAL_8N1,RXPin,TXPin);	

	// Define the HW configuration between MCU and SX126x
	hwConfig.CHIP_TYPE = SX1262_CHIP;	// Example uses an eByte E22 module with an SX1262
	hwConfig.PIN_LORA_RESET = PIN_LORA_RESET; // LORA RESET
	hwConfig.PIN_LORA_NSS = PIN_LORA_NSS;	  // LORA SPI CS
	hwConfig.PIN_LORA_SCLK = PIN_LORA_SCLK;	  // LORA SPI CLK
	hwConfig.PIN_LORA_MISO = PIN_LORA_MISO;	  // LORA SPI MISO
	hwConfig.PIN_LORA_DIO_1 = PIN_LORA_DIO_1; // LORA DIO_1
	hwConfig.PIN_LORA_BUSY = PIN_LORA_BUSY;	  // LORA SPI BUSY
	hwConfig.PIN_LORA_MOSI = PIN_LORA_MOSI;	  // LORA SPI MOSI
	hwConfig.RADIO_TXEN = RADIO_TXEN;	// LORA ANTENNA TX ENABLE (e.g. eByte E22 module)
	hwConfig.RADIO_RXEN = RADIO_RXEN;	// LORA ANTENNA RX ENABLE (e.g. eByte E22 module)
	hwConfig.USE_DIO2_ANT_SWITCH = false;	// LORA DIO2 does not control antenna
	hwConfig.USE_DIO3_TCXO = true;	// LORA DIO3 controls oscillator voltage (e.g. eByte E22 module)
	hwConfig.USE_DIO3_ANT_SWITCH = false;	// LORA DIO3 does not control antenna


	// Initialize LoRa chip.
	uint32_t err_code = lora_hardware_init(hwConfig);
	if (err_code != 0)
	{
		Serial.printf("lora_hardware_init failed - %d\n", err_code);
	}


	// Initialize Scheduler and timer (Must be after lora_hardware_init)
	err_code = timers_init();
	if (err_code != 0)
	{
		Serial.printf("timers_init failed - %d\n", err_code);
	}

	// Setup the EUIs and Keys
	lmh_setDevEui(nodeDeviceEUI);
	lmh_setAppEui(nodeAppEUI);
	lmh_setAppKey(nodeAppKey);

	// Initialize LoRaWan
	// CLASS C works for esp32 and e22, US915 region works in america, other local frequencies can be found 
	// here https://docs.helium.com/lorawan-on-helium/frequency-plans/
	
	err_code = lmh_init(&lora_callbacks, lora_param_init, doOTAA, CLASS_A, LORAMAC_REGION_EU868);
	if (err_code != 0)
	{
		Serial.printf("lmh_init failed - %d\n", err_code);
	}

	// For Helium and US915, you need as well to select subband 2 after you called lmh_init(), 
	// For US816 you need to use subband 1. Other subbands configurations can be found in
	// https://github.com/beegee-tokyo/SX126x-Arduino/blob/1c28c6e769cca2b7d699a773e737123fc74c47c7/src/mac/LoRaMacHelper.cpp

	lmh_setSubBandChannels(2);

	// Start Join procedure
//	lmh_join();
}

void loop()
{
	while (ss.available() > 0)
    	if (gps.encode(ss.read())){
		}
	if(gps.location.isValid()){

		// Start Join procedure
		Serial.println(lmh_join_status_get());
		switch(lmh_join_status_get()){
			case 0:
				lmh_join();
				break;
			case 3:
				lmh_join();
				break;
			default:
				break;
		}
	}
	else{
		Serial.printf("lat: %d\tlng: %d\n", gps.location.lat(), gps.location.lng());
	}
	delay(1000);
}

static void lorawan_join_fail_handler(void)
{
	Serial.println("OTAA joined failed");
	Serial.println("Check LPWAN credentials and if a gateway is in range");
	// Restart Join procedure
	Serial.println("Restart network join request");
}

/**@brief LoRa function for handling HasJoined event.
 */
static void lorawan_has_joined_handler(void)
{
#if (OVER_THE_AIR_ACTIVATION != 0)
	Serial.println("Network Joined");
#else
	Serial.println("OVER_THE_AIR_ACTIVATION != 0");

#endif
	lmh_class_request(CLASS_A);

	TimerSetValue(&appTimer, LORAWAN_APP_TX_DUTYCYCLE);
	TimerStart(&appTimer);
	// app_timer_start(lora_tx_timer_id, APP_TIMER_TICKS(LORAWAN_APP_TX_DUTYCYCLE), NULL);
	Serial.println("Sending frame");
//	send_lora_frame();
}

/**@brief Function for handling LoRaWan received data from Gateway
 *
 * @param[in] app_data  Pointer to rx data
 */
static void lorawan_rx_handler(lmh_app_data_t *app_data)
{
	Serial.printf("LoRa Packet received on port %d, size:%d, rssi:%d, snr:%d\n",
				  app_data->port, app_data->buffsize, app_data->rssi, app_data->snr);

	for (int i = 0; i < app_data->buffsize; i++)
	{
		Serial.printf("%0X ", app_data->buffer[i]);
	}
	Serial.println("");

	switch (app_data->port)
	{
	case 3:
		// Port 3 switches the class
		if (app_data->buffsize == 1)
		{
			switch (app_data->buffer[0])
			{
			case 0:
				lmh_class_request(CLASS_A);
				break;

			case 1:
				lmh_class_request(CLASS_B);
				break;

			case 2:
				lmh_class_request(CLASS_C);
				break;

			default:
				break;
			}
		}
		break;

	case LORAWAN_APP_PORT:
		// YOUR_JOB: Take action on received data
		break;

	default:
		break;
	}
}

/**@brief Function to confirm LORaWan class switch.
 *
 * @param[in] Class  New device class
 */
static void lorawan_confirm_class_handler(DeviceClass_t Class)
{
	Serial.printf("switch to class %c done\n", "ABC"[Class]);

	// Informs the server that switch has occurred ASAP
	m_lora_app_data.buffsize = 0;
	m_lora_app_data.port = LORAWAN_APP_PORT;
	lmh_send(&m_lora_app_data, LMH_UNCONFIRMED_MSG);
}

/**
 * @brief Called after unconfirmed packet was sent
 * 
 */
static void lorawan_unconfirm_tx_finished(void)
{
	Serial.println("Uncomfirmed TX finished");
}

/**
 * @brief Called after confirmed packet was sent
 * @param result Result of sending true = ACK received false = No ACK
 */
static void lorawan_confirm_tx_finished(bool result)
{
	Serial.printf("Comfirmed TX finished with result %s", result ? "ACK" : "NAK");
}

/**@brief Function for sending a LoRa package.
 */
static void send_lora_frame(void)
{
	
	if (lmh_join_status_get() != LMH_SET)
	{
		//Not joined, try again later
		Serial.println("Did not join network, skip sending frame");
		return;
	}

/*
	String buff = String(gps.location.lat(),6);
	buff += ",";
	buff += String(gps.location.lng(),6);

	m_lora_app_data.port = LORAWAN_APP_PORT;
	for (int i=0; i<buff.length(); i++){
		m_lora_app_data.buffer[i] = buff[i];
		Serial.print(buff[i]);
	}
	m_lora_app_data.buffsize = buff.length();
	Serial.println(" ");
*/
	int32_t data_lat = gps.location.lat() * 10000;
	int32_t data_lon = gps.location.lng() * 10000;
	int32_t data_alt = gps.altitude.meters ()* 100;
	int8_t data_prueba[11];
	m_lora_app_data.port = LORAWAN_APP_PORT;
	m_lora_app_data.buffer[0] = 01;
	m_lora_app_data.buffer[1] = 136;
	m_lora_app_data.buffer[2] = data_lat >> 16;
	m_lora_app_data.buffer[3] = data_lat >> 8;
	m_lora_app_data.buffer[4] = data_lat;
	m_lora_app_data.buffer[5] = data_lon >> 16;
	m_lora_app_data.buffer[6] = data_lon >> 8;
	m_lora_app_data.buffer[7] = data_lon;
	m_lora_app_data.buffer[8] = data_alt >> 16;
	m_lora_app_data.buffer[9] = data_alt >> 8;
	m_lora_app_data.buffer[10] = data_alt;
	m_lora_app_data.buffsize = 11;


	lmh_error_status error = lmh_send(&m_lora_app_data, LMH_UNCONFIRMED_MSG);
	if (error == LMH_SUCCESS)
	{
	}
	Serial.printf("lmh_send result %d\n", error);
	Serial.print(m_lora_app_data.buffer[0]);
	Serial.print(m_lora_app_data.buffer[1]);
	Serial.print(m_lora_app_data.buffer[2]);
	Serial.print(m_lora_app_data.buffer[3]);
	Serial.print(m_lora_app_data.buffer[4]);
	Serial.print(m_lora_app_data.buffer[5]);
	Serial.print(m_lora_app_data.buffer[6]);
	Serial.print(m_lora_app_data.buffer[7]);
	Serial.print(m_lora_app_data.buffer[8]);
	Serial.print(m_lora_app_data.buffer[9]);
	Serial.println(m_lora_app_data.buffer[10]);
}

/**@brief Function for handling a LoRa tx timer timeout event.
 */
static void tx_lora_periodic_handler(void)
{
	TimerSetValue(&appTimer, LORAWAN_APP_TX_DUTYCYCLE);
	TimerStart(&appTimer);
	//Only send data every 15 minutes
	if ((gps.location.isValid()) && (gps.time.minute()%5 == 0) || gps.time.minute() == 0){
		Serial.println("Sending frame");
		send_lora_frame();
	}
	else {
		if(!gps.location.isValid()){
			Serial.println("GPS lost");
		}
		else{
			Serial.printf("Minute: %u\n", gps.time.minute());
		}
	}
}


static uint32_t timers_init(void)
{
	appTimer.timerNum = 3;
	TimerInit(&appTimer, tx_lora_periodic_handler);
	return 0;
}


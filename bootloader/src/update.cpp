/*
 *  app_main.cpp - The application's main.
 *
 *  Copyright (c) 2015 Martin Glueck <martin@mangari.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3 as
 *  published by the Free Software Foundation.
 */

#include <eib.h>
#include <eib/bus.h>
#include <eib/apci.h>
//#include <sblib/internal/iap.h>
#include <string.h>
//#include <sblib/io_pin_names.h>
//#include <sblib/serial.h>
#include <bcu_updater.h>
#include <crc.h>
#include <boot_descriptor_block.h>
#include "Decompressor.h"

//TODO:
#define IAP_SUCCESS 0
#define iapReadUID(x) IAP_SUCCESS
#define iapEraseSector(x) IAP_SUCCESS
#define iapProgram(x,y,z) IAP_SUCCESS
#define iapErasePage(x) IAP_SUCCESS

/**
 * Updater protocol:
 *   We miss-use the memory write EIB frames. Miss-use because we do not transmitt the address in each request
 *   to have more frame left for the actal data transmission:
 *     BYTES of teh EIB telegram:
 *       8    CMD Nummer (see enum below)
 *       9-x  CMD dependent
 *
 *    UPD_ERASE_SECTOR
 *      9    Number of the sector which should be erased
 *           if the erasing was successful a T_ACK_PDU will be returned, otherwise a T_NACK_PDU
 *    UPD_SEND_DATA
 *      9-   the actual data which will be copied into a RAM buffer for later use
 *           If the RAM buffer is not yet full a T_ACK_PDU will be returned, otherwise a T_NACK_PDU
 *           The address of the RAM buffer will be automatically incremented.
 *           After a Program or Boot Desc Aupdate, the RAM buffer address will be reseted.
 *    UPD_PROGRAM
 *      9-12 How many bytes of the RMA Buffer should be programmed. Be aware that ths value nees to be one of the following
 *           256, 512, 1024, 4096 (required by the IAP of the LPC11xx devices)
 *     13-16 Flash address the data should be programmed to
 *     16-19 The CRC of the data downloaded via the UPD_SEND_DATA commands. If the CRC does not match the
 *           programming returns an error
 *    UPD_UPDATE_BOOT_DESC
 *    UPD_PROGRAM
 *      9-12 The CRC of the data downloaded via the UPD_SEND_DATA commands. If the CRC does not match the
 *           programming returns an error
 *        13 Which boot block should be used
 *    UPD_REQ_DATA
 *      ???
 *    UPD_GET_LAST_ERROR
 *      Returns the reason why the last memory write PDU had a T_NACK_PDU
 *
 *    Workflow:
 *      - erase the sector which needs to be programmed (UPD_ERASE_SECTOR)
 *      - download the data via UPD_SEND_DATA telegrams
 *      - program the transmitted to into the FLASH  (UPD_PROGRAM)
 *      - repeat the above steps until the whole application has been downloaded
 *      - download the boot descriptor block via UPD_SEND_DATA telegrams
 *      - update the boot descriptor block so that the bootloader is able to start the new
 *        application (UPD_UPDATE_BOOT_DESC)
 *      - restart the board (UPD_RESTART)
 */
enum
{
    UPD_ERASE_PAGES = 0,
    UPD_SEND_DATA = 1,
    UPD_PROGRAM = 2,
    UPD_UPDATE_BOOT_DESC = 3,
    UPD_SEND_DATA_TO_DECOMPRESS = 4,
    UPD_PROGRAM_DECOMPRESSED_DATA = 5,
    UPD_REQ_DATA = 10,
    UPD_GET_LAST_ERROR = 20,
    UPD_SEND_LAST_ERROR = 21,
    UPD_UNLOCK_DEVICE = 30,
    UPD_REQUEST_UID = 31,
    UPD_RESPONSE_UID = 32,
    UPD_APP_VERSION_REQUEST = 33,
    UPD_APP_VERSION_RESPONSE = 34,
    UPD_RESET = 35,
	UPD_REQUEST_BOOT_DESC = 36,
	UPD_RESPONSE_BOOT_DESC = 37,
};

#define DEVICE_LOCKED   ((unsigned int ) 0x5AA55AA5)
#define DEVICE_UNLOCKED ((unsigned int ) ~DEVICE_LOCKED)
#define ADDRESS2SECTOR(a) ((a + 4095) / 4096)

enum UPD_Status
{
    UDP_UNKONW_COMMAND = 0x100       //<! received command is not defined
    ,
    UDP_CRC_ERROR                     //<! CRC calculated on the device
                                      //<! and by the updater don't match
    ,
    UPD_ADDRESS_NOT_ALLOWED_TO_FLASH //<! specifed address cannot be programmed
    ,
    UPD_SECTOR_NOT_ALLOWED_TO_ERASE  //<! the specified sector cannot be erased
    ,
    UPD_RAM_BUFFER_OVERFLOW          //<! internal buffer for storing the data
                                     //<! would overflow
    ,
    UPD_WRONG_DESCRIPTOR_BLOCK     //<! the boot descriptor block does not exist
    ,
    UPD_APPLICATION_NOT_STARTABLE //<! the programmed application is not startable
    ,
    UPD_DEVICE_LOCKED                //<! the device is still locked
    ,
    UPD_UID_MISMATCH               //<! UID sent to unlock the device is invalid
    ,
	UPD_ERASE_FAILED				// page erase failed
	,
	UPD_FLASH_ERROR					// page program (flash) failed
	,
    UDP_NOT_IMPLEMENTED = 0xFFFF    //<! this command is not yet implemented
};

unsigned char ramBuffer[4096];

Decompressor decompressor(FLASH_BASE + 0x8000);	// application base address

/*
 * a direct cast does not work due to possible miss aligned addresses.
 * therefore a good old conversion has to be performed
 */
unsigned int streamToUIn32(unsigned char * buffer)
{
    return buffer[3] << 24 | buffer[2] << 16 | buffer[1] << 8 | buffer[0];
}

void UIn32ToStream(unsigned char * buffer, unsigned int val)
{
    buffer[3] = val >> 24;
    buffer[2] = val >> 16;
    buffer[1] = val >> 8;
    buffer[0] = val & 0xff;
}

/* the following two symbols are used to protect the updater from
 * killing itself with a new application downloaded over the bus
 */
/* the vector table marks the beginning of the updater application */
extern const unsigned int __vectors_start__;
/* the _etext symbol marks the end of the used flash area */
extern const unsigned int _etext;

static bool _prepareReturnTelegram(unsigned int count, unsigned char cmd)
{
    bcu.sendTelegram[5] = 0x63 + count;
    bcu.sendTelegram[6] = 0x42;
    bcu.sendTelegram[7] = 0x40 | count;
    bcu.sendTelegram[8] = 0;
    bcu.sendTelegram[9] = cmd;
    return true;
}

/*
 * Checks if the requested sector is allowed to be erased.
 */
inline bool pageAllowedToErase(unsigned int sectorNumber)
{
    if (sectorNumber < 8)
        return false; // bootloader sector
    return true; // TODO
    //return !((sectorNumber >= ADDRESS2SECTOR(__vectors_start__))
    //        && (sectorNumber <= ADDRESS2SECTOR(_etext)));
}

/*
 * Checks if the address range is allowed to be programmed
 */
inline bool addressAllowedToProgram(unsigned int start, unsigned int length)
{
    unsigned int end = start + length;
    if (start & 0xff)
    {
        return 0;
    }
    return true; // TODO
    //return !((start >= __vectors_start__) && (end <= _etext));
}

unsigned char handleMemoryRequests(int apciCmd, bool * sendTel,
        unsigned char * data)
{
    unsigned int count = data[0] & 0x0f;
    unsigned int address;
    static unsigned int ramLocation;
    static unsigned int deviceLocked = DEVICE_LOCKED;
    unsigned int crc = 0xFFFFFFFF;
    static unsigned int lastError = 0;
    unsigned int sendLastError = 0;

    //digitalWrite(PIN_INFO, !digitalRead(PIN_INFO));
    // TODO
    switch (data[2])
    {
        case UPD_UNLOCK_DEVICE:
            if (!((BcuUpdate &) bcu).progPinStatus())
            { // the operator has physical access to the device -> we unlock it
                deviceLocked = DEVICE_UNLOCKED;
                lastError = IAP_SUCCESS;
            }
            else
            {   // we need to ensure that only authorized operators can
                // update the application
                // as a simple method we use the unique ID of the CPU itself
                // only if this UUID is known, the device will be unlocked
                byte uid[4 * 32];
                lastError = IAP_SUCCESS;
                if (IAP_SUCCESS == iapReadUID(uid))
                {
                    for (unsigned int i = 0; i < 12; i++)
                    {
                        if (data[i + 3] != uid[i])
                        {
                            lastError = UPD_UID_MISMATCH;
                        }
                    }
                }
                if (lastError != UPD_UID_MISMATCH)
                {
                    deviceLocked = DEVICE_UNLOCKED;
                    lastError = IAP_SUCCESS;
                }
            }
            sendLastError = true;
            ramLocation = 0;
            crc = 0xFFFFFFFF;
            break;
        case UPD_REQUEST_UID:
            if (!((BcuUpdate &) bcu).progPinStatus())
            { // the operator has physical access to the device -> we unlock it
                byte uid[4 * 4];
                lastError = iapReadUID(uid);
                if (lastError == IAP_SUCCESS)
                {
                    *sendTel = _prepareReturnTelegram(12, UPD_RESPONSE_UID);
                    memcpy(bcu.sendTelegram + 10, uid, 12);
                }
                break;
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            break;
        case UPD_RESET:
            if (deviceLocked == DEVICE_UNLOCKED)
            	NVIC_SystemReset();
            else
                lastError = UPD_DEVICE_LOCKED;
        	sendLastError = true;
            break;
        case UPD_APP_VERSION_REQUEST:
            unsigned char * appversion;
            appversion = getAppVersion(
                    (AppDescriptionBlock *) (FIRST_SECTOR
                            - (1 + data[3]) * BOOT_BLOCK_SIZE));
            if (((unsigned int) appversion) < 0x50000)
            {
                *sendTel = _prepareReturnTelegram(12, UPD_APP_VERSION_RESPONSE);
                memcpy(bcu.sendTelegram + 10, appversion, 12);
                lastError = IAP_SUCCESS;
            }
            else
                lastError = UPD_APPLICATION_NOT_STARTABLE;
            break;
        case UPD_ERASE_PAGES:
            if (deviceLocked == DEVICE_UNLOCKED)
            {
            	int pageNr = data[3];
            	int pages = data[4];
            	for (int i = pageNr; i < pageNr + pages; i++) {
            		if (!pageAllowedToErase(i))
            			lastError = UPD_SECTOR_NOT_ALLOWED_TO_ERASE;
            	}
            	if (lastError != UPD_SECTOR_NOT_ALLOWED_TO_ERASE) {
                	FLASH_EraseInitTypeDef  pEraseInit;
                	HAL_StatusTypeDef       status = HAL_OK;
                	uint32_t PageError = 0;

                	pEraseInit.NbPages = pages;
                	pEraseInit.Page = pageNr;
                	pEraseInit.TypeErase = FLASH_TYPEERASE_PAGES;

                	HAL_FLASH_Unlock();
                	status = HAL_FLASHEx_Erase(&pEraseInit, &PageError);
                	HAL_FLASH_Lock();

                    lastError = (status == HAL_OK) ? IAP_SUCCESS : UPD_ERASE_FAILED;
                }
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            ramLocation = 0;
            sendLastError = true;
            break;
        case UPD_SEND_DATA:
            if (deviceLocked == DEVICE_UNLOCKED)
            {
                if ((ramLocation + count) <= sizeof(ramBuffer))
                {
                    memcpy((void *) &ramBuffer[ramLocation], data + 3, count);
                    crc = crc32(crc, data + 3, count);
                    ramLocation += count;
                    lastError = IAP_SUCCESS;
                }
                else
                    lastError = UPD_RAM_BUFFER_OVERFLOW;
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            sendLastError = true;
            break;
        case UPD_SEND_DATA_TO_DECOMPRESS:
            if (deviceLocked == DEVICE_UNLOCKED)
            {
                if ((ramLocation + count) <= sizeof(ramBuffer))
                {
                	for (int i = 0; i < count; i++) {
                		decompressor.putByte(data[i + 3]);
                	};
                    lastError = IAP_SUCCESS;
                }
                else
                    lastError = UPD_RAM_BUFFER_OVERFLOW;
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            sendLastError = true;
            break;
        case UPD_PROGRAM:
            if (deviceLocked == DEVICE_UNLOCKED)
            {
                count = streamToUIn32(data + 3);
                address = streamToUIn32(data + 3 + 4);
                if (addressAllowedToProgram(address, count))
                {
                    crc = crc32(0xFFFFFFFF, ramBuffer, count);
                    if (crc == streamToUIn32(data + 3 + 4 + 4))
                    {
                        count = 2048;	// fixed page size 2K for STM32F303 or STM32G071
                        HAL_FLASH_Unlock();
                        for (unsigned int i = 0; i < count; i+=8) {	// 8 = doubleword = 2*32bit = 4*16bit
                        	uint64_t wordToWrite = *((uint64_t *) ramBuffer + i/8);
							if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address + i, wordToWrite) != HAL_OK) {
								lastError = UPD_FLASH_ERROR;
								break;
							}
                        }
                        HAL_FLASH_Lock();
                    }
                    else
                        lastError = UDP_CRC_ERROR;
                }
                else
                    lastError = UPD_ADDRESS_NOT_ALLOWED_TO_FLASH;
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            ramLocation = 0;
            crc = 0xFFFFFFFF;
            sendLastError = true;
            break;
        case UPD_PROGRAM_DECOMPRESSED_DATA:
            if (deviceLocked == DEVICE_UNLOCKED)
            {
                count = decompressor.getBytesCountToBeFlashed();
                address = decompressor.getStartAddrOfPageToBeFlashed();
                if (addressAllowedToProgram(address, count))
                {
                    crc = decompressor.getCrc32();
                    if (crc == streamToUIn32(data + 3 + 4 + 4))
                    {
                        if (!decompressor.pageCompletedDoFlash()) {
                        	lastError = UPD_FLASH_ERROR;
                        }
                    }
                    else
                        lastError = UDP_CRC_ERROR;
                }
                else
                    lastError = UPD_ADDRESS_NOT_ALLOWED_TO_FLASH;
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            ramLocation = 0;
            crc = 0xFFFFFFFF;
            sendLastError = true;
            break;
        case UPD_UPDATE_BOOT_DESC:
            if (deviceLocked == DEVICE_UNLOCKED)	// TODO this check is probably wrong in combination with updater v0.2
            {
            	count = streamToUIn32(data + 3);
                crc = crc32(0xFFFFFFFF, ramBuffer, count);
                //address = FIRST_SECTOR - (1 + data[7]) * BOOT_BLOCK_SIZE; // start address of the descriptor block
                address = FIRST_SECTOR - BOOT_BLOCK_SIZE;	// one 2kB page before first available application address
                if (crc == streamToUIn32(data + 3 + 4))
                {
                    if (checkApplication((AppDescriptionBlock *) ramBuffer))
                    {
                    	FLASH_EraseInitTypeDef  pEraseInit;
						HAL_StatusTypeDef       status = HAL_OK;
						uint32_t PageError = 0;

						pEraseInit.NbPages = 1;
						pEraseInit.Page = address/FLASH_PAGE_SIZE;
						pEraseInit.TypeErase = FLASH_TYPEERASE_PAGES;

						HAL_FLASH_Unlock();

						status = HAL_FLASHEx_Erase(&pEraseInit, &PageError);

						if (status == HAL_OK) {
							for (int i = 0; i < 256; i+=8) {	// 8 = doubleword = 2*32bit = 4*16bit
	                        	uint64_t wordToWrite = *((uint64_t *) ramBuffer + i/8);
								if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address + i, wordToWrite) != HAL_OK) {
									lastError = UPD_FLASH_ERROR;
									break;
								}
							}
						} else {
							lastError = UPD_ERASE_FAILED;
						}
						HAL_FLASH_Lock();
                    }
                    else
                        lastError = UPD_APPLICATION_NOT_STARTABLE;
                }
                else
                    lastError = UDP_CRC_ERROR;
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            ramLocation = 0;
            crc = 0xFFFFFFFF;
            sendLastError = true;
            break;
        case UPD_REQUEST_BOOT_DESC:
        	if (deviceLocked == DEVICE_UNLOCKED) {
        		address = (FIRST_SECTOR - (1 + data[3]) * BOOT_BLOCK_SIZE);
        		AppDescriptionBlock* bootDescr = (AppDescriptionBlock *) address;
				if (lastError == IAP_SUCCESS)
				{
					*sendTel = _prepareReturnTelegram(12, UPD_RESPONSE_BOOT_DESC);
					memcpy(bcu.sendTelegram + 10, &(bootDescr->startAddress), 4);
					memcpy(bcu.sendTelegram + 14, &(bootDescr->endAddress), 4);
					memcpy(bcu.sendTelegram + 18, &(bootDescr->crc), 4);
				}
				break;
			}
			else
				lastError = UPD_DEVICE_LOCKED;
			break;
        case UPD_REQ_DATA:
            if (deviceLocked == DEVICE_UNLOCKED)
            {
                /*
                 memcpy(bcu.sendTelegram + 9, (void *)address, count);
                 bcu.sendTelegram[5] = 0x63 + count;
                 bcu.sendTelegram[6] = 0x42;
                 bcu.sendTelegram[7] = 0x40 | count;
                 bcu.sendTelegram[8] = UPD_SEND_DATA;
                 * sendTel = true;
                 * */
                lastError = UDP_NOT_IMPLEMENTED; // set to any error
            }
            else
                lastError = UPD_DEVICE_LOCKED;
            sendLastError = true;
            break;
        case UPD_GET_LAST_ERROR:
        	sendLastError = true;
            break;
        default:
            lastError = UDP_UNKONW_COMMAND; // set to any error
        	sendLastError = true;
    }
    if (sendLastError)
    {
        *sendTel = _prepareReturnTelegram(4, UPD_SEND_LAST_ERROR);
        UIn32ToStream(bcu.sendTelegram + 10, lastError);
    }
    return T_ACK_PDU;
}

#ifndef ADSDEVICE_H
#define ADSDEVICE_H

#include "AmsRouter.h"

static constexpr uint16_t AMSPORT_R3_SYSSERV = 10000;
static constexpr uint32_t EC_ADS_IGRP_CANOPEN_ACCESS = 0x0000F302;

#pragma pack(push, 1)
struct WriteControlHeader {
	uint16_t adsState;
	uint16_t devState;
	uint32_t length;
};

struct CoEReadHeader {
	uint8_t subIndex;
	uint8_t reserved[3];
};

struct CoEWriteHeader {
	uint8_t subIndex;
	uint8_t reserved[3];
};

struct AdsSlaveIdentity {
	uint32_t vendorId;
	uint32_t productCode;
	uint32_t revisionNumber;
	uint32_t serialNumber;
};
#pragma pack(pop)

struct SlaveStatus {
	uint8_t state;
	uint8_t linkStatus;
};

struct IOAddress {
	uint32_t iGroup;
	uint32_t iOffs;
	uint32_t size;
};

class Adsdevice
{
    public:
	Adsdevice(const AmsNetId& localNetId);
	~Adsdevice();

	long open(const std::string& ip, const AmsNetId& netId);
	void close();
	long readStatus(uint16_t * as, uint16_t * ds);
	long sendSystemRestart();
	long readByName(uint16_t port, const std::string& name, char * buff, uint32_t size);
	long readFlags(uint16_t port, uint32_t offset, char * buff, uint32_t size);
	long readECBusData(uint16_t port, uint32_t indexGroup, uint32_t indexOffset, char * buff, uint32_t size);
	long writeFlags(uint16_t port, uint32_t offset, const char * buff, uint32_t size);
	long readFile(const std::string& fileName, char * buff, uint32_t size, uint32_t * cbReturn);
	long writeFile(const std::string& fileName, const char * buff, uint32_t size);
	long deleteFile(const std::string& fileName);
	long setTimeout(uint16_t value);
	void setIoSizes(uint32_t inSize, uint32_t outSize) { m_inputsSize = inSize; m_outputsSize = outSize; }

	template<typename T>
	long readValue(uint16_t port, uint32_t offset, T& value) {
		return readFlags(port, offset, reinterpret_cast<char*>(&value), sizeof(T));
	}

	template<typename T>
	long writeValue(uint16_t port, uint32_t offset, const T& value) {
		return writeFlags(port, offset, reinterpret_cast<const char*>(&value), sizeof(T));
	}

	long readImageSize(uint16_t port, uint32_t indexGroup, uint32_t* outSize);
	long readImageBuffer(uint16_t port, uint32_t indexGroup, char* buff, uint32_t size, uint32_t* bytesRead);

	long readCoE(uint16_t slaveAddress, uint16_t canopenIndex, uint8_t subIndex, char* buff, uint32_t size);
	long writeCoE(uint16_t slaveAddress, uint16_t canopenIndex, uint8_t subIndex, const char* buff, uint32_t size);

    private:
	AmsRouter router;
	uint16_t m_port = 0;
	AmsAddr m_addr;
	bool m_connected = false;
	uint16_t m_slaveCount = 0;
	uint32_t m_inputsSize = 0;
	uint32_t m_outputsSize = 0;

	long setVarHandler(const AmsAddr& addr, const std::string &name, uint32_t * handle);
	long readByHandle(const AmsAddr& addr, uint32_t handle, char * buff, uint32_t size);
	long resetVarHandler(const AmsAddr& addr, uint32_t handle);
	long openFile(const std::string &name, uint32_t flags, uint32_t * handle);
	long closeFile(uint32_t handle);
};

#endif // ADSDEVICE_H

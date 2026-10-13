#include "plclink.h"
#include "AdsFile.h"
#include "AmsRouter.h"

#define PLC_FLAG_AREA 0x4020

PlcLink::PlcLink(const AmsNetId& netId) :
	router(netId)
{
	m_port = router.OpenPort();
}

PlcLink::~PlcLink()
{
	close();
	router.ClosePort(m_port);
}

long PlcLink::open(const std::string& ip, const AmsNetId& netId)
{
	// Если уже было открыто соединение — корректно закрываем старые маршруты
	if (m_connected) {
		close();
	}

	// Сохраняем целевой NetId в структуру адреса устройства
	m_addr.netId = netId;

	// 1. Добавляем основной маршрут для TwinCAT PLC (например, 192.168.1.10.1.1)
	long nErr = router.AddRoute(m_addr.netId, ip);
	if (nErr) {
		return nErr;
	}

	// 2. Формируем NetId для EtherCAT Мастера (меняем 5-й байт на 2)
	AmsNetId masterNetId = m_addr.netId;
	masterNetId.b[4] = 2;

	// Добавляем маршрут для EtherCAT Мастера (в обход среды исполнения PLC)
	nErr = router.AddRoute(masterNetId, ip);
	if (nErr) {
		// Если второй маршрут не добавился — откатываем первый, чтобы не плодить мусор
		router.DelRoute(m_addr.netId);
		return nErr;
	}

	m_connected = true;
	return 0;
}

void PlcLink::close()
{
	if (m_connected || m_addr.netId.b[0] != 0) {
		router.DelRoute(m_addr.netId);
		AmsNetId masterNetId = m_addr.netId;
		masterNetId.b[4] = 2;
		router.DelRoute(masterNetId);
		m_connected = false;
	}
}

long PlcLink::readStatus(uint16_t *as, uint16_t *ds)
{
	if(m_connected)
	{
		AmsAddr localAddr = m_addr;
		localAddr.port = 801;

		int16_t buffer[2];
		AmsRequest request{ localAddr, m_port, AoEHeader::READ_STATE,
				    sizeof(buffer), buffer };
		const auto status = router.AdsRequest(request);
		if (!status) {
			*as = bhf::ads::letoh(buffer[0]);
			*ds = bhf::ads::letoh(buffer[1]);
		}
		return status;
	}
	return -1;
}

long PlcLink::sendSystemRestart()
{
	if (!m_connected) return -1;
	AmsAddr localAddr = m_addr;
	localAddr.port = 10000;
	AmsRequest req{ localAddr, m_port, AoEHeader::WRITE_CONTROL, 0, nullptr };
	req.frame.prepend(WriteControlHeader{ 2, 0, 0 });
	return router.AdsRequest(req);
}

long PlcLink::readByName(uint16_t port, const std::string& name, char* buff, uint32_t size)
{
	if (!m_connected) return -1;

	AmsAddr localAddr = m_addr;
	localAddr.port = port; // Изменяем порт только для локальной копии!

	uint32_t handle = 0;
	long nErr = setVarHandler(localAddr, name, &handle);
	if (nErr) return nErr;

	nErr = readByHandle(localAddr, handle, buff, size);
	resetVarHandler(localAddr, handle);
	return nErr;
}

long PlcLink::readFlags(uint16_t port, uint32_t offset, char *buff, uint32_t size)
{
	if(m_connected)
	{
		uint32_t bytesRead;
		AmsAddr localAddr = m_addr;
		localAddr.port = port;
		AmsRequest request{ localAddr, m_port, AoEHeader::READ,
				    size, reinterpret_cast<void*>(buff), &bytesRead, sizeof(AoERequestHeader) };
		request.frame.prepend(AoERequestHeader{ PLC_FLAG_AREA, offset, size });
		return router.AdsRequest(request);
	}
	return -1;
}

long PlcLink::writeFlags(uint16_t port, uint32_t offset, const char *buff, uint32_t size)
{
	if(m_connected)
	{
		AmsAddr localAddr = m_addr;
		localAddr.port = port;
		AmsRequest request{ localAddr, m_port, AoEHeader::WRITE,
				    0, nullptr, nullptr, sizeof(AoERequestHeader) + size};
		request.frame.prepend(buff, size);
		request.frame.prepend<AoERequestHeader>({ PLC_FLAG_AREA, offset, size });
		return router.AdsRequest(request);
	}
	return -1;
}

long PlcLink::readFile(const std::string& filename, char *buff, uint32_t size, uint32_t * cbReturn)
{
	if (m_connected)
	{
		uint32_t handle = 0;
		long nErr = 0;

		m_addr.port = AMSPORT_R3_SYSSERV;

		// Передаем std::string во внутренний метод openFile
		nErr = openFile(filename, (bhf::ads::FOPEN::READ | bhf::ads::FOPEN::BINARY |
					   bhf::ads::FOPEN::ENSURE_DIR), &handle);
		if (nErr) return nErr;

		AmsRequest request{ m_addr, m_port, AoEHeader::READ_WRITE,
				    size, buff, cbReturn, sizeof(AoEReadWriteReqHeader) };
		request.frame.prepend(AoEReadWriteReqHeader{ SYSTEMSERVICE_FREAD, handle, size, 0 });

		nErr = router.AdsRequest(request);
		closeFile(handle);
		return nErr;
	}
	return -1;
}

// --- ИСПРАВЛЕНО: Чистый C++ вариант для записи файлов ---
long PlcLink::writeFile(const std::string& filename, const char *buff, uint32_t size)
{
	if(m_connected)
	{
		uint32_t handle = 0;
		uint32_t bytesRead = 0;
		long nErr = 0;

		m_addr.port = AMSPORT_R3_SYSSERV;

		// Внутренний метод openFile уже принимает std::string
		nErr = openFile(filename, (bhf::ads::FOPEN::BINARY | bhf::ads::FOPEN::WRITE |
					   bhf::ads::FOPEN::OVERWRITE | bhf::ads::FOPEN::ENSURE_DIR), &handle);
		if (nErr) return nErr;

		AmsRequest request{ m_addr, m_port, AoEHeader::READ_WRITE,
				    0, nullptr, &bytesRead, sizeof(AoEReadWriteReqHeader) + size };
		request.frame.prepend(buff, size);
		request.frame.prepend(AoEReadWriteReqHeader{ SYSTEMSERVICE_FWRITE, handle, 0, size });

		nErr = router.AdsRequest(request);
		closeFile(handle);
		return nErr;
	}
	return -1;
}

// --- ИСПРАВЛЕНО: Чистый C++ вариант для удаления файлов ---
long PlcLink::deleteFile(const std::string& fileName)
{
	if(m_connected)
	{
		// Переменная fileName теперь изначально std::string, конвертация больше не нужна
		uint32_t nameByteSize = static_cast<uint32_t>(fileName.size());
		uint32_t bytesRead = 0;

		AmsAddr localAddr = m_addr;
		localAddr.port = AMSPORT_R3_SYSSERV;

		AmsRequest request{ localAddr, m_port, AoEHeader::READ_WRITE,
				    0, nullptr, &bytesRead, sizeof(AoEReadWriteReqHeader) + nameByteSize};

		request.frame.prepend(fileName.data(), nameByteSize);
		request.frame.prepend(AoEReadWriteReqHeader{ SYSTEMSERVICE_FDELETE, 0, 0, nameByteSize });
		return router.AdsRequest(request);
	}
	return -1;
}

long PlcLink::setTimeout(uint16_t value)
{
	return router.SetTimeout(m_port, value);
}

long PlcLink::readImageSize(uint16_t port, uint32_t indexGroup, uint32_t* outSize)
{
	if (!m_connected || !outSize) return -1;

	AmsAddr ecAddr = m_addr;
	ecAddr.netId.b[4] = 1; // EtherCAT образ
	ecAddr.port = port;

	uint32_t bytesRead = 0;
	AmsRequest request{ ecAddr, m_port, AoEHeader::READ,
			    sizeof(uint32_t), outSize, &bytesRead, sizeof(AoERequestHeader) };
	request.frame.prepend(AoERequestHeader{ indexGroup, 0, sizeof(uint32_t) });

	return router.AdsRequest(request);
}

long PlcLink::readImageBuffer(uint16_t port, uint32_t indexGroup, char* buff, uint32_t size, uint32_t* bytesRead)
{
	if (!m_connected || !buff || !bytesRead) return -1;

	AmsAddr ecAddr = m_addr;
	ecAddr.netId.b[4] = 1; // EtherCAT образ
	ecAddr.port = port;
	AmsRequest request{ ecAddr, m_port, AoEHeader::READ,
			    size, reinterpret_cast<void*>(buff), bytesRead, sizeof(AoERequestHeader) };
	request.frame.prepend(AoERequestHeader{ indexGroup, 0, size });

	return router.AdsRequest(request);
}

long PlcLink::setVarHandler(const AmsAddr& addr, const std::string& name, uint32_t* handle)
{
	uint32_t nameByteSize = static_cast<uint32_t>(name.size());
	uint32_t bytesRead = 0;

	AmsRequest request{ addr, m_port, AoEHeader::READ_WRITE,
			    4, handle, &bytesRead, sizeof(AoEReadWriteReqHeader) + nameByteSize };

	request.frame.prepend(name.data(), nameByteSize);
	request.frame.prepend(AoEReadWriteReqHeader{ ADSIGRP_SYM_HNDBYNAME, 0, 4, nameByteSize });
	return router.AdsRequest(request);
}

long PlcLink::resetVarHandler(const AmsAddr &addr, uint32_t handle)
{
	AmsRequest request{ addr, m_port, AoEHeader::WRITE,
			    0, nullptr, nullptr, sizeof(AoERequestHeader) + sizeof(handle) };
	request.frame.prepend(&handle, sizeof(handle));
	request.frame.prepend<AoERequestHeader>({ (uint32_t)ADSIGRP_SYM_RELEASEHND, 0, sizeof(handle)});
	return router.AdsRequest(request);
}

long PlcLink::openFile(const std::string &name, uint32_t flags, uint32_t * handle)
{
	uint32_t nameByteSize = static_cast<uint32_t>(name.size());
	uint32_t bytesRead = 0;
	AmsRequest request{ m_addr, m_port, AoEHeader::READ_WRITE,
			    4, handle, &bytesRead, sizeof(AoEReadWriteReqHeader) + nameByteSize };
	request.frame.prepend(name.data(), nameByteSize);
	request.frame.prepend(AoEReadWriteReqHeader{SYSTEMSERVICE_FOPEN, flags, 4, nameByteSize });
	return router.AdsRequest(request);
}

long PlcLink::closeFile(uint32_t handle)
{
	uint32_t bytesRead;
	AmsRequest request{ m_addr, m_port, AoEHeader::READ_WRITE,
			    0, nullptr, &bytesRead,  sizeof(AoEReadWriteReqHeader)};
	request.frame.prepend(AoEReadWriteReqHeader{SYSTEMSERVICE_FCLOSE, handle, 0, 0 });
	return router.AdsRequest(request);
}

long PlcLink::readByHandle(const AmsAddr &addr, uint32_t handle, char *buff, uint32_t size)
{
	uint32_t bytesRead;
	AmsRequest request{ addr, m_port, AoEHeader::READ,
			    size, reinterpret_cast<void*>(buff), &bytesRead, sizeof(AoERequestHeader) };
	request.frame.prepend(AoERequestHeader{ ADSIGRP_SYM_VALBYHND, handle, size });
	return router.AdsRequest(request);
}

long PlcLink::readECBusData(uint16_t port, uint32_t indexGroup,
			      uint32_t indexOffset, char * buff, uint32_t size)
{
	if (m_connected)
	{
		uint32_t bytesRead = 0;
		AmsAddr localAddr = m_addr;
		localAddr.netId.b[4] = 2;
		localAddr.port = port;
		AmsRequest request{ localAddr, m_port, AoEHeader::READ,
				    size, reinterpret_cast<void*>(buff), &bytesRead, sizeof(AoERequestHeader) };
		request.frame.prepend(AoERequestHeader{ indexGroup, indexOffset, size });
		long nErr = router.AdsRequest(request);
		return nErr;
	}
	return -1;
}

long PlcLink::readCoE(uint16_t slaveAddress, uint16_t canopenIndex, uint8_t subIndex, char* buff, uint32_t size)
{
	if (!m_connected) return -1;
	uint32_t bytesRead = 0;
	AmsAddr targetAddr = m_addr;
	targetAddr.netId.b[4] = 2;  // Направляем запрос к EtherCAT Мастеру
	targetAddr.port = 0xFFFF;   // Системный порт мастера
	uint32_t indexOffset = (static_cast<uint32_t>(slaveAddress) << 16) | canopenIndex;
	CoEReadHeader coeHeader;
	coeHeader.subIndex = subIndex;
	coeHeader.reserved[0] = 0;
	coeHeader.reserved[1] = 0;
	coeHeader.reserved[2] = 0;
	AmsRequest request{ targetAddr, m_port, AoEHeader::READ_WRITE,
			    size, reinterpret_cast<void*>(buff), &bytesRead, sizeof(AoEReadWriteReqHeader) + sizeof(coeHeader) };
	request.frame.prepend(reinterpret_cast<char*>(&coeHeader), sizeof(coeHeader));
	request.frame.prepend(AoEReadWriteReqHeader{ EC_ADS_IGRP_CANOPEN_ACCESS, indexOffset, size, (uint32_t)sizeof(coeHeader) });
	return router.AdsRequest(request);
}

long PlcLink::writeCoE(uint16_t slaveAddress, uint16_t canopenIndex, uint8_t subIndex, const char* buff, uint32_t size)
{
	if (!m_connected) return -1;

	AmsAddr targetAddr = m_addr;
	targetAddr.netId.b[4] = 2;  // Направляем запрос к EtherCAT Мастеру
	targetAddr.port = 0xFFFF;   // Системный порт мастера
	uint32_t indexOffset = (static_cast<uint32_t>(slaveAddress) << 16) | canopenIndex;
	CoEWriteHeader coeHeader;
	coeHeader.subIndex = subIndex;
	coeHeader.reserved[0] = 0;
	coeHeader.reserved[1] = 0;
	coeHeader.reserved[2] = 0;
	uint32_t totalPayloadSize = sizeof(coeHeader) + size;
	AmsRequest request{ targetAddr, m_port, AoEHeader::WRITE,
			    0, nullptr, nullptr, (uint32_t)(sizeof(AoERequestHeader) + totalPayloadSize) };
	request.frame.prepend(const_cast<char*>(buff), size);
	request.frame.prepend(reinterpret_cast<char*>(&coeHeader), sizeof(coeHeader));
	request.frame.prepend<AoERequestHeader>({ EC_ADS_IGRP_CANOPEN_ACCESS, indexOffset, totalPayloadSize });
	return router.AdsRequest(request);
}
// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2015 - 2022 Beckhoff Automation GmbH & Co. KG
 */

#include "Sockets.h"
#include "Log.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <errno.h>
#endif

std::atomic<uint32_t> g_connectionTimeoutMs{1000};

namespace bhf
{
namespace ads
{
/**
 * Splits the provided host string into host and port. If no port was found
 * in the host string, port is returned untouched acting as a default value.
 */
static void ParseHostAndPort(std::string &host, std::string &port)
{
	if (host.empty()) {
		return;
	}
	auto split = host.find_last_of(":");

	if (host.find_first_of(":") != split) {
		// more than one colon -> IPv6
		const auto closingBracket = host.find_last_of("]");
		if (closingBracket > split) {
			// IPv6 without port
			split = host.npos;
		}
	}
	if (split != host.npos) {
		port = host.substr(split + 1);
		host.resize(split);
	}
	// remove brackets
	if (*host.crbegin() == ']') {
		host.pop_back();
	}
	if (*host.begin() == '[') {
		host.erase(host.begin());
	}
}

AddressList GetListOfAddresses(const std::string &hostPort,
			       const std::string &defaultPort)
{
	auto host = std::string(hostPort);
	auto service = std::string(defaultPort);
	ParseHostAndPort(host, service);

	InitSocketLibrary();
	struct addrinfo *results;
	if (getaddrinfo(host.c_str(), service.c_str(), nullptr, &results)) {
		throw std::runtime_error("Invalid or unknown host: " + host);
	}
	return AddressList{ results,
			    [](struct addrinfo *p) { freeaddrinfo(p); } };
}
}
}

static const struct addrinfo addrinfo = []() {
	struct addrinfo a;
	memset(&a, 0, sizeof(a));
	a.ai_family = AF_INET;
	a.ai_socktype = SOCK_STREAM;
	a.ai_protocol = IPPROTO_TCP;
	return a;
}();

uint32_t getIpv4(const std::string &addr)
{
	struct addrinfo *res;

	InitSocketLibrary();
	const auto status = getaddrinfo(addr.c_str(), nullptr, &addrinfo, &res);
	if (status) {
		throw std::runtime_error(
			"Invalid IPv4 address or unknown hostname: " + addr);
	}

	const auto value =
		((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(res);
	WSACleanup();
	return ntohl(value);
}

IpV4::IpV4(const std::string &addr)
	: value(getIpv4(addr))
{
}

IpV4::IpV4(uint32_t __val)
	: value(__val)
{
}

bool IpV4::operator<(const IpV4 &ref) const
{
	return value < ref.value;
}

bool IpV4::operator==(const IpV4 &ref) const
{
	return value == ref.value;
}

Socket::Socket(const struct addrinfo *const host, const int type)
	: m_WSAInitialized(!InitSocketLibrary())
	, m_DestAddr(SOCK_DGRAM == type ?
			     reinterpret_cast<const struct sockaddr *>(
				     &m_SockAddress) :
			     nullptr)
	, m_DestAddrLen(0)
{
	for (auto rp = host; rp; rp = rp->ai_next) {
		m_Socket = socket(rp->ai_family, type, 0);
		if (INVALID_SOCKET == m_Socket) {
			continue;
		}

		if (SOCK_STREAM == type) {
// --- ИСПРАВЛЕНО: НЕБЛОКИРУЮЩИЙ CONNECT ДЛЯ WINDOWS С ДИНАМИЧЕСКИМ ТАЙМАУТОМ ---
#if defined(_WIN32)
			// 1. Временно переводим сокет в НЕБЛОКИРУЮЩИЙ режим
			unsigned long iMode = 1;
			ioctlsocket(m_Socket, FIONBIO, &iMode);

			// 2. Асинхронно запускаем попытку подключения
			int connectResult = ::connect(m_Socket, rp->ai_addr, rp->ai_addrlen);

			// Если соединение не установилось мгновенно
			if (connectResult == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
				fd_set writeSet;
				FD_ZERO(&writeSet);
				FD_SET(m_Socket, &writeSet);

				// Загружаем актуальное значение, выставленное юзером в GUI
				uint32_t userTimeout = g_connectionTimeoutMs.load();

				timeval tv;
				tv.tv_sec = userTimeout / 1000;          // Целые секунды (например, 1)
				tv.tv_usec = (userTimeout % 1000) * 1000; // Оставшиеся микросекунды

				// Контролируем время ожидания средствами быстрого select
				int selectResult = select(0, nullptr, &writeSet, nullptr, &tv);

				if (selectResult <= 0) {
					// Время вышло — ПЛК выключен!
					closesocket(m_Socket);
					m_Socket = INVALID_SOCKET;
					continue; // Переходим к следующему rp в цикле, не тратя 21 секунду!
				}
			}

			// 3. Возвращаем сокет обратно в БЛОКИРУЮЩИЙ режим для стабильной работы AdsLib
			iMode = 0;
			ioctlsocket(m_Socket, FIONBIO, &iMode);
#else
			int flags = fcntl(m_Socket, F_GETFL, 0);
			fcntl(m_Socket, F_SETFL, flags | O_NONBLOCK);

			int connectResult = ::connect(m_Socket, rp->ai_addr, rp->ai_addrlen);
			if (connectResult < 0 && errno == EINPROGRESS) {
				fd_set writeSet;
				FD_ZERO(&writeSet);
				FD_SET(m_Socket, &writeSet);

				uint32_t userTimeout = g_connectionTimeoutMs.load();
				timeval tv;
				tv.tv_sec = userTimeout / 1000;
				tv.tv_usec = (userTimeout % 1000) * 1000;

				int selectResult = select(m_Socket + 1, nullptr, &writeSet, nullptr, &tv);
				if (selectResult <= 0) {
					// Время вышло (ПЛК оффлайн) — закрываем сразу, не вися 2 минуты!
					::close(m_Socket);
					m_Socket = INVALID_SOCKET;
					continue;
				}

				int so_error = 0;
				socklen_t len = sizeof(so_error);
				getsockopt(m_Socket, SOL_SOCKET, SO_ERROR, &so_error, &len);
				if (so_error != 0) {
					::close(m_Socket);
					m_Socket = INVALID_SOCKET;
					continue;
				}
			} else if (connectResult < 0) {
				::close(m_Socket);
				m_Socket = INVALID_SOCKET;
				continue;
			}

			// Возвращаем сокет в блокирующий режим для стабильной работы AdsLib
			fcntl(m_Socket, F_SETFL, flags);
#endif
			// ------------------------------------------------------------------------

		}
		memcpy(&m_SockAddress, rp->ai_addr,
		       std::min<size_t>(sizeof(m_SockAddress), rp->ai_addrlen));
		return;
	}
	LOG_ERROR("Unable to create socket");
	throw std::system_error(WSAGetLastError(), std::system_category());
}


Socket::~Socket()
{
	Shutdown();
	closesocket(m_Socket);

	if (m_WSAInitialized) {
		WSACleanup();
	}
}

void Socket::Shutdown()
{
	shutdown(m_Socket, SHUT_RDWR);
}

size_t Socket::read(uint8_t *buffer, size_t maxBytes, timeval *timeout) const
{
	if (!Select(timeout)) {
		return 0;
	}

	const auto msvcMaxBytes = static_cast<int>(
		std::min<size_t>((std::numeric_limits<int>::max)(), maxBytes));
	const int bytesRead = recv(m_Socket, reinterpret_cast<char *>(buffer),
				   msvcMaxBytes, 0);
	if (bytesRead > 0) {
		return bytesRead;
	}
	const auto lastError = WSAGetLastError();
	if ((0 == bytesRead) || (lastError == CONNECTION_CLOSED) ||
	    (lastError == CONNECTION_ABORTED)) {
		throw std::runtime_error("connection closed by remote");
	} else {
		LOG_ERROR("read frame failed with error: "
			  << std::dec << std::strerror(lastError));
	}
	return 0;
}

Frame &Socket::read(Frame &frame, timeval *timeout) const
{
	const size_t bytesRead =
		read(frame.rawData(), frame.capacity(), timeout);
	if (bytesRead) {
		return frame.limit(bytesRead);
	}
	return frame.clear();
}

bool Socket::Select(timeval *timeout) const
{
	/* prepare socket set for select() */
	fd_set readSockets;
	FD_ZERO(&readSockets);
	FD_SET(m_Socket, &readSockets);

	// --- ИСПРАВЛЕНО: ДИНАМИЧЕСКИЙ ТАЙМАУТ ПОЛЬЗОВАТЕЛЯ ИЗ QT ---
	timeval customTimeout;

	// Если библиотека передала nullptr (вечное чтение) или оригинальный таймаут слишком большой,
	// мы принудительно берем актуальное значение g_connectionTimeoutMs, установленное юзером!
	if (!timeout) {
		uint32_t userTimeout = g_connectionTimeoutMs.load();
		customTimeout.tv_sec = userTimeout / 1000;
		customTimeout.tv_usec = (userTimeout % 1000) * 1000;
	} else {
		customTimeout = *timeout;
		uint32_t userTimeout = g_connectionTimeoutMs.load();
		uint32_t currentTimeoutMs = (customTimeout.tv_sec * 1000) + (customTimeout.tv_usec / 1000);

		// Если системный таймаут больше пользовательского — урезаем его до рамок юзера
		if (currentTimeoutMs > userTimeout) {
			customTimeout.tv_sec = userTimeout / 1000;
			customTimeout.tv_usec = (userTimeout % 1000) * 1000;
		}
	}
	// -------------------------------------------------------------

	/* wait for receive data */
	// Передаем customTimeout вместо оригинального timeout во всех сценариях
	const int state = NATIVE_SELECT(m_Socket + 1, &readSockets, nullptr,
					nullptr, &customTimeout);

	if (0 == state) {
		// Если оригинальный таймаут был nullptr, значит это фоновый поток ожидания AdsLib.
		// Мы возвращаем false без исключения, чтобы не ломать поток, а дать ему сделать пустой круг.
		if (!timeout) {
			return false;
		}
		LOG_ERROR("select() timeout");
		throw TimeoutEx("select() timeout");
	}

	const auto lastError = WSAGetLastError();
	if (lastError == WSAENOTSOCK) {
		throw std::runtime_error("connection closed");
	}

	/* and check if socket was correct */
	if ((1 != state) || (!FD_ISSET(m_Socket, &readSockets))) {
		LOG_ERROR(
			"something strange happen while waiting for socket in state: "
			<< state
			<< " with error: " << std::strerror(lastError));
		return false;
	}
	return true;
}



size_t Socket::write(const Frame &frame) const
{
	if (frame.size() > (std::numeric_limits<int>::max)()) {
		LOG_ERROR("frame length: "
			  << frame.size()
			  << " exceeds maximum length for sockets");
		return 0;
	}

	const int bufferLength = static_cast<int>(frame.size());
	const char *const buffer = reinterpret_cast<const char *>(frame.data());
	const int status = sendto(m_Socket, buffer, bufferLength, 0, m_DestAddr,
				  m_DestAddrLen);

	if (SOCKET_ERROR == status) {
		LOG_ERROR("write frame failed with error: "
			  << std::strerror(WSAGetLastError()));
		return 0;
	}
	return status;
}

TcpSocket::TcpSocket(const struct addrinfo *const host)
	: Socket(host, SOCK_STREAM)
{
	// AdsDll.lib seems to use TCP_NODELAY, we use it to be compatible
	const int enable = 0;
	if (setsockopt(m_Socket, IPPROTO_TCP, TCP_NODELAY,
		       (const char *)&enable, sizeof(enable))) {
		LOG_WARN("Enabling TCP_NODELAY failed");
	}
}

uint32_t TcpSocket::Connect() const
{
	struct sockaddr_storage source;
	socklen_t len = sizeof(source);

	if (getsockname(m_Socket, reinterpret_cast<sockaddr *>(&source),
			&len)) {
		LOG_ERROR("Read local tcp/ip address failed");
		throw std::runtime_error("Read local tcp/ip address failed");
	}

	switch (source.ss_family) {
	case AF_INET:
		return ntohl(reinterpret_cast<sockaddr_in *>(&source)
				     ->sin_addr.s_addr);

	case AF_INET6:
		return 0xffffffff;

	default:
		return 0;
	}
}

bool TcpSocket::IsConnectedTo(const struct addrinfo *const targetAddresses) const
{
	for (auto rp = targetAddresses; rp; rp = rp->ai_next) {
		if (m_SockAddress.ss_family == rp->ai_family) {
			if (!memcmp(&m_SockAddress, rp->ai_addr,
				    std::min<size_t>(sizeof(m_SockAddress),
						     rp->ai_addrlen))) {
				return true;
			}
		}
	}
	return false;
}

UdpSocket::UdpSocket(const struct addrinfo *const host)
	: Socket(host, SOCK_DGRAM)
{
}

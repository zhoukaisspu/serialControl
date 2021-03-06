#include "SerialAdapter.h"
#include <algorithm>
#include <cctype>
#include <atlstr.h>
#include <iostream>
#include <stdexcept>
#include <functional>
#include "scl/serial.h"
using std::mutex;
using std::weak_ptr;
using std::set;

namespace scl
{

	serialAdapter::~serialAdapter()
	{
		quitReadThread();
		quitWriteThread();
		quitDetectThread();
	}
	RET_CODE serialAdapter::registerListener(weak_ptr<SerialListener> ptr)
	{
		auto sp = ptr.lock();
		if (nullptr == sp)
			return BAD_PARAMETER;
		std::lock_guard<mutex> mtx(mListenerMtx);
		auto ret = mListener.insert(ptr);
		if (ret.second)
			return SUCCESS;
		else
			return FAILURE;
	}

	RET_CODE serialAdapter::unRegisterListener(weak_ptr<SerialListener> ptr)
	{
		auto sp = ptr.lock();
		if (nullptr == sp)
			return BAD_PARAMETER;
		std::lock_guard<mutex> mtx(mListenerMtx);
		auto eraseSize = mListener.erase(ptr);
		if (eraseSize)
			return SUCCESS;
		else
			return FAILURE;
	}
	inline RET_CODE	serialAdapter::quitReadThread(void)
	{
		mReadThreadQuit = true;
		if (mReadThread.joinable()) {
			mReadThread.join();
		}
		mReadThreadQuit = false;
		return SUCCESS;
	}
	inline RET_CODE serialAdapter::quitDetectThread(void)
	{
		mDetectThreadQuit = true;
		if (mDetectThread.joinable()) {
			mDetectThread.join();
		}
		mDetectThreadQuit = false;
		return SUCCESS;
	}
	inline RET_CODE serialAdapter::quitWriteThread(void)
	{
		mWriteThreadQuit = true;
		if (mWriteThread.joinable()) {
			mQueue.push(nullptr);
			mWriteThread.join();
		}
		mWriteThreadQuit = false;
		return SUCCESS;
	}
	RET_CODE serialAdapter::open(unsigned int port,
		unsigned long nBaud,
		serialAdapter::Parity nParity,
		serialAdapter::DataBits nByteSize,
		serialAdapter::StopBits nStopBit)
	{
		if (true == mSerialOpend.load())
			return SUCCESS;
		mComPort = port;
		mParity = nParity;
		mByteSize = nByteSize;
		mStopBit = nStopBit;
		mBaudRate = nBaud;
		mQueue.clear();
		if (std::this_thread::get_id() == mReadThread.get_id() ||
			std::this_thread::get_id() == mDetectThread.get_id()) {
			return FAILURE; 
		}
		auto ret = openSerialPort();
		if (SUCCESS == ret) {
			quitReadThread();
			quitDetectThread();
			quitWriteThread();
			mReadThread = std::thread(std::mem_fn(&serialAdapter::readSerialData),this);
			mDetectThread = std::thread(std::mem_fn(&serialAdapter::detectSerialAvaliable), this);
			mWriteThread = std::thread(std::mem_fn(&serialAdapter::writeSerialData), this);
			mSerialOpend.store(true);
			return SUCCESS;
		}
		return FAILURE;

	}

	RET_CODE serialAdapter::close()
	{
		if (false == mSerialOpend.load())
			return SUCCESS;
		if (std::this_thread::get_id() == mReadThread.get_id()||
			std::this_thread::get_id() == mDetectThread.get_id()) {
			return FAILURE;
		}
		quitReadThread();
		quitWriteThread();
		quitDetectThread();
		closeSerialPort();
		mSerialOpend = false;
		return SUCCESS;
	}

	RET_CODE serialAdapter::openSerialPort(void)
	{
#ifdef _WIN32
		CString sCom;
		sCom.Format(_T("\\\\.\\COM%d"), mComPort);
		mComFile= CreateFile(sCom.GetBuffer(50),
			GENERIC_READ | GENERIC_WRITE,
			0,/* do not share*/
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			NULL);
		if (mComFile == INVALID_HANDLE_VALUE) {
			return FAILURE;
		}
		if (SUCCESS != SetupSerialPort(mComFile,
										mBaudRate,
										mParity,
										mByteSize,
										mStopBit)) {
			CloseHandle(mComFile);
			return FAILURE;
		}
		return SUCCESS;
#else
		return FAILURE;
#endif
	}

	RET_CODE serialAdapter::closeSerialPort(void)
	{
#ifdef _WIN32
		bool ret = CloseHandle(mComFile);
		if (ret)
			return SUCCESS;
		else
			return FAILURE;
#endif
	}

#ifdef _WIN32
	void serialAdapter::syncParity(DCB& dcb, serialAdapter::Parity parity)
	{
		switch (parity) {
		case serialAdapter::NO_PARITY:
			dcb.Parity = NOPARITY;
			break;
		case serialAdapter::EVEN_PARITY:
			dcb.Parity = EVENPARITY;
			break;
		case serialAdapter::MARK_PARITY:
			dcb.Parity = MARKPARITY;
			break;
		case serialAdapter::ODD_PARITY:
			dcb.Parity = ODDPARITY;
			break;
		case serialAdapter::SPACE_PARITY:
			dcb.Parity = SPACEPARITY;
			break;
		default:
			dcb.Parity = NOPARITY;
			break;
		}
	}
	void serialAdapter::syncDataBits(DCB& dcb, serialAdapter::DataBits databit)
	{
		switch (databit) {
		case serialAdapter::DATA_BITS_5:
			dcb.ByteSize = DATABITS_5;
			break;
		case serialAdapter::DATA_BITS_6:
			dcb.ByteSize = DATABITS_6;
			break;
		case serialAdapter::DATA_BITS_7:
			dcb.ByteSize = DATABITS_7;
			break;
		case serialAdapter::DATA_BITS_8:
			dcb.ByteSize = DATABITS_8;
			break;
		default:
			dcb.ByteSize = DATABITS_8;
			break;
		}
	}
	void serialAdapter::syncStopBits(DCB& dcb, serialAdapter::StopBits stopbit)
	{
		switch (stopbit) {
		case serialAdapter::STOP_BITS_1:
			dcb.StopBits = ONESTOPBIT;
			break;
		case serialAdapter::STOP_BITS_1_5:
			dcb.StopBits = ONE5STOPBITS;
			break;
		case serialAdapter::STOP_BITS_2:
			dcb.StopBits = TWOSTOPBITS;
			break;
		default:
			dcb.StopBits = ONESTOPBIT;
			break;
		}
	}
	RET_CODE serialAdapter::SetupSerialPort(HANDLE file,
		unsigned long baud,
		serialAdapter::Parity nParity,
		serialAdapter::DataBits nDatabits,
		serialAdapter::StopBits nStopbits,
		unsigned short readTimeout)
	{
		DCB ndcb;
		COMMTIMEOUTS timeouts;
		SecureZeroMemory(&ndcb, sizeof(DCB));
		ndcb.DCBlength = sizeof(DCB);
		timeouts.ReadIntervalTimeout = 0;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = readTimeout;
		timeouts.WriteTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		SetCommTimeouts(file, &timeouts);
		if (!GetCommState(file, &ndcb)) {
			throw std::runtime_error("get commstate error");
		}
		ndcb.BaudRate = baud;
		syncParity(ndcb, nParity);
		syncDataBits(ndcb, nDatabits);
		syncStopBits(ndcb, nStopbits);
		ndcb.fRtsControl = RTS_CONTROL_DISABLE;
		ndcb.fDtrControl = DTR_CONTROL_ENABLE;
		ndcb.fOutxCtsFlow = FALSE;
		ndcb.fOutxDsrFlow = FALSE;
		ndcb.fOutX = FALSE;
		ndcb.fInX = FALSE;
		if (!SetCommState(file, &ndcb)) 
			throw std::runtime_error("SetCommState failed\n");
		PurgeComm(file, PURGE_RXCLEAR | PURGE_TXCLEAR 
						| PURGE_RXABORT | PURGE_TXABORT);

		/*clear error*/
		DWORD dwError;
		COMSTAT cs;
		if (!ClearCommError(file, &dwError, &cs))
			throw std::runtime_error("ClearCommError failed\n");
		/*set mask*/
		SetCommMask(file, EV_RXCHAR);
		return SUCCESS;
	}
#else
#endif
	void serialAdapter::readSerialData(void)
	{
#ifdef _WIN32
		OVERLAPPED osRead;
		memset(&osRead, 0, sizeof(OVERLAPPED));
		osRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		bool loop = true;
		while (loop) {
			DWORD readLen = 0;
			unsigned char data = 0x00;
			if (mReadThreadQuit.load() == true) {
				return;
			}
			if (!ReadFile(mComFile, &data, 1, &readLen, &osRead)) {
				if (GetLastError() == ERROR_IO_PENDING) {
					GetOverlappedResult(mComFile, &osRead, &readLen, true);
				}
			}
			if (readLen) {
				std::lock_guard<std::mutex> ltx(mListenerMtx);
				for (auto listen : mListener) {
					auto lock = listen.lock();
					if (lock)
						lock->onData(data);
				}
			}
		}
#endif
	}

	RET_CODE serialAdapter::checkPortAvaliable(void)
	{
#ifdef _WIN32
		HANDLE comFile;
		CString sCom;
		sCom.Format(_T("\\\\.\\COM%d"), mComPort);
		comFile = CreateFile(sCom.GetBuffer(50),
			GENERIC_READ | GENERIC_WRITE,
			0,/* do not share*/
			NULL,
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
			NULL);
		if (comFile == INVALID_HANDLE_VALUE) {
			if (ERROR_ACCESS_DENIED == GetLastError()) {
				return FAILURE;
			}
			else if (ERROR_FILE_NOT_FOUND == GetLastError()) {
				return SUCCESS;
			}
		}
		else {
			CloseHandle(comFile);
			return SUCCESS;
		}
		return FAILURE;
#endif
	}

	void serialAdapter::detectSerialAvaliable(void)
	{
#ifdef _WIN32
		bool loop = true;
		while (loop) {
			if (mDetectThreadQuit.load() == true) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			if (SUCCESS == checkPortAvaliable()) {
				{
					quitReadThread();
					quitWriteThread();
					mSerialOpend.store(false);
					std::lock_guard<std::mutex> ltx(mListenerMtx);
					for (auto listener : mListener) {
						auto f = listener.lock();
						if (f) {
							f->onClosed();
						}
					}
				}
				return;
			}
		}
#endif
	}
	void serialAdapter::writeSerialData(void)
	{
#ifdef _WIN32
		bool loop = true;
		while (loop) {
			auto pData = mQueue.wait_and_pop();
			if (true == mWriteThreadQuit.load()) {
				mQueue.clear();
				return;
			}
			if (nullptr != pData) {
				writeData(pData->data(), pData->size());
			}

		}
#endif
	}
	std::shared_ptr<std::vector<std::string>> serialAdapter::enumSerial(void)
	{
		return std::shared_ptr<std::vector<std::string>>();
	}
	RET_CODE serialAdapter::writeData(unsigned char *ptr, std::size_t size)
	{
#ifdef _WIN32
		DWORD writeByte = 0;
		OVERLAPPED osWrite;
		memset(&osWrite, 0,sizeof(osWrite));
		osWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
		std::lock_guard<std::mutex> lock(mWriteMtx);
		if (!WriteFile(mComFile, ptr, static_cast<WORD>(size), &writeByte, &osWrite)) {
			if (GetLastError() == ERROR_IO_PENDING) {
				GetOverlappedResult(mComFile, &osWrite, &writeByte, true);
				if (writeByte == size)
					return SUCCESS;
				else
					return FAILURE;
			}
		}
		return SUCCESS;
#else
		return FAILURE;
#endif
	}
	RET_CODE serialAdapter::write_sync(unsigned char data)
	{
		if (false == mSerialOpend.load()) {
			return FAILURE;
		}
#ifdef _WIN32
		return writeData(&data, 1);
#else
		return FAILURE;
#endif
	}
	RET_CODE serialAdapter::write_sync(std::shared_ptr<std::vector<unsigned char>> dat)
	{
		if (false == mSerialOpend.load())
			return FAILURE;
#ifdef _WIN32
		return writeData(dat->data(), dat->size());
#else
		return FAILURE;
#endif
	}
	RET_CODE serialAdapter::write_async(unsigned char data)
	{
		auto ptr = std::make_shared<std::vector<unsigned char>>(
					std::initializer_list<unsigned char>{ data });
		mQueue.push(ptr);
		return SUCCESS;
	}
	RET_CODE serialAdapter::write_async(std::shared_ptr<std::vector<unsigned char>> data)
	{
		mQueue.push(data);
		return SUCCESS;
	}
}

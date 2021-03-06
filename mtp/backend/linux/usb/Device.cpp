/*
 * Android File Transfer for Linux: MTP client for android devices
 * Copyright (C) 2015  Vladimir Menshakov

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include <usb/Device.h>
#include <usb/Exception.h>
#include <mtp/usb/TimeoutException.h>
#include <mtp/usb/DeviceBusyException.h>
#include <mtp/ByteArray.h>

#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/time.h>

#include "linux/usbdevice_fs.h"

#define IOCTL(...) do { int r = ioctl(__VA_ARGS__); if (r < 0) throw Exception("ioctl(" #__VA_ARGS__ ")"); } while(false)

namespace mtp { namespace usb
{

	FileHandler::~FileHandler()
	{ close(_fd); }

	Device::InterfaceToken::InterfaceToken(int fd, unsigned interfaceNumber): _fd(fd), _interfaceNumber(interfaceNumber)
	{
		int r = ioctl(_fd, USBDEVFS_CLAIMINTERFACE, &interfaceNumber);
		if (r < 0)
		{
			if (errno == EBUSY)
				throw DeviceBusyException();
			else
				throw Exception("ioctl(_fd, USBDEVFS_CLAIMINTERFACE, &interfaceNumber)");
		}
	}

	Device::InterfaceToken::~InterfaceToken()
	{
		ioctl(_fd, USBDEVFS_RELEASEINTERFACE, &_interfaceNumber);
	}

	Device::Device(int fd, const EndpointPtr &controlEp): _fd(fd), _capabilities(0), _controlEp(controlEp)
	{
		try { IOCTL(_fd.Get(), USBDEVFS_GET_CAPABILITIES, &_capabilities); }
		catch(const std::exception &ex)
		{ fprintf(stderr, "get usbfs capabilities failed: %s\n", ex.what()); }
	}

	Device::~Device()
	{ }

	int Device::GetConfiguration() const
	{
		return 0;
	}

	void Device::SetConfiguration(int idx)
	{
		fprintf(stderr, "SetConfiguration(%d): not implemented", idx);
	}

	struct Device::Urb
	{
		int						Fd;
		ByteArray				Buffer;
		usbdevfs_urb			KernelUrb;

		static size_t PacketsPerBuffer(u8 type)
		{ return type == USBDEVFS_URB_TYPE_BULK? 1024: 1; }

		Urb(int fd, u8 type, const EndpointPtr & ep): Fd(fd), Buffer(PacketsPerBuffer(type) * ep->GetMaxPacketSize()), KernelUrb()
		{
			usbdevfs_urb &urb	= KernelUrb;
			urb.type			= type;
			urb.endpoint		= ep->GetAddress();
			urb.buffer			= Buffer.data();
			urb.buffer_length	= Buffer.size();
		}

		void Submit()
		{
			IOCTL(Fd, USBDEVFS_SUBMITURB, &KernelUrb);
		}

		void Discard()
		{
			int r = ioctl(Fd, USBDEVFS_DISCARDURB, &KernelUrb);
			if (r != 0)
			{
				perror("ioctl(USBDEVFS_DISCARDURB)");
			}
		}

		size_t Send(const IObjectInputStreamPtr &inputStream)
		{
			size_t r = inputStream->Read(Buffer.data(), Buffer.size());
			//HexDump("write", ByteArray(Buffer.data(), Buffer.data() + r));
			KernelUrb.buffer_length = r;
			return r;
		}

		size_t Send(const ByteArray &inputData)
		{
			size_t r = std::min(Buffer.size(), inputData.size());
			std::copy(inputData.data(), inputData.data() + r, Buffer.data());
			KernelUrb.buffer_length = r;
			return r;
		}

		size_t Recv(const IObjectOutputStreamPtr &outputStream)
		{
			//HexDump("read", ByteArray(Buffer.data(), Buffer.data() + KernelUrb.actual_length));
			return outputStream->Write(Buffer.data(), KernelUrb.actual_length);
		}

		ByteArray Recv()
		{ return ByteArray(Buffer.begin(), Buffer.begin() + KernelUrb.actual_length); }

		void SetContinuationFlag(bool continuation)
		{
			if (continuation)
				KernelUrb.flags |= USBDEVFS_URB_BULK_CONTINUATION;
			else
				KernelUrb.flags &= ~USBDEVFS_URB_BULK_CONTINUATION;
		}
	};

	void * Device::Reap(int timeout)
	{
		timeval started = {};
		if (gettimeofday(&started, NULL) == -1)
			throw usb::Exception("gettimeofday");

		pollfd fd = {};
		fd.fd		= _fd.Get();
		fd.events	= POLLOUT;
		int r = poll(&fd, 1, timeout);

		timeval now = {};
		if (gettimeofday(&now, NULL) == -1)
			throw usb::Exception("gettimeofday");

		if (r < 0)
			throw Exception("poll");

		if (r == 0 && timeout > 0)
		{
			int ms = (now.tv_sec - started.tv_sec) * 1000 + (now.tv_usec - started.tv_usec) / 1000;
			fprintf(stderr, "%d ms since the last poll call\n", ms);
		}

		usbdevfs_urb *urb;
		r = ioctl(_fd.Get(), USBDEVFS_REAPURBNDELAY, &urb);
		if (r == 0)
			return urb;
		else if (errno == EAGAIN)
			throw TimeoutException("timeout reaping usb urb");
		else
			throw Exception("ioctl");
	}

	void Device::Submit(const UrbPtr &urb, int timeout)
	{
		urb->Submit();
		{
			scoped_mutex_lock l(_mutex);
			_urbs.insert(std::make_pair(&urb->KernelUrb, urb));
		}
		try
		{
			while(true)
			{
				UrbPtr completedUrb;
				{
					void *completedKernelUrb = Reap(timeout);
					scoped_mutex_lock l(_mutex);
					auto urbIt = _urbs.find(completedKernelUrb);
					if (urbIt == _urbs.end())
					{
						fprintf(stderr, "got unknown urb: %p\n", completedKernelUrb);
						continue;
					}
					completedUrb = urbIt->second;
					_urbs.erase(urbIt);
				}
				if (completedUrb == urb)
					break;
			}
		}
		catch(const TimeoutException &ex)
		{
			urb->Discard();
			throw;
		}
		catch(const std::exception &ex)
		{
			fprintf(stderr, "error while submitting urb: %s\n", ex.what());
			urb->Discard();
			throw;
		}
	}

	void Device::WriteBulk(const EndpointPtr & ep, const IObjectInputStreamPtr &inputStream, int timeout)
	{
		UrbPtr urb = std::make_shared<Urb>(_fd.Get(), USBDEVFS_URB_TYPE_BULK, ep);
		size_t transferSize = urb->Buffer.size();

		size_t r;
		bool continuation = false;
		do
		{
			r = urb->Send(inputStream);
			urb->SetContinuationFlag(continuation);
			continuation = true;
			Submit(urb, timeout);

			ProcessControl();
		}
		while(r == transferSize);
	}

	void Device::ReadBulk(const EndpointPtr & ep, const IObjectOutputStreamPtr &outputStream, int timeout)
	{
		UrbPtr urb = std::make_shared<Urb>(_fd.Get(), USBDEVFS_URB_TYPE_BULK, ep);
		size_t transferSize = urb->Buffer.size();

		size_t r;
		bool continuation = false;
		do
		{
			urb->SetContinuationFlag(continuation);
			continuation = true;
			Submit(urb, timeout);

			ProcessControl();
			r = urb->Recv(outputStream);
		}
		while(r == transferSize);
	}

	u8 Device::TransactionType(const EndpointPtr &ep)
	{
		EndpointType type = ep->GetType();
		switch(type)
		{
		case EndpointType::Control:
			return USBDEVFS_URB_TYPE_CONTROL;
		case EndpointType::Isochronous:
			return USBDEVFS_URB_TYPE_ISO;
		case EndpointType::Bulk:
			return USBDEVFS_URB_TYPE_BULK;
		case EndpointType::Interrupt:
			return USBDEVFS_URB_TYPE_INTERRUPT;
		default:
			throw std::runtime_error("invalid endpoint type");
		}
	}

	void Device::WriteControl(u8 type, u8 req, u16 value, u16 index, const ByteArray &data, bool interruptCurrentTransaction, int timeout)
	{
		usbdevfs_ctrltransfer ctrl = { };
		ctrl.bRequestType = type;
		ctrl.bRequest = req;
		ctrl.wValue = value;
		ctrl.wIndex = index;
		ctrl.wLength = data.size();
		ctrl.data = const_cast<u8 *>(data.data());
		ctrl.timeout = timeout;

		int fd = _fd.Get();

		_controls.push(
		[fd, ctrl, interruptCurrentTransaction]()
		{
			int r = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
			if (r >= 0)
			{
				if (interruptCurrentTransaction)
					throw std::runtime_error("transaction aborted");
				else
					return;
			}
			else if (errno == EAGAIN)
				throw TimeoutException("timeout sending control transfer");
			else
				throw Exception("ioctl");
		});
	}

	void Device::ProcessControl()
	{
		scoped_mutex_lock l(_mutex);
		while(!_controls.empty())
		{
			try { _controls.front()(); } catch(const std::exception &ex) { _controls.pop(); throw; }
			_controls.pop();
		}
	}

}}

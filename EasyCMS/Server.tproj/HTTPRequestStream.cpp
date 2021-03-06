/*
	Copyleft (c) 2012-2016 EasyDarwin.ORG.  All rights reserved.
	Github: https://github.com/EasyDarwin
	WEChat: EasyDarwin
	Website: http://www.EasyDarwin.org
*/
/*
	File:       HTTPRequestStream.cpp

	Contains:   Implementation of HTTPRequestStream class.
*/

#include "HTTPRequestStream.h"
#include "StringParser.h"
#include "OSMemory.h"
#include "base64.h"
#include "OS.h"

#include <errno.h>

#define READ_DEBUGGING 0

HTTPRequestStream::HTTPRequestStream(TCPSocket* sock)
	: fSocket(sock),
	fRetreatBytes(0),
	fRetreatBytesRead(0),
	fCurOffset(0),
	fEncodedBytesRemaining(0),
	fRequest(fRequestBuffer, 0),
	fRequestPtr(nullptr),
	fDecode(false),
	fIsDataPacket(false),
	fPrintMSG(false)
{}

void HTTPRequestStream::SnarfRetreat(HTTPRequestStream &fromRequest)
{
	// Simplest thing to do is to just completely blow away everything in this current
	// stream, and replace it with the retreat bytes from the other stream.
	fRequestPtr = nullptr;
	Assert(fRetreatBytes < EASY_REQUEST_BUFFER_SIZE_LEN);
	fRetreatBytes = fromRequest.fRetreatBytes;
	fEncodedBytesRemaining = fCurOffset = fRequest.Len = 0;
	::memcpy(&fRequestBuffer[0], fromRequest.fRequest.Ptr + fromRequest.fRequest.Len, fromRequest.fRetreatBytes);
}

QTSS_Error HTTPRequestStream::ReadRequest()
{
	while (true)
	{
		UInt32 newOffset = 0;

		//If this is the case, we already HAVE a request on this session, and we now are done
		//with the request and want to move onto the next one. The first thing we should do
		//is check whether there is any lingering(������) data in the stream. If there is, the parent
		//session believes that is part of a new request
		if (fRequestPtr != nullptr)
		{
			fRequestPtr = nullptr;//flag that we no longer have a complete request

			// Take all the retreated leftover data and move it to the beginning of the buffer
			if ((fRetreatBytes > 0) && (fRequest.Len > 0))
				::memmove(fRequest.Ptr, fRequest.Ptr + fRequest.Len + fRetreatBytesRead, fRetreatBytes);

			// if we are decoding, we need to also move over the remaining encoded bytes
			// to the right position in the fRequestBuffer
			if (fEncodedBytesRemaining > 0)
			{
				//Assert(fEncodedBytesRemaining < 4);

				// The right position is at fRetreatBytes offset in the request buffer. The reason for this is:
				//  1) We need to find a place in the request buffer where we know we have enough space to store
				//  fEncodedBytesRemaining. fRetreatBytes + fEncodedBytesRemaining will always be less than
				//  kRequestBufferSize because all this data must have been in the same request buffer, together, at one point.
				//
				//  2) We need to make sure that there is always more data in the RequestBuffer than in the decoded
				//  request buffer, otherwise we could overrun the decoded request buffer (we bounds check on the encoded
				//  buffer, not the decoded buffer). Leaving fRetreatBytes as empty space in the request buffer ensures
				//  that this principle is maintained. 
				::memmove(&fRequestBuffer[fRetreatBytes], &fRequestBuffer[fCurOffset - fEncodedBytesRemaining], fEncodedBytesRemaining);
				fCurOffset = fRetreatBytes + fEncodedBytesRemaining;
				Assert(fCurOffset < EASY_REQUEST_BUFFER_SIZE_LEN);
			}
			else
				fCurOffset = fRetreatBytes;

			newOffset = fRequest.Len = fRetreatBytes;
			fRetreatBytes = fRetreatBytesRead = 0;
		}

		// We don't have any new data, so try and get some
		if (newOffset == 0)
		{
			if (fRetreatBytes > 0)
			{
				// This will be true if we've just snarfed another input stream, in which case the encoded data
				// is copied into our request buffer, and its length is tracked in fRetreatBytes.
				// If this is true, just fall through and decode the data.
				newOffset = fRetreatBytes;
				fRetreatBytes = 0;
				Assert(fEncodedBytesRemaining == 0);
			}
			else
			{
				// We don't have any new data, get some from the socket...
				QTSS_Error sockErr = fSocket->Read(&fRequestBuffer[fCurOffset],
					(EASY_REQUEST_BUFFER_SIZE_LEN - fCurOffset) - 1, &newOffset);
				//assume the client is dead if we get an error back
				if (sockErr == EAGAIN)
					return QTSS_NoErr;
				if (sockErr != QTSS_NoErr)
				{
					Assert(!fSocket->IsConnected());
					return sockErr;
				}
			}

			if (fDecode)
			{
				// If we need to decode this data, do it now.
				Assert(fCurOffset >= fEncodedBytesRemaining);
				QTSS_Error decodeErr = this->decodeIncomingData(&fRequestBuffer[fCurOffset - fEncodedBytesRemaining],
					newOffset + fEncodedBytesRemaining);
				// If the above function returns an error, it is because we've
				// encountered some non-base64 data in the stream. We can process
				// everything up until that point, but all data after this point will
				// be ignored.
				if (decodeErr == QTSS_NoErr)
					Assert(fEncodedBytesRemaining < 4);
			}
			else
				fRequest.Len += newOffset;
			Assert(fRequest.Len < EASY_REQUEST_BUFFER_SIZE_LEN);
			fCurOffset += newOffset;
		}
		Assert(newOffset > 0);

		// See if this is an interleaved data packet
		if ('$' == *(fRequest.Ptr))
		{
			if (fRequest.Len < 4)
				continue;
			UInt16* dataLenP = reinterpret_cast<UInt16*>(fRequest.Ptr);
			UInt32 interleavedPacketLen = ntohs(dataLenP[1]) + 4;
			if (interleavedPacketLen > fRequest.Len)
				continue;

			//put back any data that is not part of the header
			fRetreatBytes += fRequest.Len - interleavedPacketLen;
			fRequest.Len = interleavedPacketLen;

			fRequestPtr = &fRequest;
			fIsDataPacket = true;
			return QTSS_RequestArrived;
		}
		fIsDataPacket = false;

		if (fPrintMSG)
		{
			DateBuffer theDate;
			DateTranslator::UpdateDateBuffer(&theDate, 0); // get the current GMT date and time
			qtss_printf("\n\n#C->S:\n#time: ms=%" _U32BITARG_ " date=%s\n", static_cast<UInt32>(OS::StartTimeMilli_Int()), theDate.GetDateBuffer());

			if (fSocket != nullptr)
			{
				UInt16 serverPort = fSocket->GetLocalPort();
				UInt16 clientPort = fSocket->GetRemotePort();
				StrPtrLen* theLocalAddrStr = fSocket->GetLocalAddrStr();
				StrPtrLen* theRemoteAddrStr = fSocket->GetRemoteAddrStr();
				if (theLocalAddrStr != nullptr)
				{
					qtss_printf("#server: ip="); theLocalAddrStr->PrintStr(); qtss_printf(" port=%u\n", serverPort);
				}
				else
				{
					qtss_printf("#server: ip=nullptr port=%u\n", serverPort);
				}

				if (theRemoteAddrStr != nullptr)
				{
					qtss_printf("#client: ip="); theRemoteAddrStr->PrintStr(); qtss_printf(" port=%u\n", clientPort);
				}
				else
				{
					qtss_printf("#client: ip=nullptr port=%u\n", clientPort);
				}

			}

			StrPtrLen str(fRequest);
			str.PrintStrEOL("\n\r\n", "\n");// print the request but stop on \n\r\n and add a \n afterwards.
		}

		//use a StringParser object to search for a double EOL, which signifies the end of
		//the header.
		bool weAreDone = false;
		StringParser headerParser(&fRequest);

		UInt16 lcount = 0;
		while (headerParser.GetThruEOL(nullptr))
		{
			lcount++;
			if (headerParser.ExpectEOL())
			{
				//The legal end-of-header sequences are \r\r, \r\n\r\n, & \n\n. NOT \r\n\r!
				//If the packets arrive just a certain way, we could get here with the latter
				//combo, and not wait for a final \n.
				if ((headerParser.GetDataParsedLen() > 2) &&
					(memcmp(headerParser.GetCurrentPosition() - 3, "\r\n\r", 3) == 0))
					continue;
				weAreDone = true;
				break;
			}
			//         else if (lcount == 1) 
					 //{
			//             // if this request is actually a ShoutCast password it will be 
			//             // in the form of "xxxxxx\r" where "xxxxx" is the password.
			//             // If we get a 1st request line ending in \r with no blanks we will
			//             // assume that this is the end of the request.
			//             UInt16 flag = 0;
			//             UInt16 i = 0;
			//             for (i=0; i<fRequest.Len; i++)
			//             {
			//                 if (fRequest.Ptr[i] == ' ')
			//                     flag++;
			//             }
			//             if (flag == 0)
			//             {
			//                 weAreDone = true;
			//                 break;
			//             }
			//         }
		}

		//weAreDone means we have gotten a full request
		if (weAreDone)
		{
			//put back any data that is not part of the header
			fRequest.Len -= headerParser.GetDataRemaining();
			fRetreatBytes += headerParser.GetDataRemaining();

			fRequestPtr = &fRequest;
			return QTSS_RequestArrived;
		}

		//check for a full buffer
		if (fCurOffset == EASY_REQUEST_BUFFER_SIZE_LEN - 1)
		{
			fRequestPtr = &fRequest;
			return E2BIG;
		}
	}
}

QTSS_Error HTTPRequestStream::Read(void* ioBuffer, UInt32 inBufLen, UInt32* outLengthRead)
{
	UInt32 theLengthRead = 0;
	UInt8* theIoBuffer = static_cast<UInt8*>(ioBuffer);

	//
	// If there are retreat bytes available, read them first.
	if (fRetreatBytes > 0)
	{
		theLengthRead = fRetreatBytes;
		if (inBufLen < theLengthRead)
			theLengthRead = inBufLen;

		::memcpy(theIoBuffer, fRequest.Ptr + fRequest.Len + fRetreatBytesRead, theLengthRead);

		//
		// We should not update fRequest.Len even though we've read some of the retreat bytes.
		// fRequest.Len always refers to the length of the request header. Instead, we
		// have a separate variable, fRetreatBytesRead
		fRetreatBytes -= theLengthRead;
		fRetreatBytesRead += theLengthRead;
#if READ_DEBUGGING
		qtss_printf("In HTTPRequestStream::Read: Got %d Retreat Bytes\n", theLengthRead);
#endif  
	}

	//
	// If there is still space available in ioBuffer, continue. Otherwise, we can return now
	if (theLengthRead == inBufLen)
	{
		if (outLengthRead != nullptr)
			*outLengthRead = theLengthRead;
		return QTSS_NoErr;
	}

	//
	// Read data directly from the socket and place it in our buffer
	UInt32 theNewOffset = 0;
	QTSS_Error theErr = fSocket->Read(&theIoBuffer[theLengthRead], inBufLen - theLengthRead, &theNewOffset);
#if READ_DEBUGGING
	qtss_printf("In HTTPRequestStream::Read: Got %d bytes off Socket\n", theNewOffset);
#endif  
	if (outLengthRead != nullptr)
		*outLengthRead = theNewOffset + theLengthRead;

	return theErr;
}

QTSS_Error HTTPRequestStream::decodeIncomingData(char* inSrcData, UInt32 inSrcDataLen)
{
	Assert(fRetreatBytes == 0);

	if (fRequest.Ptr == &fRequestBuffer[0])
	{
		fRequest.Ptr = NEW char[EASY_REQUEST_BUFFER_SIZE_LEN];
		fRequest.Len = 0;
	}

	// We always decode up through the last chunk of 4.
	fEncodedBytesRemaining = inSrcDataLen & 3;

	// Let our friendly Base64Decode function know this by nullptr terminating at that point
	UInt32 bytesToDecode = inSrcDataLen - fEncodedBytesRemaining;
	char endChar = inSrcData[bytesToDecode];
	inSrcData[bytesToDecode] = '\0';

	UInt32 encodedBytesConsumed = 0;

	// Loop until the whole load is decoded
	while (encodedBytesConsumed < bytesToDecode)
	{
		Assert((encodedBytesConsumed & 3) == 0);
		Assert((bytesToDecode & 3) == 0);

		UInt32 bytesDecoded = Base64decode(fRequest.Ptr + fRequest.Len, inSrcData + encodedBytesConsumed);

		// If bytesDecoded is 0, we will end up being in an endless loop. The
		// base64 must be corrupt, so let's just return an error and abort
		if (bytesDecoded == 0)
		{
			//Assert(0);
			return QTSS_BadArgument;
		}

		fRequest.Len += bytesDecoded;

		// Assuming the stream is valid, the # of encoded bytes we just consumed is
		// 4/3rds of the number of decoded bytes returned by the decode function,
		// rounded up to the nearest multiple of 4.
		encodedBytesConsumed += (bytesDecoded / 3) * 4;
		if ((bytesDecoded % 3) > 0)
			encodedBytesConsumed += 4;

	}

	// Make sure to replace the sacred endChar
	inSrcData[bytesToDecode] = endChar;

	Assert(fRequest.Len < EASY_REQUEST_BUFFER_SIZE_LEN);
	Assert(encodedBytesConsumed == bytesToDecode);

	return QTSS_NoErr;
}


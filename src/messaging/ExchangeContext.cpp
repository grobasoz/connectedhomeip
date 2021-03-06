/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file implements the ExchangeContext class.
 *
 */
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#include <core/CHIPCore.h>
#include <core/CHIPEncoding.h>
#include <messaging/ExchangeContext.h>
#include <messaging/ExchangeMgr.h>
#include <protocols/CHIPProtocols.h>
#include <protocols/common/CommonProtocol.h>
#include <support/logging/CHIPLogging.h>
#include <system/SystemTimer.h>

using namespace chip::Encoding;
using namespace chip::Inet;
using namespace chip::System;

namespace chip {

static void DefaultOnMessageReceived(ExchangeContext * ec, const PacketHeader & packetHeader, uint32_t protocolId, uint8_t msgType,
                                     PacketBuffer * payload)
{
    ChipLogError(ExchangeManager, "Dropping unexpected message %08" PRIX32 ":%d %04" PRIX16 " MsgId:%08" PRIX32, protocolId,
                 msgType, ec->GetExchangeId(), packetHeader.GetMessageId());

    PacketBuffer::Free(payload);
}

bool ExchangeContext::IsInitiator() const
{
    return mFlags.Has(ExFlagValues::kFlagInitiator);
}

bool ExchangeContext::IsResponseExpected() const
{
    return mFlags.Has(ExFlagValues::kFlagResponseExpected);
}

void ExchangeContext::SetInitiator(bool inInitiator)
{
    mFlags.Set(ExFlagValues::kFlagInitiator, inInitiator);
}

void ExchangeContext::SetResponseExpected(bool inResponseExpected)
{
    mFlags.Set(ExFlagValues::kFlagResponseExpected, inResponseExpected);
}

CHIP_ERROR ExchangeContext::SendMessage(uint16_t protocolId, uint8_t msgType, PacketBuffer * msgBuf, uint16_t sendFlags,
                                        void * msgCtxt)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    PayloadHeader payloadHeader;

    // Don't let method get called on a freed object.
    VerifyOrDie(mExchangeMgr != nullptr && mRefCount != 0);

    // we hold the exchange context here in case the entity that
    // originally generated it tries to close it as a result of
    // an error arising below. at the end, we have to close it.
    AddRef();

    // Set the exchange ID for this header.
    payloadHeader.SetExchangeID(mExchangeId);

    // Set the protocol ID for this header.
    payloadHeader.SetProtocolID(protocolId);

    // Set the message type for this header.
    payloadHeader.SetMessageType(msgType);

    // If a response message is expected...
    if ((sendFlags & kSendFlag_ExpectResponse) != 0)
    {
        // Only one 'response expected' message can be outstanding at a time.
        VerifyOrExit(!IsResponseExpected(), err = CHIP_ERROR_INCORRECT_STATE);

        SetResponseExpected(true);

        // Arm the response timer if a timeout has been specified.
        if (mResponseTimeout > 0)
        {
            err = StartResponseTimer();
            SuccessOrExit(err);
        }
    }

    payloadHeader.SetInitiator(IsInitiator());

    err    = mExchangeMgr->GetSessionMgr()->SendMessage(payloadHeader, mPeerNodeId, msgBuf);
    msgBuf = nullptr;
    SuccessOrExit(err);

exit:
    if (err != CHIP_NO_ERROR && IsResponseExpected())
    {
        CancelResponseTimer();
        SetResponseExpected(false);
    }
    if (msgBuf != nullptr && (sendFlags & kSendFlag_RetainBuffer) == 0)
    {
        PacketBuffer::Free(msgBuf);
    }

    // Release the reference to the exchange context acquired above. Under normal circumstances
    // this will merely decrement the reference count, without actually freeing the exchange context.
    // However if one of the function calls in this method resulted in a callback to the protocol,
    // the protocol may have released its reference, resulting in the exchange context actually
    // being freed here.
    Release();

    return err;
}

/**
 *  Increment the reference counter for the exchange context by one.
 *
 */
void ExchangeContext::AddRef()
{
    mRefCount++;
#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogProgress(ExchangeManager, "ec id: %d [%04" PRIX16 "], refCount++: %d", (this - mExchangeMgr->ContextPool + 1),
                    mExchangeId, mRefCount);
#endif
}

void ExchangeContext::DoClose(bool clearRetransTable)
{
    // Clear protocol callbacks
    mDelegate = nullptr;

    // Cancel the response timer.
    CancelResponseTimer();
}

/**
 *  Gracefully close an exchange context. This call decrements the
 *  reference count and releases the exchange when the reference
 *  count goes to zero.
 *
 */
void ExchangeContext::Close()
{
    VerifyOrDie(mExchangeMgr != nullptr && mRefCount != 0);

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogProgress(ExchangeManager, "ec id: %d [%04" PRIX16 "], %s", (this - mExchangeMgr->ContextPool + 1), mExchangeId,
                    __func__);
#endif

    DoClose(false);
    Release();
}
/**
 *  Abort the Exchange context immediately and release all
 *  references to it.
 *
 */
void ExchangeContext::Abort()
{
    VerifyOrDie(mExchangeMgr != nullptr && mRefCount != 0);

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
    ChipLogProgress(ExchangeManager, "ec id: %d [%04" PRIX16 "], %s", (this - mExchangeMgr->ContextPool + 1), mExchangeId,
                    __func__);
#endif

    DoClose(true);
    Release();
}

/**
 *  Release reference to this exchange context. If count is down
 *  to one then close the context, reset all protocol callbacks,
 *  and stop all timers.
 *
 */
void ExchangeContext::Release()
{
    VerifyOrDie(mExchangeMgr != nullptr && mRefCount != 0);

    if (mRefCount == 1)
    {
        // Ideally, in this scenario, the retransmit table should
        // be clear of any outstanding messages for this context and
        // the boolean parameter passed to DoClose() should not matter.
        ExchangeManager * em = mExchangeMgr;
#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
        uint16_t tmpid = mExchangeId;
#endif

        DoClose(false);
        mRefCount    = 0;
        mExchangeMgr = nullptr;

        em->DecrementContextsInUse();

#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
        ChipLogProgress(ExchangeManager, "ec-- id: %d [%04" PRIX16 "], inUse: %d, addr: 0x%x", (this - em->ContextPool + 1), tmpid,
                        em->GetContextsInUse(), this);
#endif
        SYSTEM_STATS_DECREMENT(chip::System::Stats::kExchangeMgr_NumContexts);
    }
    else
    {
        mRefCount--;
#if defined(CHIP_EXCHANGE_CONTEXT_DETAIL_LOGGING)
        ChipLogProgress(ExchangeManager, "ec id: %d [%04" PRIX16 "], refCount--: %d", (this - mExchangeMgr->ContextPool + 1),
                        mExchangeId, mRefCount);
#endif
    }
}

bool ExchangeContext::MatchExchange(const PacketHeader & packetHeader, const PayloadHeader & payloadHeader)
{
    // A given message is part of a particular exchange if...
    return

        // The exchange identifier of the message matches the exchange identifier of the context.
        (mExchangeId == payloadHeader.GetExchangeID())

        // AND The message was received from the peer node associated with the exchange, or the peer node identifier is 'any'.
        && ((mPeerNodeId == kAnyNodeId) || (mPeerNodeId == packetHeader.GetSourceNodeId().Value()))

        // AND The message was sent by an initiator and the exchange context is a responder (IsInitiator==false)
        //    OR The message was sent by a responder and the exchange context is an initiator (IsInitiator==true) (for the broadcast
        //    case, the initiator is ill defined)

        && (payloadHeader.IsInitiator() != IsInitiator());
}

CHIP_ERROR ExchangeContext::StartResponseTimer()
{
    System::Layer * lSystemLayer = mExchangeMgr->GetSessionMgr()->SystemLayer();
    if (lSystemLayer == nullptr)
    {
        // this is an assertion error, which shall never happen
        return CHIP_ERROR_INTERNAL;
    }

    return lSystemLayer->StartTimer(mResponseTimeout, HandleResponseTimeout, this);
}

void ExchangeContext::CancelResponseTimer()
{
    System::Layer * lSystemLayer = mExchangeMgr->GetSessionMgr()->SystemLayer();
    if (lSystemLayer == nullptr)
    {
        // this is an assertion error, which shall never happen
        return;
    }

    lSystemLayer->CancelTimer(HandleResponseTimeout, this);
}

void ExchangeContext::HandleResponseTimeout(System::Layer * aSystemLayer, void * aAppState, System::Error aError)
{
    ExchangeContext * ec = reinterpret_cast<ExchangeContext *>(aAppState);

    if (ec == nullptr)
        return;

    // NOTE: we don't set mResponseExpected to false here because the response could still arrive. If the user
    // wants to never receive the response, they must close the exchange context.

    ExchangeContextDelegate * delegate = ec->GetDelegate();

    // Call the user's timeout handler.
    if (delegate != nullptr)
        delegate->OnResponseTimeout(ec);
}

CHIP_ERROR ExchangeContext::HandleMessage(const PacketHeader & packetHeader, const PayloadHeader & payloadHeader,
                                          PacketBuffer * msgBuf)
{
    return HandleMessage(packetHeader, payloadHeader, msgBuf, nullptr);
}

CHIP_ERROR ExchangeContext::HandleMessage(const PacketHeader & packetHeader, const PayloadHeader & payloadHeader,
                                          PacketBuffer * msgBuf, ExchangeContext::MessageReceiveFunct umhandler)
{
    CHIP_ERROR err      = CHIP_NO_ERROR;
    uint16_t protocolId = 0;
    uint8_t messageType = 0;

    // We hold a reference to the ExchangeContext here to
    // guard against Close() calls(decrementing the reference
    // count) by the protocol before the CHIP Exchange
    // layer has completed its work on the ExchangeContext.
    AddRef();

    protocolId  = payloadHeader.GetProtocolID();
    messageType = payloadHeader.GetMessageType();

    // Return and not pass this to protocol if Common::Null Msg Type
    if ((protocolId == chip::Protocols::kChipProtocol_Common) && (messageType == chip::Protocols::Common::kMsgType_Null))
    {
        ExitNow(err = CHIP_NO_ERROR);
    }
    else
    {
        // Since we got the response, cancel the response timer.
        CancelResponseTimer();

        // If the context was expecting a response to a previously sent message, this message
        // is implicitly that response.
        SetResponseExpected(false);

        // Deliver the message to the app via its callback.
        if (umhandler)
        {
            umhandler(this, packetHeader, protocolId, messageType, msgBuf);
            msgBuf = nullptr;
        }
        else if (mDelegate != nullptr)
        {
            mDelegate->OnMessageReceived(this, packetHeader, protocolId, messageType, msgBuf);
            msgBuf = nullptr;
        }
        else
        {
            DefaultOnMessageReceived(this, packetHeader, protocolId, messageType, msgBuf);
        }
    }

exit:

    // Release the reference to the ExchangeContext that was held at the beginning of this function.
    // This call should also do the needful of closing the ExchangeContext if the protocol has
    // already made a prior call to Close().
    Release();

    if (msgBuf != nullptr)
    {
        PacketBuffer::Free(msgBuf);
    }

    return err;
}

} // namespace chip

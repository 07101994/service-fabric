// ------------------------------------------------------------
// Copyright (c) Microsoft Corporation.  All rights reserved.
// Licensed under the MIT License (MIT). See License.txt in the repo root for license information.
// ------------------------------------------------------------

#include "stdafx.h"
#include "Federation/FederationConfig.h"

using namespace std;
using namespace Common;
using namespace LeaseWrapper;
using namespace Hosting2;
using namespace Transport;
using namespace Federation;

namespace
{
    StringLiteral TraceType = "LeaseMonitor";
    bool disableIpcLeasePolling = false;
}

void LeaseMonitor::DisableIpcLeasePolling()
{
    disableIpcLeasePolling = true;
    WriteInfo(TraceType, "IPC disabled");
}

LeaseMonitor::LeaseMonitor(
    ComponentRoot & root,
    wstring const & traceId,
    Transport::IpcClient & ipcClient,
    HANDLE leaseHandle,
    int64 initialLeaseExpirationInTicks,
    Common::TimeSpan const leaseDuration,
    bool useDirectLease)
    : RootedObject(root)
    , traceId_(traceId)
    , leaseHandle_(leaseHandle)
    , ipcClient_(ipcClient)
    , expirationInTicks_(initialLeaseExpirationInTicks)
    , useDirectLease_(useDirectLease)
{
    leaseDuration; // ignored, because this is from a dynamic config, may change 
    CODING_ERROR_ASSERT(expirationInTicks_ >= 0);
    CODING_ERROR_ASSERT((leaseHandle_ != nullptr) && (leaseHandle_ != INVALID_HANDLE_VALUE));

    if (useDirectLease_ || disableIpcLeasePolling)
    {
        WriteInfo(
            TraceType, traceId_,
            "LeaseHandle={0}, LeaseDuration={1}, IPC disabled",
            (uint64)leaseHandle_,
            FederationConfig::GetConfig().LeaseDuration);
    }
    else
    {
        auto pollInterval = GetPollInterval();
        auto pollStartDelay = GetPollStartDelay(pollInterval);
        WriteInfo(
            TraceType, traceId_,
            "LeaseDuration={0}, pollInterval={1}, pollStartDelay={2}",
            FederationConfig::GetConfig().LeaseDuration, pollInterval, pollStartDelay);

        pollTimer_ = Timer::Create(
            "LeaseMonitor",
            [root = Root.CreateComponentRoot(), this](TimerSPtr const &) { PollLeaseExpiration(); });

        pollTimer_->Change(pollStartDelay, pollInterval); //LINUXTODO consider updating interval according to config change
    }
}

TimeSpan LeaseMonitor::GetPollInterval() const
{
    auto nodeLeaseRenewInterval =
        FederationConfig::GetConfig().LeaseDuration.TotalMilliseconds() / FederationConfig::GetConfig().LeaseRenewBeginRatio;

    //make it a bit larger than nodeLeaseRenewInterval so that most polls get back a new expiration
    auto pollInterval = TimeSpan::FromMilliseconds(nodeLeaseRenewInterval + 1000.0);
    Invariant(pollInterval > TimeSpan::Zero);
    return pollInterval;
}

TimeSpan LeaseMonitor::GetPollStartDelay(TimeSpan pollInterval) const
{
    //delay polling to give node lease renewal a headstart
    auto expiration = StopwatchTime(expirationInTicks_);
    if ((expiration - Stopwatch::Now()) < (pollInterval + pollInterval))
    {
        return TimeSpan::Zero;
    }

    return pollInterval;
}

void LeaseMonitor::Close()
{
    WriteInfo(TraceType, traceId_, "closing");
    expirationInTicks_ = -1; // mark close
    if (pollTimer_)
    {
        pollTimer_->Cancel();
    }
}

bool LeaseMonitor::IsLeaseExpired_DirectRead()
{
    StopwatchTime now = Stopwatch::Now();
    if (now.Ticks < expirationInTicks_)
    {
        return false;
    }

    if (expirationInTicks_ < 0) return true; //closed

    LONG ttlInMilliseconds = 0;
    LONGLONG kernelNow = 0;
    if (!LeaseAgent::GetLeasingApplicationExpirationTime(leaseHandle_, &ttlInMilliseconds, &kernelNow))
    {
        WriteWarning(
            TraceType,
            traceId_,
            "GetLeasingApplicationExpirationTime() failed with {0} on lease handle {1}",
            ::GetLastError(),
            (uint64)leaseHandle_);
        return true;
    }

    WriteNoise(TraceType, traceId_, "lease will be valid for {0}ms", ttlInMilliseconds);

    StopwatchTime newExpiration;
    if (MAXLONG == ttlInMilliseconds)
    {
        auto leaseDuration = FederationConfig::GetConfig().LeaseDuration;
        WriteNoise(TraceType, traceId_, "Lease driver returned MAXLONG TTL, we will use lease timeout {0}", leaseDuration);
        newExpiration = now + FederationConfig::GetConfig().LeaseDuration;
    }
    else
    {
        newExpiration = now + TimeSpan::FromMilliseconds(static_cast<double>(ttlInMilliseconds));
    }

    auto storedExpiration = expirationInTicks_;
    if (storedExpiration < newExpiration.Ticks) //ensure expirationInTicks_ never decrease
    {
        InterlockedCompareExchange64(&expirationInTicks_, newExpiration.Ticks, storedExpiration);
    }

    return now.Ticks >= expirationInTicks_;
}

void LeaseMonitor::OnLeaseRequestComplete(AsyncOperationSPtr const & operation, StopwatchTime requestTime)
{
    MessageUPtr reply;
    ErrorCode error = ipcClient_.EndRequest(operation, reply);
    if (!error.IsSuccess())
    {
        WriteWarning(TraceType, traceId_, "OnLeaseRequestComplete: request failed: {0}", error);
        return;
    }

    LeaseReply leaseReply;
    if (!reply->GetBody(leaseReply))
    {
        WriteError(TraceType, traceId_, "OnLeaseRequestComplete: failed to extract LeaseReply from reply {0}", reply->TraceId());
        return;
    }

    StopwatchTime newExpiration = requestTime + leaseReply.TTL(); //MAXLONG TTL is handled on node host side due to dependency on config
    WriteNoise(
        TraceType, traceId_,
        "OnLeaseRequestComplete: requestTime = {0}, ttl = {1}, new newExpiration = {2}",
        requestTime, leaseReply.TTL(), newExpiration);

    auto storedExpiration = expirationInTicks_;
    if (storedExpiration < 0)
    {
        WriteInfo(TraceType, traceId_, "already closed, ignore new expiration");
        return;
    }

    if (newExpiration.Ticks <= storedExpiration)
    {
        WriteInfo(TraceType, traceId_, "stored expiration {0} is newer, ignoring {1}", StopwatchTime(storedExpiration), newExpiration);
        return;
    }

    for (;;)
    {

        auto initialValue = InterlockedCompareExchange64(&expirationInTicks_, newExpiration.Ticks, storedExpiration);
        if (initialValue == storedExpiration) return;

        storedExpiration = initialValue;
        WriteInfo(TraceType, traceId_, "InterlockedCompareExchange64 needs to be retried");

        if (storedExpiration < 0)
        {
            WriteInfo(TraceType, traceId_, "already closed, ignore new expiration");
            return;
        }

        if (newExpiration.Ticks <= storedExpiration)
        {
            WriteNoise(TraceType, traceId_, "stored expiration {0} is newer, ignoring {1}", StopwatchTime(storedExpiration), newExpiration);
            return;
        }
    }
}

void LeaseMonitor::PollLeaseExpiration()
{
    WriteNoise(TraceType, traceId_, "polling...");
    auto request = make_unique<Message>();
    request->Headers.Add(ActorHeader(Actor::Hosting));
    ipcClient_.BeginRequest(
        move(request),
        FederationConfig::GetConfig().LeaseDuration,
        [this, requestTime = Stopwatch::Now()](AsyncOperationSPtr const & operation) { OnLeaseRequestComplete(operation, requestTime); },
        Root.CreateAsyncOperationRoot());
}

bool LeaseMonitor::IsLeaseExpired()
{
    if (useDirectLease_ || disableIpcLeasePolling)
    {
        return IsLeaseExpired_DirectRead();
    }

    StopwatchTime now = Stopwatch::Now();
    return expirationInTicks_ <= now.Ticks;
}


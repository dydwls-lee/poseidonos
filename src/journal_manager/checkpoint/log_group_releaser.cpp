/*
 *   BSD LICENSE
 *   Copyright (c) 2021 Samsung Electronics Corporation
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "log_group_releaser.h"

#include "src/journal_manager/log_buffer/journal_log_buffer.h"
#include "src/journal_manager/log_buffer/buffer_write_done_notifier.h"
#include "src/journal_manager/log_buffer/callback_sequence_controller.h"
#include "src/journal_manager/log_write/log_write_handler.h"
#include "src/journal_manager/checkpoint/checkpoint_handler.h"
#include "src/journal_manager/checkpoint/dirty_map_manager.h"
#include "src/include/pos_event_id.h"
#include "src/logger/logger.h"

namespace pos
{
// Constructor for the product code
LogGroupReleaser::LogGroupReleaser(void)
: LogGroupReleaser(new CheckpointHandler(this))
{
}

// Constructor for unit test
LogGroupReleaser::LogGroupReleaser(CheckpointHandler* checkpointHandler)
: releaseNotifier(nullptr),
  logBuffer(nullptr),
  dirtyPageManager(nullptr),
  sequenceController(nullptr),
  flushingLogGroupId(-1),
  checkpointTriggerInProgress(false),
  checkpointHandler(checkpointHandler)
{
}

LogGroupReleaser::~LogGroupReleaser(void)
{
    if (checkpointHandler != nullptr)
    {
        delete checkpointHandler;
    }
}

void
LogGroupReleaser::Init(LogBufferWriteDoneNotifier* released,
    JournalLogBuffer* buffer, DirtyMapManager* dirtyPage,
    CallbackSequenceController* sequencer,
    IMapFlush* mapFlush, IContextManager* contextManager)
{
    releaseNotifier = released;
    logBuffer = buffer;
    dirtyPageManager = dirtyPage;
    sequenceController = sequencer;

    checkpointHandler->Init(mapFlush, contextManager);
}

void
LogGroupReleaser::Reset(void)
{
    flushingLogGroupId = -1;
    fullLogGroup.clear();
}

void
LogGroupReleaser::AddToFullLogGroup(int groupId)
{
    _AddToFullLogGroupList(groupId);
    _FlushNextLogGroup();
}

void
LogGroupReleaser::_AddToFullLogGroupList(int groupId)
{
    std::unique_lock<std::mutex> lock(fullLogGroupLock);
    fullLogGroup.push_back(groupId);
}

void
LogGroupReleaser::_FlushNextLogGroup(void)
{
    if ((flushingLogGroupId == -1) && (_HasFullLogGroup()))
    {
        if (checkpointTriggerInProgress.exchange(true) == false)
        {
            _UpdateFlushingLogGroup();
            assert(flushingLogGroupId != -1);
            checkpointTriggerInProgress = false;

            StartCheckpoint();
        }
    }
}

bool
LogGroupReleaser::_HasFullLogGroup(void)
{
    std::unique_lock<std::mutex> lock(fullLogGroupLock);
    return (fullLogGroup.size() != 0);
}

int
LogGroupReleaser::StartCheckpoint(void)
{
    // TODO(huijeong.kim) Create event for this job, not to be called in front-end write path

    MapPageList dirtyPages = dirtyPageManager->GetDirtyList(flushingLogGroupId);
    POS_TRACE_DEBUG((int)POS_EVENT_ID::JOURNAL_CHECKPOINT_STARTED,
        "Checkpoint started for log group {}", flushingLogGroupId);

    sequenceController->GetCheckpointExecutionApproval();
    int ret = checkpointHandler->Start(dirtyPages);
    sequenceController->AllowCallbackExecution();

    if (ret != 0)
    {
        // TODO(huijeong.kim): Go to the fail mode - not to journal any more
    }
    return 0;
}

void
LogGroupReleaser::_UpdateFlushingLogGroup(void)
{
    flushingLogGroupId = _PopFullLogGroup();
    POS_TRACE_DEBUG((int)POS_EVENT_ID::JOURNAL_FLUSH_LOG_GROUP,
        "Flush next log group {}", flushingLogGroupId);
}

int
LogGroupReleaser::_PopFullLogGroup(void)
{
    std::unique_lock<std::mutex> lock(fullLogGroupLock);

    assert(fullLogGroup.size() != 0);
    int retLogGroup = fullLogGroup.front();
    fullLogGroup.pop_front();

    return retLogGroup;
}

void
LogGroupReleaser::CheckpointCompleted(void)
{
    assert(flushingLogGroupId != -1);

    int ret = logBuffer->AsyncReset(flushingLogGroupId,
        std::bind(&LogGroupReleaser::_LogGroupResetCompleted,
            this, std::placeholders::_1));

    if (ret != 0)
    {
        // TODO(huijeong.kim) add log
    }
}

int
LogGroupReleaser::GetNumFullLogGroups(void)
{
    if (flushingLogGroupId != -1)
    {
        return fullLogGroup.size() + 1;
    }
    else
    {
        return fullLogGroup.size();
    }
}

void
LogGroupReleaser::_LogGroupResetCompleted(int logGroupId)
{
    releaseNotifier->NotifyLogBufferReseted(logGroupId);

    _ResetFlushingLogGroup();
    _FlushNextLogGroup();
}

void
LogGroupReleaser::_ResetFlushingLogGroup(void)
{
    flushingLogGroupId = -1;
}

int
LogGroupReleaser::GetFlushingLogGroupId(void)
{
    return flushingLogGroupId;
}

std::list<int>
LogGroupReleaser::GetFullLogGroups(void)
{
    return fullLogGroup;
}

CheckpointStatus
LogGroupReleaser::GetStatus(void)
{
    return checkpointHandler->GetStatus();
}

} // namespace pos

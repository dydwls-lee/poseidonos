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

#include "src/volume/volume_meta_intf.h"

#include <rapidjson/document.h>
#include <string>
#include "src/metafs/include/metafs_service.h"
#include "src/helper/json_helper.h"
#include "src/include/pos_event_id.h"
#include "src/logger/logger.h"
#include "src/volume/volume.h"

namespace pos
{
int
VolumeMetaIntf::LoadVolumes(VolumeList& volList, std::string arrayName)
{
    std::string volFile = "vbr";
    uint32_t fileSize = 256 * 1024; // 256KB
    MetaFs* metaFs = MetaFsServiceSingleton::Instance()->GetMetaFs(arrayName);

    POS_EVENT_ID rc = metaFs->ctrl->CheckFileExist(volFile);
    if (POS_EVENT_ID::SUCCESS != rc)
    {
        return (int)POS_EVENT_ID::META_OPEN_FAIL;
    }

    int fd = 0;
    rc = metaFs->ctrl->Open(volFile, fd);
    if (POS_EVENT_ID::SUCCESS != rc)
    {
        POS_TRACE_ERROR((int)POS_EVENT_ID::META_OPEN_FAIL, "Fail to open volume meta");
        return (int)POS_EVENT_ID::META_OPEN_FAIL;
    }

    char* rBuf = (char*)malloc(fileSize);
    memset(rBuf, 0, fileSize);

    // for partial read: metaFsMgr.io.Read(fd, byteOffset, dataChunkSize, rBuf);
    rc = metaFs->io->Read(fd, rBuf);
    metaFs->ctrl->Close(fd);

    if (POS_EVENT_ID::SUCCESS != rc)
    {
        POS_TRACE_ERROR((int)POS_EVENT_ID::META_READ_FAIL, "Fail to read volume meta");
        free(rBuf);
        return (int)POS_EVENT_ID::META_READ_FAIL;
    }

    std::string contents = rBuf;
    if (contents != "")
    {
        try
        {
            rapidjson::Document doc;
            doc.Parse<0>(rBuf);
            if (doc.HasMember("volumes"))
            {
                for (rapidjson::SizeType i = 0; i < doc["volumes"].Size(); i++)
                {
                    int id = doc["volumes"][i]["id"].GetInt();
                    std::string name = doc["volumes"][i]["name"].GetString();
                    uint64_t total = doc["volumes"][i]["total"].GetUint64();
                    uint64_t maxiops = doc["volumes"][i]["maxiops"].GetUint64();
                    uint64_t maxbw = doc["volumes"][i]["maxbw"].GetUint64();
                    VolumeBase* volume = new Volume(arrayName, name, total, maxiops, maxbw);
                    volList.Add(volume, id);
                }
            }
        }
        catch (const std::exception& e)
        {
            POS_TRACE_ERROR((int)POS_EVENT_ID::META_CONTENT_BROKEN, "Volume meta broken {}", e.what());
            return (int)POS_EVENT_ID::META_CONTENT_BROKEN;
        }
    }

    free(rBuf);
    return (int)POS_EVENT_ID::SUCCESS;
}

int
VolumeMetaIntf::SaveVolumes(VolumeList& volList, std::string arrayName)
{
    std::string volFile = "vbr";
    uint32_t fileSize = 256 * 1024; // 256KB
    std::string contents = "";
    MetaFs* metaFs = MetaFsServiceSingleton::Instance()->GetMetaFs(arrayName);

    int vol_cnt = volList.Count();
    if (vol_cnt > 0)
    {
        JsonElement root("");
        JsonArray array("volumes");

        int idx = -1;
        while (true)
        {
            VolumeBase* vol = volList.Next(idx);
            if (vol == nullptr)
            {
                break;
            }
            if (vol->IsValid() == true)
            {
                JsonElement elem("");
                elem.SetAttribute(JsonAttribute("name", "\"" + vol->GetName() + "\""));
                elem.SetAttribute(JsonAttribute("id", std::to_string(vol->ID)));
                elem.SetAttribute(JsonAttribute("total", std::to_string(vol->TotalSize())));
                elem.SetAttribute(JsonAttribute("maxiops", std::to_string(vol->MaxIOPS())));
                elem.SetAttribute(JsonAttribute("maxbw", std::to_string(vol->MaxBW())));
                array.AddElement(elem);
            }
        }
        root.SetArray(array);
        contents = root.ToJson();
    }

    POS_EVENT_ID rc = metaFs->ctrl->CheckFileExist(volFile);
    if (POS_EVENT_ID::SUCCESS != rc)
    {
        rc = metaFs->ctrl->Create(volFile, fileSize);
        if (POS_EVENT_ID::SUCCESS != rc)
        {
            POS_TRACE_ERROR((int)POS_EVENT_ID::META_CREATE_FAIL, "Fail to create meta file");
            return (int)POS_EVENT_ID::META_CREATE_FAIL;
        }
    }

    int fd = 0;
    rc = metaFs->ctrl->Open(volFile, fd);
    if (POS_EVENT_ID::SUCCESS != rc)
    {
        POS_TRACE_ERROR((int)POS_EVENT_ID::META_OPEN_FAIL, "Fail to open meta file");
        return (int)POS_EVENT_ID::META_OPEN_FAIL;
    }

    uint32_t contentsSize = contents.size();
    if (contentsSize >= fileSize)
    {
        POS_TRACE_ERROR((int)POS_EVENT_ID::VOL_DATA_SIZE_TOO_BIG, "Volume meta write buffer overflows");
        return (int)POS_EVENT_ID::VOL_DATA_SIZE_TOO_BIG;
    }

    char* wBuf = (char*)malloc(fileSize);
    memset(wBuf, 0, fileSize);
    strncpy(wBuf, contents.c_str(), contentsSize);

    MetaFsReturnCode<POS_EVENT_ID> ioRC;
    ioRC = metaFs->io->Write(fd, wBuf);

    metaFs->ctrl->Close(fd);

    if (POS_EVENT_ID::SUCCESS != rc)
    {
        free(wBuf);
        POS_TRACE_ERROR((int)POS_EVENT_ID::META_WRITE_FAIL, "Fail to write volume meta");
        return (int)POS_EVENT_ID::META_WRITE_FAIL;
    }

    free(wBuf);
    POS_TRACE_DEBUG((int)POS_EVENT_ID::SUCCESS, "SaveVolumes succeed");
    return (int)POS_EVENT_ID::SUCCESS;
}

} // namespace pos

/**
 * Cloud storage plugins for Orthanc
 * Copyright (C) 2020-2023 Osimis S.A., Belgium
 * Copyright (C) 2024-2025 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2025 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/

#include "MoveStorageJob.h"
#include "Logging.h"
#include "Constants.h"
#include "Helpers.h"

namespace fs = boost::filesystem;

namespace OrthancPlugins
{

  MoveStorageJob::MoveStorageJob(const std::string& targetStorageId,
                                const std::vector<std::string>& instances,
                                const Json::Value& resourceForJobContent)
    : OrthancPlugins::OrthancJob(JOB_TYPE_MOVE_STORAGE),
      targetStorageId_(targetStorageId),
      instances_(instances),
      processedInstancesCount_(0),
      resourceForJobContent_(resourceForJobContent)
  {
    UpdateContent(resourceForJobContent);
    
    Json::Value serialized;
    Serialize(serialized);
    UpdateSerialized(serialized);
  }

  void MoveStorageJob::Serialize(Json::Value& target) const
  {
    target[KEY_CONTENT] = resourceForJobContent_;
    target[KEY_TARGET_STORAGE_ID] = targetStorageId_;
    target[KEY_INSTANCES] = Json::arrayValue;

    for (size_t i = 0; i < instances_.size(); ++i)
    {
      target[KEY_INSTANCES].append(instances_[i]);
    }

  }

  static bool MoveAttachment(const CustomData currentCustomData, const std::string& targetStorageId)
  {
    if (!currentCustomData.IsOwner())
    {
      LOG(WARNING) << "Unable to move attachment " << currentCustomData.GetUuid() << " because Orthanc is not owning the file";
      return false;
    }

    CustomData newCustomData = CustomData::CreateForMoveStorage(currentCustomData, targetStorageId);

    fs::path currentPath = currentCustomData.GetAbsolutePath();
    fs::path newPath = newCustomData.GetAbsolutePath();

    try
    {
      // Check if source file exists
      if (!fs::exists(currentPath)) 
      {
        LOG(ERROR) << "Unable to move attachment " << currentCustomData.GetUuid() << " because the file could not be found: " << currentPath;
        return false;
      }

      // Make sure the folders hierarchy exists
      if (fs::exists(newPath.parent_path()))
      {
        if (!fs::is_directory(newPath.parent_path()))
        {
          LOG(ERROR) << "Unable to move attachment " << currentCustomData.GetUuid() << " because the target directory already exists as a file: " << newPath.parent_path();
          return false;
        }
      }
      else
      {
        if (!fs::create_directories(newPath.parent_path()))
        {
          LOG(ERROR) << "Unable to move attachment " << currentCustomData.GetUuid() << ", unable to create the target directory: " << newPath.parent_path();
          return false;
        }
      }

      // Copy the file
      fs::copy_file(currentPath, newPath, fs::copy_option::fail_if_exists);
    } 
    catch (const fs::filesystem_error& e) 
    {
      LOG(ERROR) << "Unable to move attachment " << currentCustomData.GetUuid() << ": " << e.what();
      return false;
    }

    // Write the new customData in DB
    if (!UpdateAttachmentCustomData(currentCustomData.GetUuid(), newCustomData))
    {
      LOG(ERROR) << "Unable to update custom data for attachment " << currentCustomData.GetUuid() << ", deleting newly copied file";

      fs::remove(newPath);
      RemoveEmptyParentDirectories(newPath);
      return false;
    }

    // Delete the original file and its parent folders if they are empty now
    fs::remove(currentPath);
    RemoveEmptyParentDirectories(currentPath);
    
    return true;
  }

  static bool MoveInstance(const std::string& instanceId, const std::string& targetStorageId)
  {
    LOG(INFO) << "Moving instance to storage " << targetStorageId;

    Json::Value attachmentsList;
    OrthancPlugins::RestApiGet(attachmentsList, std::string("/instances/") + instanceId + "/attachments?full", false);

    Json::Value::Members attachmentsMembers = attachmentsList.getMemberNames();
    bool success = true;

    for (size_t i = 0; i < attachmentsMembers.size(); i++)
    {
      int attachmentId = attachmentsList[attachmentsMembers[i]].asInt();

      Json::Value attachmentInfo;
      OrthancPlugins::RestApiGet(attachmentInfo, std::string("/instances/") + instanceId + "/attachments/" + boost::lexical_cast<std::string>(attachmentId) + "/info", false);

      CustomData customData = OrthancPlugins::GetAttachmentCustomData(attachmentInfo["Uuid"].asString());
      success &= MoveAttachment(customData, targetStorageId);
    }

    return success;
  }

  OrthancPluginJobStepStatus MoveStorageJob::Step()
  {
    if (processedInstancesCount_ < instances_.size())
    {
      if (MoveInstance(instances_[processedInstancesCount_], targetStorageId_))
      {
        processedInstancesCount_++;
        UpdateProgress((float)processedInstancesCount_/(float)instances_.size());
        
        return OrthancPluginJobStepStatus_Continue;
      }
      else
      {
        return OrthancPluginJobStepStatus_Failure;
      }
    }

    return OrthancPluginJobStepStatus_Success;
  }

  void MoveStorageJob::Stop(OrthancPluginJobStopReason reason)
  {
  }
      
  void MoveStorageJob::Reset()
  {
    processedInstancesCount_ = 0;
  }
}
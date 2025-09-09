/**
 * Cloud storage plugins for Orthanc
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
#include <SystemToolbox.h>

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
    UpdateContent();
    
    Json::Value serialized;
    Serialize(serialized);
    UpdateSerialized(serialized);
  }

  void MoveStorageJob::UpdateContent()
  {
    Json::Value jobContent;
    jobContent[KEY_MOVE_STORAGE_JOB_RESOURCES] = resourceForJobContent_;
    jobContent[KEY_MOVE_STORAGE_JOB_TARGET_STORAGE_ID] = targetStorageId_;

    if (!errorDetails_.empty())
    {
      jobContent[KEY_MOVE_STORAGE_JOB_ERROR_DETAILS] = errorDetails_;
    }

    OrthancJob::UpdateContent(jobContent); 
  }


  void MoveStorageJob::Serialize(Json::Value& target) const
  {
    target[KEY_MOVE_STORAGE_JOB_RESOURCES] = resourceForJobContent_;
    target[KEY_TARGET_STORAGE_ID] = targetStorageId_;
    target[KEY_INSTANCES] = Json::arrayValue;

    for (size_t i = 0; i < instances_.size(); ++i)
    {
      target[KEY_INSTANCES].append(instances_[i]);
    }

  }

  bool MoveStorageJob::MoveAttachment(const CustomData& currentCustomData, const std::string& targetStorageId)
  {
    if (!currentCustomData.IsOwner())
    {
      errorDetails_= std::string("Unable to move attachment ") + currentCustomData.GetUuid() + " because Orthanc is not owning the file";
      UpdateContent();
      LOG(ERROR) << errorDetails_;
      return false;
    }

    if (!currentCustomData.IsRelativePath())
    {
      errorDetails_= std::string("Unable to move attachment ") + currentCustomData.GetUuid() + " because its path is an absolute path (the file has likely been adopted).  Try to reconstruct the resource to move the file to one of the Orthanc Storage";
      UpdateContent();
      LOG(ERROR) << errorDetails_;
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
        errorDetails_= std::string("Unable to move attachment ") + currentCustomData.GetUuid() + " because the file could not be found: " + Orthanc::SystemToolbox::PathToUtf8(currentPath);
        UpdateContent();
        LOG(ERROR) << errorDetails_;
        return false;
      }

      // Make sure the folders hierarchy exists
      if (fs::exists(newPath.parent_path()))
      {
        if (!fs::is_directory(newPath.parent_path()))
        {
          errorDetails_= std::string("Unable to move attachment ") + currentCustomData.GetUuid() + " because the target directory already exists as a file: " + Orthanc::SystemToolbox::PathToUtf8(newPath.parent_path());
          UpdateContent();
          LOG(ERROR) << errorDetails_;
          return false;
        }
      }
      else
      {
        if (!fs::create_directories(newPath.parent_path()))
        {
          errorDetails_= std::string("Unable to move attachment ") + currentCustomData.GetUuid() + " unable to create the target directory:: " + Orthanc::SystemToolbox::PathToUtf8(newPath.parent_path());
          UpdateContent();
          LOG(ERROR) << errorDetails_;
          return false;
        }
      }

      // Copy the file
#if BOOST_VERSION < 107400
      fs::copy_file(currentPath, newPath, fs::copy_option::fail_if_exists);
#else
      fs::copy_file(currentPath, newPath);
#endif
    } 
    catch (const fs::filesystem_error& e) 
    {
      if (e.code() == boost::system::errc::file_exists)
      {
        if (!Orthanc::SystemToolbox::CompareFilesMD5(currentPath, newPath))
        {
          errorDetails_= std::string("MoveAttachment: Destination file already exists and is different from current file: ") + Orthanc::SystemToolbox::PathToUtf8(newPath);
          UpdateContent();
          LOG(ERROR) << errorDetails_;
          return false;
        }
        // else, files are identical -> continue and write the customData in DB
      }
      else
      {
        errorDetails_= std::string("Unable to move attachment ") + currentCustomData.GetUuid() + ": " + e.what();
        UpdateContent();
        LOG(ERROR) << errorDetails_;
        return false;
      }
    }

    // Write the new customData in DB
    if (!UpdateAttachmentCustomData(currentCustomData.GetUuid(), newCustomData))
    {
      errorDetails_= std::string("Unable to update custom data for attachment ") + currentCustomData.GetUuid() + ", deleting newly copied file";
      UpdateContent();
      LOG(ERROR) << errorDetails_;

      fs::remove(newPath);
      RemoveEmptyParentDirectories(newPath);
      return false;
    }

    // Delete the original file and its parent folders if they are empty now
    fs::remove(currentPath);
    RemoveEmptyParentDirectories(currentPath);
    
    return true;
  }

  bool MoveStorageJob::MoveInstance(const std::string& instanceId, const std::string& targetStorageId)
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
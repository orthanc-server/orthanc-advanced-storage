/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2022 Osimis S.A., Belgium
 * Copyright (C) 2021-2022 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 **/

#include "Helpers.h"
#include "PathOwner.h"

#include <SystemToolbox.h>
#include <Toolbox.h>
#include <Logging.h>


namespace fs = boost::filesystem;

namespace OrthancPlugins
{
  static OrthancPlugins::KeyValueStore kvsAdoptedPath_("advst-adopted-path");


  CustomData GetAttachmentCustomData(const std::string& attachmentUuid)
  {
    OrthancPlugins::MemoryBuffer customDataBuffer;
    if (OrthancPluginGetAttachmentCustomData(OrthancPlugins::GetGlobalContext(),
                                             *customDataBuffer,
                                             attachmentUuid.c_str()) == OrthancPluginErrorCode_Success)
    {
      return CustomData::FromString(attachmentUuid, customDataBuffer.GetData(), customDataBuffer.GetSize());
    }

    throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, std::string("Could not retrieve custom data for attachment ") + attachmentUuid);
  }


  bool UpdateAttachmentCustomData(const std::string& attachmentUuid, const CustomData& customData)
  {
    std::string serializedCustomDataString;
    customData.ToString(serializedCustomDataString);

    return OrthancPluginSetAttachmentCustomData(OrthancPlugins::GetGlobalContext(),
                                                attachmentUuid.c_str(),
                                                serializedCustomDataString.c_str(),
                                                serializedCustomDataString.size()) == OrthancPluginErrorCode_Success;
  }

  void RemoveEmptyParentDirectories(const fs::path& path)
  {  
    // Remove the empty parent directories, (ignoring the error code if these directories are not empty)
    try
    {
      fs::path parent = path.parent_path();

      while (!CustomData::IsARootPath(parent))
      {
        fs::remove(parent);
        parent = parent.parent_path();
      }
    }
    catch (...)
    {
      // Ignore the error
    }
  }

  void AdoptFile(std::string& instanceId,
                 std::string& attachmentUuid,
                 OrthancPluginStoreStatus& storeStatus,
                 const std::string& path, 
                 bool takeOwnership)
  {
    CustomData cd = CustomData::CreateForAdoption(path, takeOwnership);
    
    std::string customDataString;
    cd.ToString(customDataString);

    if (static_cast<size_t>(static_cast<uint32_t>(customDataString.size())) != customDataString.size())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_NotEnoughMemory);
    }

    std::string fileContent;
    Orthanc::SystemToolbox::ReadFile(fileContent, path, true);

    OrthancPlugins::MemoryBuffer createdResourceIdBuffer;
    OrthancPlugins::MemoryBuffer attachmentUuidBuffer;

    OrthancPluginErrorCode res = OrthancPluginAdoptDicomInstance(
      OrthancPlugins::GetGlobalContext(),
      *createdResourceIdBuffer,
      *attachmentUuidBuffer,
      &storeStatus,
      fileContent.data(),
      fileContent.size(),
      customDataString.empty() ? NULL : customDataString.c_str(),
      customDataString.size());

    if (res == OrthancPluginErrorCode_Success)
    {
      createdResourceIdBuffer.ToString(instanceId);

      if (storeStatus == OrthancPluginStoreStatus_Success)
      {
        PathOwner owner = PathOwner::Create(instanceId, OrthancPluginResourceType_Instance, OrthancPluginContentType_Dicom);
        std::string serializedOwner;
        owner.ToString(serializedOwner);

        // also store the owner info in the key value store in case the file is abandoned later
        kvsAdoptedPath_.Store(path, serializedOwner);
        
        attachmentUuidBuffer.ToString(attachmentUuid);
      }
    }
    else
    {
      storeStatus = OrthancPluginStoreStatus_Failure;
    }
  }

  void AbandonFile(const std::string& path)
  {
    // find attachment uuid from path -> lookup in DB the Key-Value Store provided by Orthanc
    std::string serializedPathOwner;

    if (kvsAdoptedPath_.GetValue(serializedPathOwner, path))
    {
      PathOwner owner = PathOwner::FromString(serializedPathOwner);
      std::string urlToDelete;
      owner.GetUrlForDeletion(urlToDelete);

      kvsAdoptedPath_.DeleteKey(path);

      // trigger the deletion of this attachment
      LOG(INFO) << "Deleting resource " << urlToDelete << " for path " << path;
      OrthancPlugins::RestApiDelete(urlToDelete, true);
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "The path could not be found: " + path);
    }
  }

  void MarkAdoptedFileAsDeleted(const std::string& path)
  {
    kvsAdoptedPath_.DeleteKey(path);
  }
}

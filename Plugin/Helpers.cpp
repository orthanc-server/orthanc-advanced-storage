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
		                                         attachmentUuid.c_str(),
																						 *customDataBuffer) == OrthancPluginErrorCode_Success)
		{
			return CustomData::FromString(attachmentUuid, customDataBuffer.GetData(), customDataBuffer.GetSize());
		}

		throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, std::string("Could not retrieve custom data for attachment ") + attachmentUuid);
	}

  bool UpdateAttachmentCustomData(const std::string& attachmentUuid, const CustomData& customData)
  {
    std::string seriliazedCustomDataString;
    customData.ToString(seriliazedCustomDataString);

    return OrthancPluginUpdateAttachmentCustomData(OrthancPlugins::GetGlobalContext(),
                                                   attachmentUuid.c_str(),
                                                   seriliazedCustomDataString.c_str(),
                                                   seriliazedCustomDataString.size()) == OrthancPluginErrorCode_Success;
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

    std::string fileContent;
    Orthanc::SystemToolbox::ReadFile(fileContent, path, true);
    std::string fileContentMD5;
    Orthanc::Toolbox::ComputeMD5(fileContentMD5, fileContent);

    OrthancPluginAttachment2 fileInfo;
    fileInfo.compressedHash = fileContentMD5.c_str();
    fileInfo.uncompressedHash = fileContentMD5.c_str();
    fileInfo.uncompressedSize = fileContent.size();
    fileInfo.compressedSize = fileContent.size();
    fileInfo.contentType = OrthancPluginContentType_Dicom;
    fileInfo.uuid = NULL;
    fileInfo.compressionType = OrthancPluginCompressionType_None;
    fileInfo.customData = customDataString.c_str();
    fileInfo.customDataSize = customDataString.size();

    OrthancPlugins::MemoryBuffer createdResourceIdBuffer;
    OrthancPlugins::MemoryBuffer attachmentUuidBuffer;

    OrthancPluginErrorCode res = OrthancPluginAdoptAttachment(OrthancPlugins::GetGlobalContext(),
                                                              fileContent.data(),
                                                              fileContent.size(),
                                                              &fileInfo,
                                                              OrthancPluginResourceType_None,
                                                              NULL,
                                                              *createdResourceIdBuffer,
                                                              *attachmentUuidBuffer,
                                                              &storeStatus
                                                              );

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

    if (kvsAdoptedPath_.Get(serializedPathOwner, path))
    {
      PathOwner owner = PathOwner::FromString(serializedPathOwner);
      std::string urlToDelete;
      owner.GetUrlForDeletion(urlToDelete);

      kvsAdoptedPath_.Delete(path);

      // trigger the deletion of this attachment
      LOG(INFO) << "Deleting attachment " << urlToDelete << " for path " << path;
      OrthancPlugins::RestApiDelete(urlToDelete, true);
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "The path could not be found: " + path);
    }
  }

}
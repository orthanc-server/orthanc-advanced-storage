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

#define ORTHANC_PLUGIN_NAME "advanced-storage"

#include "../Resources/Orthanc/Plugins/OrthancPluginCppWrapper.h"

#include <Compatibility.h>
#include <OrthancException.h>
#include <Logging.h>
#include <SystemToolbox.h>
#include <Toolbox.h>
#include <DicomFormat/DicomInstanceHasher.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>

#include <json/value.h>
#include <json/writer.h>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <map>
#include <list>
#include <time.h>

#include "CustomData.h"
#include "PathGenerator.h"
#include "PathOwner.h"

namespace fs = boost::filesystem;

bool fsyncOnWrite_ = true;
size_t legacyPathLength = 39; // ex "/00/f7/00f7fd8b-47bd8c3a-ff917804-d180cdbc-40cf9527"

using namespace OrthancPlugins;
static const char* PLUGIN_ID_ADOPTED_PATH = "advst-adopted-path";

static const char* const SYSTEM_CAPABILITIES = "Capabilities";
static const char* const SYSTEM_CAPABILITIES_HAS_KEY_VALUE_STORE = "HasKeyValueStore";
static const char* const READ_ONLY = "ReadOnly";
bool isReadOnly_ = false;
bool hasKeyValueStore_ = false;


OrthancPluginErrorCode StorageCreate(OrthancPluginMemoryBuffer* customData,
                                     const char* uuid,
                                     const void* content,
                                     uint64_t size,
                                     OrthancPluginContentType type,
                                     OrthancPluginCompressionType compressionType,
                                     const OrthancPluginDicomInstance* dicomInstance)
{
  try
  {
    Json::Value tags;

    if (dicomInstance != NULL)
    {
      OrthancPlugins::DicomInstance dicom(dicomInstance);
      dicom.GetSimplifiedJson(tags);
    }

    const bool isCompressed = (compressionType != OrthancPluginCompressionType_None);

    boost::filesystem::path relativePath;
    if (!PathGenerator::IsDefaultNamingScheme())
    {
      relativePath = PathGenerator::GetRelativePathFromTags(tags, uuid, type, isCompressed);
    }
    
    CustomData cd = CustomData::CreateForWriting(uuid, relativePath);

    boost::filesystem::path absolutePath = cd.GetAbsolutePath(); //ForWriting()
    if (fs::exists(absolutePath))
    {
      // Extremely unlikely case if uuid is included in the path: This Uuid has already been created
      // in the past.
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "Advanced Storage - path already exists");

      // TODO for the future: handle duplicates path (e.g: there's no uuid in the path and we are uploading the same file again)
    }

    std::string seriliazedCustomDataString;
    cd.ToString(seriliazedCustomDataString);

    LOG(INFO) << "Advanced Storage - creating attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + absolutePath.string() + ")";

    if (fs::exists(absolutePath.parent_path()))
    {
      if (!fs::is_directory(absolutePath.parent_path()))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_DirectoryOverFile);
      }
    }
    else
    {
      if (!fs::create_directories(absolutePath.parent_path()))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_FileStorageCannotWrite);
      }
    }

    Orthanc::SystemToolbox::WriteFile(content, size, absolutePath.string(), fsyncOnWrite_);

    OrthancPluginCreateMemoryBuffer(OrthancPlugins::GetGlobalContext(), customData, seriliazedCustomDataString.size());
    memcpy(customData->data, seriliazedCustomDataString.data(), seriliazedCustomDataString.size());
    return OrthancPluginErrorCode_Success;
  }
  catch (Orthanc::OrthancException& e)
  {
    return static_cast<OrthancPluginErrorCode>(e.GetErrorCode());
  }
  catch (...)
  {
    return OrthancPluginErrorCode_StorageAreaPlugin;
  }
}


OrthancPluginErrorCode StorageReadRange(OrthancPluginMemoryBuffer64* target,
                                        const char* uuid,
                                        OrthancPluginContentType type,
                                        uint64_t rangeStart,
                                        const void* customData,
                                        uint64_t customDataSize)
{
  CustomData cd = CustomData::FromString(uuid, customData, customDataSize);
  std::string path = cd.GetAbsolutePath().string();

  LOG(INFO) << "Advanced Storage - Reading range of attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + path + ")";

  if (!Orthanc::SystemToolbox::IsRegularFile(path))
  {
    LOG(ERROR) << "The path does not point to a regular file: " << path;
    return OrthancPluginErrorCode_InexistentFile;
  }

  try
  {
    fs::ifstream f;
    f.open(path, std::ifstream::in | std::ifstream::binary);
    if (!f.good())
    {
      LOG(ERROR) << "The path does not point to a regular file: " << path;
      return OrthancPluginErrorCode_InexistentFile;
    }

    f.seekg(rangeStart, std::ios::beg);

    // The ReadRange uses a target that has already been allocated by orthanc
    f.read(reinterpret_cast<char*>(target->data), target->size);

    f.close();
  }
  catch (...)
  {
    LOG(ERROR) << "Unexpected error while reading: " << path;
    return OrthancPluginErrorCode_StorageAreaPlugin;
  }

  return OrthancPluginErrorCode_Success;
}


OrthancPluginErrorCode StorageRemove(const char* uuid,
                                     OrthancPluginContentType type,
                                     const void* customData,
                                     uint64_t customDataSize)
{
  CustomData cd = CustomData::FromString(uuid, customData, customDataSize);
  boost::filesystem::path path = cd.GetAbsolutePath();

  if (!cd.IsOwner())
  {
    LOG(INFO) << "Advanced Storage - NOT deleting attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + path.string() + ") since the file has been adopted.";
  }
  else
  {
    LOG(INFO) << "Advanced Storage - Deleting attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + path.string() + ")";

    try
    {
      fs::remove(path);
    }
    catch (...)
    {
      // Ignore the error
    }

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

  return OrthancPluginErrorCode_Success;
}

static OrthancPluginErrorCode OnChangeCallback(OrthancPluginChangeType changeType, 
                                              OrthancPluginResourceType resourceType, 
                                              const char *resourceId)
{
  try
  {
    switch (changeType)
    {
      case OrthancPluginChangeType_OrthancStarted:
      {
        Json::Value system;
        if (OrthancPlugins::RestApiGet(system, "/system", false))
        {
          hasKeyValueStore_ = system.isMember(SYSTEM_CAPABILITIES) 
                              && system[SYSTEM_CAPABILITIES].isMember(SYSTEM_CAPABILITIES_HAS_KEY_VALUE_STORE)
                              && system[SYSTEM_CAPABILITIES][SYSTEM_CAPABILITIES_HAS_KEY_VALUE_STORE].asBool();
          if (hasKeyValueStore_)
          {
            LOG(INFO) << "Orthanc supports KeyValueStore.";
          }
          else
          {
            LOG(WARNING) << "Orthanc does not support KeyValueStore.  The plugin will not be able to adopt files and the indexer mode will not be available";
          }

          isReadOnly_ = system.isMember(READ_ONLY) && system[READ_ONLY].asBool();

          if (isReadOnly_)
          {
            LOG(WARNING) << "Orthanc is ReadOnly.  The plugin will not be able to adopt files and the indexer mode will not be available";
          }
        }

      }; break;
      default:
        break;
    }
  }
  catch (Orthanc::OrthancException& e)
  {
    LOG(ERROR) << "Exception: " << e.What();
  }
  catch (...)
  {
    LOG(ERROR) << "Uncatched native exception";
  }  

  return OrthancPluginErrorCode_Success;
}

extern "C"
{

  OrthancPluginErrorCode PostAdoptInstance(OrthancPluginRestOutput* output,
                                           const char* url,
                                           const OrthancPluginHttpRequest* request)
  {
    if (request->method != OrthancPluginHttpMethod_Post)
    {
      OrthancPlugins::AnswerMethodNotAllowed(output, "POST");
    }
    else
    {
      Json::Value body;

      if (!OrthancPlugins::ReadJson(body, request->body, request->bodySize))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "A JSON payload was expected");
      }

      fs::path path;
      if (!body.isMember("Path") || !body["Path"].isString())
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "'Path' field is missing or not a string");
      }
      
      path = body["Path"].asString();

      // uint64_t pixelDataOffset = 0;
      // if (body.isMember("PixelDataOffset") && body["PixelDataOffset"].isUInt64()) // TODO_ATTACH_CUSTOM_DATA: temporary code for testing: the plugin should read the file to compute it
      // {
      //   pixelDataOffset = body["PixelDataOffset"].asUInt64();
      // }

      CustomData cd = CustomData::CreateForAdoption(path);
      std::string customDataString;
      cd.ToString(customDataString);

      std::string fileContent;
      Orthanc::SystemToolbox::ReadFile(fileContent, path.string(), true);
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
      OrthancPluginStoreStatus storeStatus;

      OrthancPluginErrorCode res = OrthancPluginAdoptAttachment(OrthancPlugins::GetGlobalContext(),
                                                                fileContent.data(),
                                                                fileContent.size(), // TODO_ATTACH_CUSTOM_DATA ?pixelDataOffset or fileContent.size(),
                                                                // TODO_ATTACH_CUSTOM_DATA pixelDataOffset,
                                                                &fileInfo,
                                                                OrthancPluginResourceType_None,
                                                                NULL,
                                                                *createdResourceIdBuffer,
                                                                *attachmentUuidBuffer,
                                                                &storeStatus
                                                                );

      if (res == OrthancPluginErrorCode_Success)
      {
        std::string createdResourceId, attachmentUuid;

        createdResourceIdBuffer.ToString(createdResourceId);

        Json::Value response;
        response["InstanceId"] = createdResourceId;

        if (storeStatus == OrthancPluginStoreStatus_Success)
        {
          PathOwner owner = PathOwner::Create(createdResourceId, OrthancPluginResourceType_Instance, OrthancPluginContentType_Dicom);
          std::string serializedOwner;
          owner.ToString(serializedOwner);

          // also store the owner info in the key value store in case the file is abandoned later
          OrthancPluginStoreKeyValue(OrthancPlugins::GetGlobalContext(),
                                    PLUGIN_ID_ADOPTED_PATH,
                                    path.string().c_str(),
                                    serializedOwner.c_str(),
                                    serializedOwner.size());

          attachmentUuidBuffer.ToString(attachmentUuid);
          response["AttachmentUuid"] = attachmentUuid;
          response["Status"] = "Success";
        }
        else if (storeStatus == OrthancPluginStoreStatus_AlreadyStored)
        {
          response["Status"] = "AlreadyStored";
        }
        else if (storeStatus == OrthancPluginStoreStatus_Failure)
        {
          response["Status"] = "Failure";
        }
        else if (storeStatus == OrthancPluginStoreStatus_FilteredOut)
        {
          response["Status"] = "FilteredOut";
        }
        else if (storeStatus == OrthancPluginStoreStatus_StorageFull)
        {
          response["Status"] = "StorageFull";
        }
        else
        {
          response["Status"] = "Unknown";
        }

        OrthancPlugins::AnswerJson(response, output);
      }
      else
      {
        // TODO
      }
    }

    return OrthancPluginErrorCode_Success;
  }

  OrthancPluginErrorCode PostAbandonInstance(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request)
  {
    if (request->method != OrthancPluginHttpMethod_Post)
    {
      OrthancPlugins::AnswerMethodNotAllowed(output, "POST");
    }
    else
    {
      Json::Value body;

      if (!OrthancPlugins::ReadJson(body, request->body, request->bodySize))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "A JSON payload was expected");
      }

      fs::path path;
      if (!body.isMember("Path") || !body["Path"].isString())
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "'Path' field is missing or not a string");
      }
      
      path = body["Path"].asString();

      // find attachment uuid from path -> lookup in DB the Key-Value Store provided by Orthanc
      OrthancPlugins::MemoryBuffer attachmentOwnerBuffer;

      OrthancPluginErrorCode ret = OrthancPluginGetKeyValue(OrthancPlugins::GetGlobalContext(),
                                                            PLUGIN_ID_ADOPTED_PATH,
                                                            path.string().c_str(),
                                                            *attachmentOwnerBuffer);

      if (ret == OrthancPluginErrorCode_Success)
      {
        PathOwner owner = PathOwner::FromString(attachmentOwnerBuffer.GetData(), attachmentOwnerBuffer.GetSize());
        std::string urlToDelete;
        owner.GetUrlForDeletion(urlToDelete);

        OrthancPluginDeleteKeyValue(OrthancPlugins::GetGlobalContext(),
                                    PLUGIN_ID_ADOPTED_PATH,
                                    path.string().c_str());

        // trigger the deletion of this attachment
        LOG(INFO) << "Deleting attachment " << urlToDelete << " for path " << path.string();
        OrthancPlugins::RestApiDelete(urlToDelete, true);

        // The StorageRemove will know if it needs to delete the file or not (depending on the ownership)
        OrthancPlugins::AnswerHttpError(200, output);
      }
      else if (ret == OrthancPluginErrorCode_UnknownResource)
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource, "The path could not be found: " + path.string());
      }


    }

    return OrthancPluginErrorCode_Success;
  }

  OrthancPluginErrorCode GetAttachmentInfo(OrthancPluginRestOutput* output,
                                           const char* url,
                                           const OrthancPluginHttpRequest* request)
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPlugins::AnswerMethodNotAllowed(output, "GET");
    }
    else
    {
      Json::Value response;

      OrthancPlugins::RestApiClient coreApi(url, request);
      // TODO: we need to be able to retrieve the Attachment info from DB including CustomData in order to show the path

      if (coreApi.Execute() && coreApi.GetAnswerJson(response) && response.isArray())
      {
        response.append("path");
      }

      OrthancPlugins::AnswerJson(response, output);
      LOG(INFO) << request->groups[0];
      LOG(INFO) << request->groups[1];
      LOG(INFO) << request->groups[2];
    }

    return OrthancPluginErrorCode_Success;
  }

  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    OrthancPlugins::SetGlobalContext(context, ORTHANC_PLUGIN_NAME);
    Orthanc::Logging::InitializePluginContext(context, ORTHANC_PLUGIN_NAME);

    /* Check the version of the Orthanc core */
    if (OrthancPluginCheckVersion(context) == 0)
    {
      OrthancPlugins::ReportMinimalOrthancVersion(ORTHANC_PLUGINS_MINIMAL_MAJOR_NUMBER,
                                                  ORTHANC_PLUGINS_MINIMAL_MINOR_NUMBER,
                                                  ORTHANC_PLUGINS_MINIMAL_REVISION_NUMBER);
      return -1;
    }

    LOG(WARNING) << "AdvancedStorage plugin is initializing";
    OrthancPluginSetDescription2(context, ORTHANC_PLUGIN_NAME, "Provides alternative layout for your storage.");

    OrthancPlugins::OrthancConfiguration orthancConfiguration;

    OrthancPlugins::OrthancConfiguration advancedStorage;
    orthancConfiguration.GetSection(advancedStorage, "AdvancedStorage");

    bool enabled = advancedStorage.GetBooleanValue("Enable", false);
    if (enabled)
    {
      fsyncOnWrite_ = orthancConfiguration.GetBooleanValue("SyncStorageArea", true);

      const Json::Value& pluginJson = advancedStorage.GetJson();

      try
      {
        PathGenerator::SetNamingScheme(advancedStorage.GetStringValue("NamingScheme", "OrthancDefault"));
      }
      catch (Orthanc::OrthancException& ex)
      {
        return -1;
      }

      std::string otherAttachmentsPrefix = advancedStorage.GetStringValue("OtherAttachmentsPrefix", "");
      LOG(WARNING) << "AdvancedStorage - Prefix path to the other attachments root: " << otherAttachmentsPrefix;
      CustomData::SetOtherAttachmentsPrefix(otherAttachmentsPrefix);
      
      // if we have enabled multiple storage after files have been saved without this plugin, we still need the default StorageDirectory
      CustomData::SetOrthancCoreRootPath(orthancConfiguration.GetStringValue("StorageDirectory", "OrthancStorage"));
      LOG(WARNING) << "AdvancedStorage - Path to the default storage area: " << CustomData::GetOrthancCoreRootPath();

      size_t maxPathLength = advancedStorage.GetIntegerValue("MaxPathLength", 256);
      LOG(WARNING) << "AdvancedStorage - Maximum path length: " << maxPathLength;
      CustomData::SetMaxPathLength(maxPathLength);

      // if (!rootPath_.is_absolute())
      // {
      //   LOG(WARNING) << "AdvancedStorage - Path to the default storage is not an absolute path " << rootPath_ << " (\"StorageDirectory\" in main Orthanc configuration), computing absolute path";
      //   rootPath_ = fs::absolute(rootPath_);
      //   LOG(WARNING) << "AdvancedStorage - Absolute path to the default storage is now " << rootPath_;
      // }

      // if (rootPath_.size() > (maxPathLength_ - legacyPathLength))
      // {
      //   LOG(ERROR) << "AdvancedStorage - Path to the default storage is too long";
      //   return -1;
      // }

      if (pluginJson.isMember("MultipleStorages"))
      {
        // multipleStoragesEnabled_ = true;
        const Json::Value& multipleStoragesJson = pluginJson["MultipleStorages"];
        
        if (multipleStoragesJson.isMember("Storages") && multipleStoragesJson.isObject())
        {
          const Json::Value& storagesJson = multipleStoragesJson["Storages"];
          Json::Value::Members storageIds = storagesJson.getMemberNames();
    
          for (Json::Value::Members::const_iterator it = storageIds.begin(); it != storageIds.end(); ++it)
          {
            if (!storagesJson[*it].isString())
            {
              LOG(ERROR) << "AdvancedStorage - Storage path is not a string " << *it;
              return -1;
            }
            fs::path storagePath = storagesJson[*it].asString();

            // if (!storagePath.is_absolute())
            // {
            //   LOG(WARNING) << "AdvancedStorage - Storage path is not an absolute path " << storagePath << ", computing absolute path";
            //   storagePath = fs::absolute(storagePath);
            //   LOG(WARNING) << "AdvancedStorage - Absolute path to the default storage is now " << storagePath;
            // }

            // if (storagePath.size() > (maxPathLength_ - legacyPathLength))
            // {
            //   LOG(ERROR) << "AdvancedStorage - Storage path is too long '" << storagePath << "'";
            //   return -1;
            // }

            CustomData::SetStorageRootPath(*it, storagesJson[*it].asString());
            // rootPaths_[*it] = storagePath;
          }

          if (multipleStoragesJson.isMember("CurrentWriteStorage") && multipleStoragesJson["CurrentWriteStorage"].isString())
          {
            try
            {
              CustomData::SetCurrentWriteStorageId(multipleStoragesJson["CurrentWriteStorage"].asString());
            }
            catch (Orthanc::OrthancException& ex)
            {
              return -1;
            }
          }

          LOG(WARNING) << "AdvancedStorage - multiple storages enabled.  Current Write storage : " << multipleStoragesJson["CurrentWriteStorage"].asString();
        }
      }

      OrthancPluginRegisterStorageArea3(context, StorageCreate, StorageReadRange, StorageRemove);

      OrthancPluginRegisterRestCallback(context, "/(studies|series|instances|patients)/([^/]+)/attachments/(.*)/info", GetAttachmentInfo);

      {
        OrthancPluginRegisterRestCallback(context, "/plugins/advanced-storage/adopt-instance", PostAdoptInstance);
        OrthancPluginRegisterRestCallback(context, "/plugins/advanced-storage/abandon-instance", PostAbandonInstance);
        OrthancPluginRegisterOnChangeCallback(context, OnChangeCallback);
      }
    }
    else
    {
      LOG(WARNING) << "AdvancedStorage plugin is disabled by the configuration file";
    }

    return 0;
  }


  ORTHANC_PLUGINS_API void OrthancPluginFinalize()
  {
    LOG(WARNING) << "AdvancedStorage plugin is finalizing";
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetName()
  {
    return ORTHANC_PLUGIN_NAME;
  }


  ORTHANC_PLUGINS_API const char* OrthancPluginGetVersion()
  {
    return ADVANCED_STORAGE_VERSION;
  }
}

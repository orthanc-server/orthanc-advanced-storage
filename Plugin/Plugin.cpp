/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2012-2016 Sebastien Jodogne, Medical Physics
 * Department, University Hospital of Liege, Belgium
 * Copyright (C) 2017-2023 Osimis S.A., Belgium
 * Copyright (C) 2024-2026 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2026 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
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
#include "MoveStorageJob.h"
#include "Constants.h"
#include "Helpers.h"
#include "FoldersIndexer.h"
#include "DelayedFilesDeleter.h"

namespace fs = boost::filesystem;

bool fsyncOnWrite_ = true;
bool overwriteInstances_ = false;
size_t legacyPathLength = 39; // ex "/00/f7/00f7fd8b-47bd8c3a-ff917804-d180cdbc-40cf9527"

using namespace OrthancPlugins;


static const char* const SYSTEM_CAPABILITIES = "Capabilities";
static const char* const SYSTEM_CAPABILITIES_HAS_KEY_VALUE_STORE = "HasKeyValueStores";
static const char* const SYSTEM_CAPABILITIES_HAS_QUEUES = "HasQueues";
static const char* const READ_ONLY = "ReadOnly";

static const char* const CONFIG_SYNC_STORAGE_AREA = "SyncStorageArea";
static const char* const CONFIG_OVERWRITE_INSTANCES = "OverwriteInstances";
static const char* const CONFIG_STORAGE_DIRECTORY = "StorageDirectory";
static const char* const CONFIG_ENABLE = "Enable";
static const char* const CONFIG_NAMING_SCHEME = "NamingScheme";
static const char* const CONFIG_MAX_PATH_LENGTH = "MaxPathLength";
static const char* const CONFIG_OTHER_ATTACHMENTS_PREFIX = "OtherAttachmentsPrefix";
static const char* const CONFIG_MULTIPLE_STORAGES = "MultipleStorages";
static const char* const CONFIG_MULTIPLE_STORAGES_STORAGES = "Storages";
static const char* const CONFIG_MULTIPLE_STORAGES_CURRENT_WRITE_STORAGE = "CurrentWriteStorage";
static const char* const CONFIG_INDEXER = "Indexer";
static const char* const CONFIG_INDEXER_ENABLE = "Enable";
static const char* const CONFIG_INDEXER_FOLDERS = "Folders";
static const char* const CONFIG_INDEXER_INTERVAL = "Interval";
static const char* const CONFIG_INDEXER_THROTTLE_DELAY_MS = "ThrottleDelayMs";
static const char* const CONFIG_INDEXER_PARSED_EXTENSIONS = "ParsedExtensions";
static const char* const CONFIG_INDEXER_SKIPPED_EXTENSIONS = "SkippedExtensions";
static const char* const CONFIG_INDEXER_TAKE_OWNERSHIP = "TakeOwnership";
static const char* const CONFIG_DELAYED_DELETION = "DelayedDeletion";
static const char* const CONFIG_DELAYED_DELETION_ENABLE = "Enable";
static const char* const CONFIG_DELAYED_DELETION_THROTTLE_DELAY_MS = "ThrottleDelayMs";

static const char* const PLUGIN_STATUS_DELAYED_DELETION_ACTIVE = "DelayedDeletionIsActive";
static const char* const PLUGIN_STATUS_DELAYED_DELETION_PENDING_FILES = "FilesPendingDeletion";
static const char* const PLUGIN_STATUS_INDEXER_ACTIVE = "IndexerIsActive";

bool isReadOnly_ = false;
bool hasKeyValueStoresSupport_ = false;
bool hasQueuesSupport_ = false;

boost::mutex mutex_;
std::unique_ptr<FoldersIndexer> foldersIndexer_;
std::unique_ptr<DelayedFilesDeleter> delayedFilesDeleter_;


OrthancPluginErrorCode StorageCreate(OrthancPluginMemoryBuffer* customData,
                                     const char* uuid,
                                     const void* content,
                                     uint64_t size,
                                     OrthancPluginContentType type,
                                     OrthancPluginCompressionType compressionType,
                                     const OrthancPluginDicomInstance* dicomInstance) noexcept
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

    LOG(INFO) << "Advanced Storage - creating attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + Orthanc::SystemToolbox::PathToUtf8(absolutePath) + ")";

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

    Orthanc::SystemToolbox::WriteFile(content, size, absolutePath, fsyncOnWrite_);

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
                                        uint32_t customDataSize) noexcept
{
  CustomData cd = CustomData::FromString(uuid, customData, customDataSize);
  boost::filesystem::path path = cd.GetAbsolutePath();

  LOG(INFO) << "Advanced Storage - Reading range of attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + Orthanc::SystemToolbox::PathToUtf8(path) + ")";

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
                                     uint32_t customDataSize) noexcept
{
  CustomData cd = CustomData::FromString(uuid, customData, customDataSize);
  boost::filesystem::path path = cd.GetAbsolutePath();
  std::string pathUtf8Str = Orthanc::SystemToolbox::PathToUtf8(path);

  if (!cd.IsOwner())
  {
    LOG(INFO) << "NOT deleting attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + pathUtf8Str + ") since the file has been adopted.";

    // remove it from the adopted paths
    MarkAdoptedFileAsDeleted(pathUtf8Str);

    // notify the indexer that the file has been deleted (if it has been indexed by the indexer)
    if (foldersIndexer_.get() != NULL)
    {
      foldersIndexer_->MarkAsDeletedByOrthanc(pathUtf8Str);
    }
  }
  else
  {
    if (!cd.IsRelativePath()) // the file has been adopted and is now owned by Orthanc
    {
      // remove it from the adopted paths
      MarkAdoptedFileAsDeleted(pathUtf8Str);

      {
        boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and/or delayedDeletion pointer

        // notify the indexer that the file has been deleted (if it has been indexed by the indexer)
        if (foldersIndexer_.get() != NULL)
        {
          foldersIndexer_->MarkAsDeletedByOrthanc(pathUtf8Str);
        }
      }
    }

    try
    {
      {
        boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and/or delayedDeletion pointer

        if (delayedFilesDeleter_.get() != NULL)
        {
          LOG(INFO) << "Scheduling later deletion of attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + pathUtf8Str + ")";
          delayedFilesDeleter_->ScheduleFileDeletion(pathUtf8Str);
          return OrthancPluginErrorCode_Success;
        }
      }

      LOG(INFO) << "Deleting attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + pathUtf8Str + ")";

      fs::remove(path);

      // Remove the empty parent directories, (ignoring the error code if these directories are not empty)
      RemoveEmptyParentDirectories(path);
    }
    catch (...)
    {
      // Ignore the error
    }

  }

  return OrthancPluginErrorCode_Success;
}

extern "C" {
  OrthancPluginErrorCode PostAbandonInstance(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request) noexcept;

  OrthancPluginErrorCode PostAdoptInstance(OrthancPluginRestOutput* output,
                                           const char* url,
                                           const OrthancPluginHttpRequest* request) noexcept;

  OrthancPluginErrorCode PostMoveStorage(OrthancPluginRestOutput* output,
                                         const char* /*url*/,
                                         const OrthancPluginHttpRequest* request) noexcept;
}

static OrthancPluginErrorCode OnChangeCallback(OrthancPluginChangeType changeType, 
                                               OrthancPluginResourceType resourceType,
                                               const char *resourceId) noexcept
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
          boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and delayedDeletion pointer

          hasKeyValueStoresSupport_ = system.isMember(SYSTEM_CAPABILITIES) 
            && system[SYSTEM_CAPABILITIES].isMember(SYSTEM_CAPABILITIES_HAS_KEY_VALUE_STORE)
            && system[SYSTEM_CAPABILITIES][SYSTEM_CAPABILITIES_HAS_KEY_VALUE_STORE].asBool();
          
          if (hasKeyValueStoresSupport_)
          {
            LOG(INFO) << "Orthanc supports KeyValueStore.";

            OrthancPluginRegisterRestCallback(OrthancPlugins::GetGlobalContext(), "/plugins/advanced-storage/adopt-instance", PostAdoptInstance);
            OrthancPluginRegisterRestCallback(OrthancPlugins::GetGlobalContext(), "/plugins/advanced-storage/abandon-instance", PostAbandonInstance);

            if (foldersIndexer_.get() != NULL)
            {
              LOG(INFO) << "Starting Folders Indexer";
              foldersIndexer_->Start();
            }
          }
          else
          {
            LOG(WARNING) << "Orthanc does not support KeyValueStore.  The plugin will not be able to adopt files and the indexer mode will not be available";
            foldersIndexer_.reset(NULL); 
          }

          hasQueuesSupport_ = system.isMember(SYSTEM_CAPABILITIES) 
            && system[SYSTEM_CAPABILITIES].isMember(SYSTEM_CAPABILITIES_HAS_QUEUES)
            && system[SYSTEM_CAPABILITIES][SYSTEM_CAPABILITIES_HAS_QUEUES].asBool();
          
          if (hasQueuesSupport_)
          {
            if (delayedFilesDeleter_.get() != NULL)
            {
              LOG(INFO) << "Starting Delayed Files Deleter";
              delayedFilesDeleter_->Start();
            }
          }
          else
          {
            LOG(WARNING) << "Orthanc does not support Queues.  The plugin will not be able to implement the delayed deletion mode";
            delayedFilesDeleter_.reset(NULL); 
          }

          isReadOnly_ = system.isMember(READ_ONLY) && system[READ_ONLY].asBool();

          if (isReadOnly_)
          {
            LOG(WARNING) << "Orthanc is ReadOnly.  The plugin will not be able to adopt files and the indexer mode will not be available";
          }
        }

      }; break;
      case OrthancPluginChangeType_OrthancStopped:
      {
        boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and delayedDeletion pointer

        if (foldersIndexer_.get() != NULL)
        {
          foldersIndexer_->Stop();
          foldersIndexer_.reset(NULL);
        }

        if (delayedFilesDeleter_.get() != NULL)
        {
          delayedFilesDeleter_->Stop();
          delayedFilesDeleter_.reset(NULL);
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

static MoveStorageJob* CreateMoveStorageJob(const std::string& targetStorage, const std::vector<std::string>& instances, const Json::Value& resourcesForJobContent)
{
  std::unique_ptr<MoveStorageJob> job(new MoveStorageJob(targetStorage, instances, resourcesForJobContent));

  return job.release();
}


static void AddResourceForJobContent(Json::Value& resourcesForJobContent /* out */, Orthanc::ResourceType resourceType, const std::string& resourceId)
{
  const char* resourceGroup = Orthanc::GetResourceTypeText(resourceType, true, true);

  if (!resourcesForJobContent.isMember(resourceGroup))
  {
    resourcesForJobContent[resourceGroup] = Json::arrayValue;
  }
  
  resourcesForJobContent[resourceGroup].append(resourceId);
}

extern "C"
{

  OrthancPluginErrorCode PostAdoptInstance(OrthancPluginRestOutput* output,
                                           const char* url,
                                           const OrthancPluginHttpRequest* request) noexcept
  {
    try
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

        if (!body.isMember("Path") || !body["Path"].isString())
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "'Path' field is missing or not a string");
        }

        bool takeOwnership = body.isMember("TakeOwnership") && body["TakeOwnership"].asBool();  // false by default

        std::string instanceId, attachmentUuid;
        OrthancPluginStoreStatus storeStatus;

        AdoptFile(instanceId, attachmentUuid, storeStatus, body["Path"].asString(), takeOwnership);

        Json::Value response;

        if (storeStatus == OrthancPluginStoreStatus_Success)
        {
          response["InstanceId"] = instanceId;
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

      return OrthancPluginErrorCode_Success;
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "Exception: " << e.What();
      return static_cast<OrthancPluginErrorCode>(e.GetErrorCode());
    }
  }

  OrthancPluginErrorCode PostAbandonInstance(OrthancPluginRestOutput* output,
                                             const char* url,
                                             const OrthancPluginHttpRequest* request) noexcept
  {
    try
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

        if (!body.isMember("Path") || !body["Path"].isString())
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "'Path' field is missing or not a string");
        }

        AbandonFile(body["Path"].asString());

        OrthancPlugins::AnswerHttpError(200, output);
      }

      return OrthancPluginErrorCode_Success;
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "Exception: " << e.What();
      return static_cast<OrthancPluginErrorCode>(e.GetErrorCode());
    }
  }

  OrthancPluginErrorCode PostMoveStorage(OrthancPluginRestOutput* output,
                                         const char* /*url*/,
                                         const OrthancPluginHttpRequest* request) noexcept
  {
    try
    {
      OrthancPluginContext* context = OrthancPlugins::GetGlobalContext();

      if (request->method != OrthancPluginHttpMethod_Post)
      {
        OrthancPluginSendMethodNotAllowed(context, output, "POST");
        return OrthancPluginErrorCode_Success;
      }

      Json::Value requestPayload;

      if (!OrthancPlugins::ReadJson(requestPayload, request->body, request->bodySize))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "A JSON payload was expected");
      }

      std::vector<std::string> instances;
      Json::Value resourcesForJobContent;

      if (requestPayload.type() != Json::objectValue ||
          !requestPayload.isMember(KEY_RESOURCES) ||
          requestPayload[KEY_RESOURCES].type() != Json::arrayValue)
      {
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_BadFileFormat,
          "A request to the move-storage endpoint must provide a JSON object "
          "with the field \"" + std::string(KEY_RESOURCES) +
          "\" containing an array of resources to be sent");
      }

      if (!requestPayload.isMember(KEY_TARGET_STORAGE_ID)
          || requestPayload[KEY_TARGET_STORAGE_ID].type() != Json::stringValue
          || !CustomData::HasStorage(requestPayload[KEY_TARGET_STORAGE_ID].asString()))
      {
        throw Orthanc::OrthancException(
          Orthanc::ErrorCode_BadFileFormat,
          "A request to the move-storage endpoint must provide a JSON object "
          "with the field \"" + std::string(KEY_TARGET_STORAGE_ID) +
          "\" set to one of the storage ids");
      }

      const std::string& targetStorage = requestPayload[KEY_TARGET_STORAGE_ID].asString();
      const Json::Value& resources = requestPayload[KEY_RESOURCES];

      // Extract information about all the child instances
      for (Json::Value::ArrayIndex i = 0; i < resources.size(); i++)
      {
        if (resources[i].type() != Json::stringValue)
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat);
        }

        std::string resource = resources[i].asString();
        if (resource.empty())
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
        }

        // Test whether this resource is an instance
        Json::Value tmpResource;
        Json::Value tmpInstances;
        if (OrthancPlugins::RestApiGet(tmpResource, "/instances/" + resource, false))
        {
          instances.push_back(resource);
          AddResourceForJobContent(resourcesForJobContent, Orthanc::ResourceType_Instance, resource);
        }
        // This was not an instance, successively try with series/studies/patients
        else if ((OrthancPlugins::RestApiGet(tmpResource, "/series/" + resource, false) &&
                  OrthancPlugins::RestApiGet(tmpInstances, "/series/" + resource + "/instances?expand=false", false)) ||
                 (OrthancPlugins::RestApiGet(tmpResource, "/studies/" + resource, false) &&
                  OrthancPlugins::RestApiGet(tmpInstances, "/studies/" + resource + "/instances?expand=false", false)) ||
                 (OrthancPlugins::RestApiGet(tmpResource, "/patients/" + resource, false) &&
                  OrthancPlugins::RestApiGet(tmpInstances, "/patients/" + resource + "/instances?expand=false", false)))
        {
          if (tmpInstances.type() != Json::arrayValue)
          {
            throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
          }

          AddResourceForJobContent(resourcesForJobContent, Orthanc::StringToResourceType(tmpResource["Type"].asString().c_str()), resource);

          for (Json::Value::ArrayIndex j = 0; j < tmpInstances.size(); j++)
          {
            instances.push_back(tmpInstances[j].asString());
          }
        }
        else
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_UnknownResource);
        }
      }

      LOG(INFO) << "Moving " << instances.size() << " instances to storageId " << targetStorage;

      std::unique_ptr<MoveStorageJob> job(CreateMoveStorageJob(targetStorage, instances, resourcesForJobContent));

      try
      {
        OrthancPlugins::OrthancJob::SubmitFromRestApiPost(output, requestPayload, job.release());
      }
      catch (Orthanc::OrthancException ex)
      {
        LOG(ERROR) << "Failed to move instances: " << ex.What();
        // ANSWER buffer
        OrthancPlugins::AnswerHttpError(400, output);
      }
      return OrthancPluginErrorCode_Success;
    }
    catch (Orthanc::OrthancException& e)
    {
      LOG(ERROR) << "Exception: " << e.What();
      return static_cast<OrthancPluginErrorCode>(e.GetErrorCode());
    }
  }

  void GetPluginStatus(OrthancPluginRestOutput* output,
                       const char* url,
                       const OrthancPluginHttpRequest* request) noexcept
  {
    Json::Value status;
    
    {
      boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and delayedDeletion pointer

      status[PLUGIN_STATUS_DELAYED_DELETION_ACTIVE] = delayedFilesDeleter_.get() != NULL;
      status[PLUGIN_STATUS_INDEXER_ACTIVE] = foldersIndexer_.get() != NULL;
      
      if (delayedFilesDeleter_.get() != NULL)
      {
        status[PLUGIN_STATUS_DELAYED_DELETION_PENDING_FILES] = Json::UInt64(delayedFilesDeleter_->GetPendingDeletionFilesCount());
      }
    }
    
    OrthancPlugins::AnswerJson(status, output);
  }


  void GetAttachmentInfo(OrthancPluginRestOutput* output,
                         const char* url,
                         const OrthancPluginHttpRequest* request) noexcept
  {
    if (request->method != OrthancPluginHttpMethod_Get)
    {
      OrthancPlugins::AnswerMethodNotAllowed(output, "GET");
    }
    else
    {
      Json::Value response;

      OrthancPlugins::RestApiClient coreApi(url, request);

      if (coreApi.Execute() && coreApi.GetAnswerJson(response) && response.isObject())
      {
        CustomData customData = OrthancPlugins::GetAttachmentCustomData(response["Uuid"].asString());

        response["Path"] = customData.GetAbsolutePath().string();
        response["IsOwnedByOrthanc"] = customData.IsOwner();
        
        if (foldersIndexer_.get() != NULL)
        {
          response["IsIndexed"] = foldersIndexer_->IsFileIndexed(customData.GetAbsolutePath().string());
        }
        
        if (customData.IsOwner())
        {
          response["StorageId"] = customData.GetStorageId();
        }
  
        OrthancPlugins::AnswerJson(response, output);
      }
      else
      {
        OrthancPlugins::AnswerHttpError(404, output);
      }
    }
  }

  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context) noexcept
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

    OrthancPlugins::OrthancConfiguration advancedStorageConfiguration;
    orthancConfiguration.GetSection(advancedStorageConfiguration, "AdvancedStorage");

    bool enabled = advancedStorageConfiguration.GetBooleanValue(CONFIG_ENABLE, false);
    if (enabled)
    {
      try
      {
        fsyncOnWrite_ = orthancConfiguration.GetBooleanValue(CONFIG_SYNC_STORAGE_AREA, true);
        overwriteInstances_ = orthancConfiguration.GetBooleanValue(CONFIG_OVERWRITE_INSTANCES, false);

        const Json::Value& pluginJson = advancedStorageConfiguration.GetJson();

        try
        {
          PathGenerator::SetNamingScheme(advancedStorageConfiguration.GetStringValue(CONFIG_NAMING_SCHEME, "OrthancDefault"), overwriteInstances_);
        }
        catch (Orthanc::OrthancException& ex)
        {
          return -1;
        }

        std::string otherAttachmentsPrefix = advancedStorageConfiguration.GetStringValue(CONFIG_OTHER_ATTACHMENTS_PREFIX, "");
        LOG(WARNING) << "Prefix path to the other attachments root: " << otherAttachmentsPrefix;
        CustomData::SetOtherAttachmentsPrefix(otherAttachmentsPrefix);
        PathGenerator::SetOtherAttachmentsPrefix(otherAttachmentsPrefix);
      
        // if we have enabled multiple storage after files have been saved without this plugin, we still need the default StorageDirectory
        CustomData::SetOrthancCoreRootPath(orthancConfiguration.GetStringValue(CONFIG_STORAGE_DIRECTORY, "OrthancStorage"));
        LOG(WARNING) << "Path to the default storage area: " << CustomData::GetOrthancCoreRootPath();

        size_t maxPathLength = advancedStorageConfiguration.GetIntegerValue(CONFIG_MAX_PATH_LENGTH, 256);
        LOG(WARNING) << "Maximum path length: " << maxPathLength;
        CustomData::SetMaxPathLength(maxPathLength);

        if (pluginJson.isMember(CONFIG_MULTIPLE_STORAGES))
        {
          // multipleStoragesEnabled_ = true;
          const Json::Value& multipleStoragesJson = pluginJson[CONFIG_MULTIPLE_STORAGES];

          if (multipleStoragesJson.isMember(CONFIG_MULTIPLE_STORAGES_STORAGES) && multipleStoragesJson.isObject())
          {
            const Json::Value& storagesJson = multipleStoragesJson[CONFIG_MULTIPLE_STORAGES_STORAGES];
            Json::Value::Members storageIds = storagesJson.getMemberNames();

            for (Json::Value::Members::const_iterator it = storageIds.begin(); it != storageIds.end(); ++it)
            {
              if (!storagesJson[*it].isString())
              {
                LOG(ERROR) << "Storage path is not a string " << *it;
                return -1;
              }
              fs::path storagePath = storagesJson[*it].asString();

              CustomData::SetStorageRootPath(*it, storagesJson[*it].asString());
            }

            if (multipleStoragesJson.isMember(CONFIG_MULTIPLE_STORAGES_CURRENT_WRITE_STORAGE) && multipleStoragesJson[CONFIG_MULTIPLE_STORAGES_CURRENT_WRITE_STORAGE].isString())
            {
              try
              {
                CustomData::SetCurrentWriteStorageId(multipleStoragesJson[CONFIG_MULTIPLE_STORAGES_CURRENT_WRITE_STORAGE].asString());
              }
              catch (Orthanc::OrthancException& ex)
              {
                return -1;
              }
            }

            LOG(WARNING) << "multiple storages enabled.  Current Write storage : " << multipleStoragesJson[CONFIG_MULTIPLE_STORAGES_CURRENT_WRITE_STORAGE].asString();

            OrthancPluginRegisterRestCallback(OrthancPlugins::GetGlobalContext(), "/plugins/advanced-storage/move-storage", PostMoveStorage);
          }
        }

        if (advancedStorageConfiguration.IsSection(CONFIG_INDEXER))
        {
          OrthancPlugins::OrthancConfiguration indexerConfig;
          advancedStorageConfiguration.GetSection(indexerConfig, CONFIG_INDEXER);

          if (indexerConfig.GetBooleanValue(CONFIG_INDEXER_ENABLE, false))
          {

            std::list<std::string> indexedFolders, parsedExtensions, skippedExtensions;

            unsigned int indexerIntervalSeconds = indexerConfig.GetUnsignedIntegerValue(CONFIG_INDEXER_INTERVAL, 10 /* 10 seconds by default */);
            unsigned int throttleDelayMs = indexerConfig.GetUnsignedIntegerValue(CONFIG_INDEXER_THROTTLE_DELAY_MS, 0 /* 0 ms seconds by default */);
            bool takeOwnership = indexerConfig.GetBooleanValue(CONFIG_INDEXER_TAKE_OWNERSHIP, false);

            if (!indexerConfig.LookupListOfStrings(indexedFolders, CONFIG_INDEXER_FOLDERS, true) ||
                indexedFolders.empty())
            {
              throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                              "Missing configuration option for the AdvancedStorage - Indexer: " + std::string(CONFIG_INDEXER_FOLDERS));
            }

            indexerConfig.LookupListOfStrings(parsedExtensions, CONFIG_INDEXER_PARSED_EXTENSIONS, true);
            indexerConfig.LookupListOfStrings(skippedExtensions, CONFIG_INDEXER_SKIPPED_EXTENSIONS, true);

            if (parsedExtensions.size() > 0 && skippedExtensions.size() > 0)
            {
              throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange,
                                              std::string("You can not configure \"") + CONFIG_INDEXER_PARSED_EXTENSIONS + "\" and \"" + CONFIG_INDEXER_SKIPPED_EXTENSIONS + "\" at the same time");
            }


            LOG(WARNING) << "creating FoldersIndexer";
    
            boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and delayedDeletion pointer
            foldersIndexer_.reset(new FoldersIndexer(indexedFolders, indexerIntervalSeconds, throttleDelayMs, parsedExtensions, skippedExtensions, takeOwnership));
          }
          else
          {
            LOG(WARNING) << "FoldersIndexer is currently DISABLED";
          }
        }

        if (advancedStorageConfiguration.IsSection(CONFIG_DELAYED_DELETION))
        {
          OrthancPlugins::OrthancConfiguration delayedDeletionConfig;
          advancedStorageConfiguration.GetSection(delayedDeletionConfig, CONFIG_DELAYED_DELETION);

          if (delayedDeletionConfig.GetBooleanValue(CONFIG_DELAYED_DELETION_ENABLE, false))
          {
            unsigned int throttleDelayMs = delayedDeletionConfig.GetUnsignedIntegerValue(CONFIG_DELAYED_DELETION_THROTTLE_DELAY_MS, 0 /* 0 ms seconds by default */);

            LOG(WARNING) << "creating DelayedDeleter";
    
            boost::mutex::scoped_lock lock(mutex_); // because we modify/access foldersIndexer and delayedDeletion pointer
            delayedFilesDeleter_.reset(new DelayedFilesDeleter(throttleDelayMs));
          }
          else
          {
            LOG(WARNING) << "DelayedDeletion is currently DISABLED";
          }
        }

        OrthancPluginRegisterStorageArea3(context, StorageCreate, StorageReadRange, StorageRemove);

        OrthancPlugins::RegisterRestCallback<GetAttachmentInfo>("/(studies|series|instances|patients)/([^/]+)/attachments/(.*)/info", true);
        OrthancPlugins::RegisterRestCallback<GetPluginStatus>(std::string("/plugins/") + ORTHANC_PLUGIN_NAME + "/status", true);

        OrthancPluginRegisterOnChangeCallback(context, OnChangeCallback);
      }
      catch (Orthanc::OrthancException& e)
      {
        LOG(ERROR) << "Exception: " << e.What();
        return -1;
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
    return ORTHANC_PLUGIN_VERSION;
  }
}

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

namespace fs = boost::filesystem;

fs::path rootPath_;
bool multipleStoragesEnabled_ = false;
std::map<std::string, fs::path> rootPaths_;
std::string currentStorageId_;
std::string namingScheme_;
std::string otherAttachmentsPrefix_;
bool fsyncOnWrite_ = true;
size_t maxPathLength_ = 256;
size_t legacyPathLength = 39; // ex "/00/f7/00f7fd8b-47bd8c3a-ff917804-d180cdbc-40cf9527"

fs::path GetRootPath()
{
  if (multipleStoragesEnabled_)
  {
    return rootPaths_[currentStorageId_];
  }

  return rootPath_;
}

fs::path GetRootPath(const std::string& storageId)
{
  if (multipleStoragesEnabled_)
  {
    if (rootPaths_.find(storageId) == rootPaths_.end())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, std::string("Advanced Storage - storage '" + storageId + "' is not defined in configuration"));
    }
    return rootPaths_[storageId];
  }

  return rootPath_;
}


fs::path GetLegacyRelativePath(const std::string& uuid)
{
  if (!Orthanc::Toolbox::IsUuid(uuid))
  {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
  }

  fs::path path;

  if (!otherAttachmentsPrefix_.empty())
  {
    path /= otherAttachmentsPrefix_;
  }

  path /= std::string(&uuid[0], &uuid[2]);
  path /= std::string(&uuid[2], &uuid[4]);
  path /= uuid;

#if BOOST_HAS_FILESYSTEM_V3 == 1
  path.make_preferred();
#endif

  return path;
}

fs::path GetPath(const std::string& uuid,
                 const void* customDataBuffer,
                 uint64_t customDataSize)
{
  fs::path path;

  if (customDataSize != 0)
  {
    Json::Value customData;
    Orthanc::Toolbox::ReadJson(customData, customDataBuffer, customDataSize);

    if (customData["v"].asInt() == 1)   // Version
    {
      if (customData.isMember("s"))     // Storage ID
      {
        path = GetRootPath(customData["s"].asString());
      }
      else
      {
        path = GetRootPath();
      }
      
      if (customData.isMember("p"))   // Path
      {
        path /= customData["p"].asString();
      }
      else
      { // we are in "legacy mode" for the path part
        path /= GetLegacyRelativePath(uuid);
      }
    }
    else
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, std::string("Advanced Storage - unknown version for custom data '" + boost::lexical_cast<std::string>(customData["Version"].asInt()) + "'"));
    }
  }
  else // we are in "legacy mode"
  {
    path = rootPath_;
    path /= GetLegacyRelativePath(uuid);
  }

  path.make_preferred();
  return path;
}

void GetCustomData(std::string& output, const fs::path& path)
{
  // if we use defaults, non need to store anything in the metadata, the plugin has the same behavior as the core of Orthanc
  if (namingScheme_ == "OrthancDefault" && !multipleStoragesEnabled_)
  {
    return;
  }

  Json::Value customDataJson;
  // Keep short field names to reduce SQL data usage
  customDataJson["v"] = 1;  // Version

  // no need to store the path since if we are in the default mode
  if (namingScheme_ != "OrthancDefault")
  { 
    customDataJson["p"] = path.string();   // Path
  }

  if (multipleStoragesEnabled_)
  {
    customDataJson["s"] = currentStorageId_;  // Storage id
  }

  return Orthanc::Toolbox::WriteFastJson(output, customDataJson);
}

std::string GetSplitDateDicomTagToPath(const Json::Value& tags, const char* tagName, const char* defaultValue = NULL)
{
  if (tags.isMember(tagName) && tags[tagName].asString().size() == 8)
  {
    std::string date = tags[tagName].asString();
    return date.substr(0, 4) + "/" + date.substr(4, 2) + "/" + date.substr(6, 2);
  }
  else if (defaultValue != NULL)
  {
    return defaultValue;
  }

  return "";
}

std::string GetStringDicomTagForPath(const Json::Value& tags, const std::string& tagName, const char* defaultValue = NULL)
{
  if (tags.isMember(tagName) && tags[tagName].isString() && tags[tagName].asString().size() > 0)
  {
    return tags[tagName].asString();
  }
  else if (defaultValue != NULL)
  {
    return defaultValue;
  }
  
  return "";
}

std::string GetIntDicomTagForPath(const Json::Value& tags, const std::string& tagName, const char* defaultValue = NULL, size_t padding = 0)
{
  if (tags.isMember(tagName))
  {
    std::string value;
    if (tags[tagName].isInt())
    {
      value = boost::lexical_cast<std::string>(tags[tagName].asInt());
    }
    else if (tags[tagName].isString())
    {
      value = tags[tagName].asString();
    }

    if (padding > 0 && padding > value.size())
    {
      value = std::string(padding - value.size(), '0') + value;
    }
    return value;
  }
  else if (defaultValue != NULL)
  {
    return defaultValue;
  }
  
  return "";
}

void ReplaceTagKeyword(std::string& folderName, const std::string& keyword, const Json::Value& tags, const char* defaultValue, const char* tagKey = NULL)
{
  if (folderName.find(keyword) != std::string::npos)
  {
    std::string key = keyword.substr(1, keyword.size() -2);
    if (tagKey != NULL)
    {
      key = tagKey;
    }
    boost::replace_all(folderName, keyword, GetStringDicomTagForPath(tags, key, defaultValue));
  }
}

void ReplaceIntTagKeyword(std::string& folderName, const std::string& keyword, const Json::Value& tags, const char* defaultValue, size_t padding, const char* tagKey = NULL)
{
  if (folderName.find(keyword) != std::string::npos)
  {
    std::string key = keyword.substr(1, keyword.size() -2);
    if (tagKey != NULL)
    {
      key = tagKey;
    }
    boost::replace_all(folderName, keyword, GetIntDicomTagForPath(tags, key, defaultValue, padding));
  }
}


void ReplaceOrthancID(std::string& folderName, const std::string& keyword, const std::string& id, size_t from, size_t length)
{
  if (length == 0)
  {
    boost::replace_all(folderName, keyword, id);
  }
  else
  {
    boost::replace_all(folderName, keyword, id.substr(from, length));
  }
}



void AddIntDicomTagToPath(fs::path& path, const Json::Value& tags, const char* tagName, size_t zeroPaddingWidth = 0, const char* defaultValue = NULL)
{
  if (tags.isMember(tagName) && tags[tagName].isString() && tags[tagName].asString().size() > 0)
  {
    std::string tagValue = tags[tagName].asString();
    if (zeroPaddingWidth > 0 && tagValue.size() < zeroPaddingWidth)
    {
      std::string padding(zeroPaddingWidth - tagValue.size(), '0');
      path /= padding + tagValue; 
    }
    else
    {
      path /= tagValue;
    }
  }
  else if (defaultValue != NULL)
  {
    path /= defaultValue;
  }
}

std::string GetExtension(OrthancPluginContentType type, bool isCompressed)
{
  std::string extension;

  switch (type)
  {
    case OrthancPluginContentType_Dicom:
      extension = ".dcm";
      break;
    case OrthancPluginContentType_DicomUntilPixelData:
      extension = ".dcm.head";
      break;
    default:
      extension = ".unk";
  }
  if (isCompressed)
  {
    extension = extension + ".cmp"; // compression is zlib + size -> we can not use the .zip extension
  }
  
  return extension;
}

fs::path GetRelativePathFromTags(const Json::Value& tags, const char* uuid, OrthancPluginContentType type, bool isCompressed)
{
  fs::path path;

  if (!tags.isNull())
  { 
    std::vector<std::string> folderNames;
    Orthanc::Toolbox::SplitString(folderNames, namingScheme_, '/');

    for (std::vector<std::string>::const_iterator it = folderNames.begin(); it != folderNames.end(); ++it)
    {
      std::string folderName = *it;
      
      if (folderName.find("{split(StudyDate)}") != std::string::npos)
      {
        boost::replace_all(folderName, "{split(StudyDate)}", GetSplitDateDicomTagToPath(tags, "StudyDate", "NO_STUDY_DATE"));
      }

      if (folderName.find("{split(PatientBirthDate)}") != std::string::npos)
      {
        boost::replace_all(folderName, "{split(PatientBirthDate)}", GetSplitDateDicomTagToPath(tags, "PatientBirthDate", "NO_PATIENT_BIRTH_DATE"));
      }

      ReplaceTagKeyword(folderName, "{PatientID}", tags, "NO_PATIENT_ID");
      ReplaceTagKeyword(folderName, "{PatientBirthDate}", tags, "NO_PATIENT_BIRTH_DATE");
      ReplaceTagKeyword(folderName, "{PatientName}", tags, "NO_PATIENT_NAME");
      ReplaceTagKeyword(folderName, "{PatientSex}", tags, "NO_PATIENT_SEX");
      ReplaceTagKeyword(folderName, "{StudyInstanceUID}", tags, "NO_STUDY_INSTANCE_UID");
      ReplaceTagKeyword(folderName, "{StudyDate}", tags, "NO_STUDY_DATE");
      ReplaceTagKeyword(folderName, "{StudyID}", tags, "NO_STUDY_ID");
      ReplaceTagKeyword(folderName, "{StudyDescription}", tags, "NO_STUDY_DESCRIPTION");
      ReplaceTagKeyword(folderName, "{AccessionNumber}", tags, "NO_ACCESSION_NUMBER");
      ReplaceTagKeyword(folderName, "{SeriesInstanceUID}", tags, "NO_SERIES_INSTANCE_UID");
      ReplaceTagKeyword(folderName, "{SeriesDate}", tags, "NO_SERIES_DATE");
      ReplaceTagKeyword(folderName, "{SeriesDescription}", tags, "NO_SERIES_DESCRIPTION");
      ReplaceTagKeyword(folderName, "{SOPInstanceUID}", tags, "NO_SOP_INSTANCE_UID");
      ReplaceIntTagKeyword(folderName, "{SeriesNumber}", tags, "NO_SERIES_NUMBER", 0);
      ReplaceIntTagKeyword(folderName, "{InstanceNumber}", tags, "NO_INSTANCE_NUMBER", 0);
      ReplaceIntTagKeyword(folderName, "{pad4(SeriesNumber)}", tags, "NO_SERIES_NUMBER", 4, "SeriesNumber");
      ReplaceIntTagKeyword(folderName, "{pad4(InstanceNumber)}", tags, "NO_INSTANCE_NUMBER", 4, "InstanceNumber");
      ReplaceIntTagKeyword(folderName, "{pad6(SeriesNumber)}", tags, "NO_SERIES_NUMBER", 6, "SeriesNumber");
      ReplaceIntTagKeyword(folderName, "{pad6(InstanceNumber)}", tags, "NO_INSTANCE_NUMBER", 6, "InstanceNumber");
      ReplaceIntTagKeyword(folderName, "{pad8(SeriesNumber)}", tags, "NO_SERIES_NUMBER", 8, "SeriesNumber");
      ReplaceIntTagKeyword(folderName, "{pad8(InstanceNumber)}", tags, "NO_INSTANCE_NUMBER", 8, "InstanceNumber");

      Orthanc::DicomInstanceHasher hasher(tags["PatientID"].asString(), tags["StudyInstanceUID"].asString(), tags["SeriesInstanceUID"].asString(), tags["SOPInstanceUID"].asString());
      std::string orthancPatientId = hasher.HashPatient();
      std::string orthancStudyId = hasher.HashStudy();
      std::string orthancSeriesId = hasher.HashSeries();
      std::string orthancInstanceId = hasher.HashInstance();

      ReplaceOrthancID(folderName, "{OrthancPatientID}", orthancPatientId, 0, 0);
      ReplaceOrthancID(folderName, "{OrthancStudyID}", orthancStudyId, 0, 0);
      ReplaceOrthancID(folderName, "{OrthancSeriesID}", orthancSeriesId, 0, 0);
      ReplaceOrthancID(folderName, "{OrthancInstanceID}", orthancInstanceId, 0, 0);

      ReplaceOrthancID(folderName, "{01(OrthancPatientID)}", orthancPatientId, 0, 2);
      ReplaceOrthancID(folderName, "{01(OrthancStudyID)}", orthancStudyId, 0, 2);
      ReplaceOrthancID(folderName, "{01(OrthancSeriesID)}", orthancSeriesId, 0, 2);
      ReplaceOrthancID(folderName, "{01(OrthancInstanceID)}", orthancInstanceId, 0, 2);

      ReplaceOrthancID(folderName, "{23(OrthancPatientID)}", orthancPatientId, 2, 2);
      ReplaceOrthancID(folderName, "{23(OrthancStudyID)}", orthancStudyId, 2, 2);
      ReplaceOrthancID(folderName, "{23(OrthancSeriesID)}", orthancSeriesId, 2, 2);
      ReplaceOrthancID(folderName, "{23(OrthancInstanceID)}", orthancInstanceId, 2, 2);

      if (folderName.find("{UUID}") != std::string::npos)
      {
        boost::replace_all(folderName, "{UUID}", uuid);
      }

      if (folderName.find("{.ext}") != std::string::npos)
      {
        boost::replace_all(folderName, "{.ext}", GetExtension(type, isCompressed));
      }

      path /= folderName;
    }

    return path;
  }

  return GetLegacyRelativePath(uuid);
}


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
    fs::path relativePath = GetRelativePathFromTags(tags, uuid, type, isCompressed);
    fs::path rootPath = GetRootPath();
    fs::path path = rootPath / relativePath;

    // check that the final path is not 'above' the root path (this could happen if e.g., a PatientName is ../../../../toto)
    // fs::canonical() can not be used for that since the file needs to exist
    // so far, we'll just forbid path containing '..' since they might be suspicious
    if (path.string().find("..") != std::string::npos)
    {
      relativePath = GetLegacyRelativePath(uuid);
      fs::path legacyPath = rootPath / relativePath;
      LOG(WARNING) << "Advanced Storage - WAS02 - Path is suspicious since it contains '..': '" << path.string() << "' will be stored in '" << legacyPath << "'";
      path = legacyPath;
    }

    // check path length !!!!!, if too long, go back to legacy path and issue a warning
    if (path.string().size() > maxPathLength_)
    {
      relativePath = GetLegacyRelativePath(uuid);
      fs::path legacyPath = rootPath / relativePath;
      LOG(WARNING) << "Advanced Storage - WAS01 - Path is too long: '" << path.string() << "' will be stored in '" << legacyPath << "'";
      path = legacyPath;
    }

    if (fs::exists(path))
    {
      // Extremely unlikely case if uuid is included in the path: This Uuid has already been created
      // in the past.
      throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, "Advanced Storage - path already exists");

      // TODO for the future: handle duplicates path (e.g: there's no uuid in the path and we are uploading the same file again)
    }

    std::string customDataString;
    GetCustomData(customDataString, relativePath);

    LOG(INFO) << "Advanced Storage - creating attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + path.string() + ")";


    if (fs::exists(path.parent_path()))
    {
      if (!fs::is_directory(path.parent_path()))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_DirectoryOverFile);
      }
    }
    else
    {
      if (!fs::create_directories(path.parent_path()))
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_FileStorageCannotWrite);
      }
    }

    Orthanc::SystemToolbox::WriteFile(content, size, path.string(), fsyncOnWrite_);

    OrthancPluginCreateMemoryBuffer(OrthancPlugins::GetGlobalContext(), customData, customDataString.size());
    memcpy(customData->data, customDataString.data(), customDataString.size());
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


OrthancPluginErrorCode StorageReadWhole(OrthancPluginMemoryBuffer64* target,
                                        const char* uuid,
                                        OrthancPluginContentType type,
                                        const void* customData,
                                        uint64_t customDataSize)
{
  std::string path = GetPath(uuid, customData, customDataSize).string();

  LOG(INFO) << "Advanced Storage - Reading whole attachment \"" << uuid << "\" of type " << static_cast<int>(type) << " (path = " + path + ")";

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

    // get file size
    f.seekg(0, std::ios::end);
    std::streamsize fileSize = f.tellg();
    f.seekg(0, std::ios::beg);

    // The ReadWhole must allocate the buffer itself
    if (OrthancPluginCreateMemoryBuffer64(OrthancPlugins::GetGlobalContext(), target, fileSize) != OrthancPluginErrorCode_Success)
    {
      LOG(ERROR) << "Unable to allocate memory to read file: " << path;
      return OrthancPluginErrorCode_NotEnoughMemory;
    }

    if (fileSize != 0)
    {
      f.read(reinterpret_cast<char*>(target->data), fileSize);
    }

    f.close();
  }
  catch (...)
  {
    LOG(ERROR) << "Unexpected error while reading: " << path;
    return OrthancPluginErrorCode_StorageAreaPlugin;
  }

  return OrthancPluginErrorCode_Success;
}


OrthancPluginErrorCode StorageReadRange(OrthancPluginMemoryBuffer64* target,
                                        const char* uuid,
                                        OrthancPluginContentType type,
                                        uint64_t rangeStart,
                                        const void* customData,
                                        uint64_t customDataSize)
{
  std::string path = GetPath(uuid, customData, customDataSize).string();

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
  fs::path path = GetPath(uuid, customData, customDataSize);

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

    while (parent != GetRootPath())
    {
      fs::remove(parent);
      parent = parent.parent_path();
    }
  }
  catch (...)
  {
    // Ignore the error
  }

  return OrthancPluginErrorCode_Success;
}

extern "C"
{

  ORTHANC_PLUGINS_API int32_t OrthancPluginInitialize(OrthancPluginContext* context)
  {
    OrthancPlugins::SetGlobalContext(context);
    Orthanc::Logging::InitializePluginContext(context);

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

      namingScheme_ = advancedStorage.GetStringValue("NamingScheme", "OrthancDefault");
      if (namingScheme_ != "OrthancDefault")
      {
        // when using a custom scheme, to avoid collisions, you must include, at least the attachment UUID
        // or each of the DICOM IDs or orthanc IDs
        if (namingScheme_.find("{UUID}") == std::string::npos)
        {
          LOG(ERROR) << "AdvancedStorage - To avoid files from being overwritten, your naming scheme shall alway contain the {UUID} (at least in this beta version !!!).";
          return -1;

          if (namingScheme_.find("PatientID") == std::string::npos && namingScheme_.find("OrthancPatientID") == std::string::npos)
          {
            LOG(ERROR) << "AdvancedStorage - To avoid files from being overwritten, your naming scheme shall alway contain either the {UUID} or 4 DICOM identifiers ({PatientID}, {StudyInstanceUID}, {SeriesInstanceUID}, {SOPInstanceUID}) or 4 Orthanc identifiers ({PatientOrthancID}, {StudyOrthancID}, {SeriesOrthancID}, {SOPInstanceUID}).";
            return -1;
          }

          if (namingScheme_.find("StudyInstanceUID") == std::string::npos && namingScheme_.find("OrthancStudyID") == std::string::npos)
          {
            LOG(ERROR) << "AdvancedStorage - To avoid files from being overwritten, your naming scheme shall alway contain either the {UUID} or 4 DICOM identifiers ({PatientID}, {StudyInstanceUID}, {SeriesInstanceUID}, {SOPInstanceUID}) or 4 Orthanc identifiers ({PatientOrthancID}, {StudyOrthancID}, {SeriesOrthancID}, {SOPInstanceUID}).";
            return -1;
          }

          if (namingScheme_.find("SeriesInstanceUID") == std::string::npos && namingScheme_.find("OrthancSeriesID") == std::string::npos)
          {
            LOG(ERROR) << "AdvancedStorage - To avoid files from being overwritten, your naming scheme shall alway contain either the {UUID} or 4 DICOM identifiers ({PatientID}, {StudyInstanceUID}, {SeriesInstanceUID}, {SOPInstanceUID}) or 4 Orthanc identifiers ({PatientOrthancID}, {StudyOrthancID}, {SeriesOrthancID}, {SOPInstanceUID}).";
            return -1;
          }

          if (namingScheme_.find("SOPInstanceUID") == std::string::npos && namingScheme_.find("OrthancInstanceID") == std::string::npos)
          {
            LOG(ERROR) << "AdvancedStorage - To avoid files from being overwritten, your naming scheme shall alway contain either the {UUID} or 4 DICOM identifiers ({PatientID}, {StudyInstanceUID}, {SeriesInstanceUID}, {SOPInstanceUID}) or 4 Orthanc identifiers ({PatientOrthancID}, {StudyOrthancID}, {SeriesOrthancID}, {SOPInstanceUID}).";
            return -1;
          }
        }
      }

      otherAttachmentsPrefix_ = advancedStorage.GetStringValue("OtherAttachmentsPrefix", "");
      LOG(WARNING) << "AdvancedStorage - Path to the other attachments root: " << otherAttachmentsPrefix_;
      
      // if we have enabled multiple storage after files have been saved without this plugin, we still need the default StorageDirectory
      rootPath_ = fs::path(orthancConfiguration.GetStringValue("StorageDirectory", "OrthancStorage"));
      LOG(WARNING) << "AdvancedStorage - Path to the default storage area: " << rootPath_.string();

      maxPathLength_ = advancedStorage.GetIntegerValue("MaxPathLength", 256);
      LOG(WARNING) << "AdvancedStorage - Maximum path length: " << maxPathLength_;

      if (!rootPath_.is_absolute())
      {
        LOG(ERROR) << "AdvancedStorage - Path to the default storage area should be an absolute path " << rootPath_ << " (\"StorageDirectory\" in main Orthanc configuration)";
        return -1;
      }

      if (rootPath_.size() > (maxPathLength_ - legacyPathLength))
      {
        LOG(ERROR) << "AdvancedStorage - Path to the default storage is too long";
        return -1;
      }

      if (pluginJson.isMember("MultipleStorages"))
      {
        multipleStoragesEnabled_ = true;
        const Json::Value& multipleStoragesJson = pluginJson["MultipleStorages"];
        
        if (multipleStoragesJson.isMember("Storages") && multipleStoragesJson.isObject() && multipleStoragesJson.isMember("CurrentStorage") && multipleStoragesJson["CurrentStorage"].isString())
        {
          const Json::Value& storagesJson = multipleStoragesJson["Storages"];
          Json::Value::Members storageIds = storagesJson.getMemberNames();
    
          for (Json::Value::Members::const_iterator it = storageIds.begin(); it != storageIds.end(); ++it)
          {
            const Json::Value& storagePath = storagesJson[*it];
            if (!storagePath.isString())
            {
              LOG(ERROR) << "AdvancedStorage - Storage path is not a string " << *it;
              return -1;
            }

            rootPaths_[*it] = storagePath.asString();

            if (!rootPaths_[*it].is_absolute())
            {
              LOG(ERROR) << "AdvancedStorage - Storage path shall be absolute path '" << storagePath.asString() << "'";
              return -1;
            }

            if (storagePath.asString().size() > (maxPathLength_ - legacyPathLength))
            {
              LOG(ERROR) << "AdvancedStorage - Storage path is too long '" << storagePath.asString() << "'";
              return -1;
            }
          }

          currentStorageId_ = multipleStoragesJson["CurrentStorage"].asString();

          if (rootPaths_.find(currentStorageId_) == rootPaths_.end())
          {
            LOG(ERROR) << "AdvancedStorage - CurrentStorage is not defined in Storages list: " << currentStorageId_;
            return -1;
          }

          LOG(WARNING) << "AdvancedStorage - multiple storages enabled.  Current storage : " << rootPaths_[currentStorageId_].string();
        }
      }

      OrthancPluginRegisterStorageArea3(context, StorageCreate, StorageReadWhole, StorageReadRange, StorageRemove);
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

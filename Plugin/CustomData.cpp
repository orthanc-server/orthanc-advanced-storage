/**
 * Orthanc - A Lightweight, RESTful DICOM Store
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


#include <Compatibility.h>
#include <OrthancException.h>
#include <Logging.h>
#include <SystemToolbox.h>
#include <Toolbox.h>

#include "CustomData.h"
#include "PathGenerator.h"
#include "Helpers.h"


namespace OrthancPlugins
{
  static const char* SERIALIZATION_KEY_VERSION = "v";
  static const char* SERIALIZATION_KEY_IS_OWNER = "o";
  static const char* SERIALIZATION_KEY_PATH = "p";
  static const char* SERIALIZATION_KEY_STORAGE_ID = "s";
  
  static boost::filesystem::path orthancCoreRootPath_;
  static std::map<std::string, boost::filesystem::path> storagesRootPaths_;
  static std::string currentWriteStorageId_;
  static size_t maxPathLength_ = 256;
	static std::string otherAttachmentsPrefix_;


  void CustomData::SetCurrentWriteStorageId(const std::string& storageId)
  {
    if (storagesRootPaths_.find(storageId) == storagesRootPaths_.end())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, "Advanced Storage - CurrentWriteStorage is not defined in Storages list " + storageId);
    }

    currentWriteStorageId_ = storageId;
  }

  void CustomData::SetOtherAttachmentsPrefix(const std::string& prefix)
  {
    otherAttachmentsPrefix_ = prefix;
  }

  void CustomData::SetMaxPathLength(size_t maxPathLength)
  {
    maxPathLength_ = maxPathLength;
  }

  bool CustomData::IsARootPath(const boost::filesystem::path& path)
  {
    if (path == orthancCoreRootPath_)
    {
      return true;
    }

    for (std::map<std::string, boost::filesystem::path>::const_iterator it = storagesRootPaths_.begin(); it != storagesRootPaths_.end(); ++it)
    {
      if (path == it->second)
      {
        return true;
      }
    }

    return false;
  }

  void CustomData::SetStorageRootPath(const std::string& storageId, const std::string& rootPath)
  {
    storagesRootPaths_[storageId] = Orthanc::SystemToolbox::PathFromUtf8(rootPath);
  }

  boost::filesystem::path CustomData::GetStorageRootPath(const std::string& storageId)
  {
    if (storagesRootPaths_.find(storageId) == storagesRootPaths_.end())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, "Advanced Storage - no storage root path found for storage  '" + storageId + "'");
    }

    return storagesRootPaths_[storageId];
  }

  bool CustomData::IsMultipleStoragesEnabled()
  {
    return storagesRootPaths_.size() > 0 && !currentWriteStorageId_.empty();
  }

  void CustomData::SetOrthancCoreRootPath(const std::string& rootPath)
  {
    orthancCoreRootPath_ = Orthanc::SystemToolbox::PathFromUtf8(rootPath);
  }

  boost::filesystem::path CustomData::GetOrthancCoreRootPath()
  {
    if (orthancCoreRootPath_.empty())
    {
      throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, "Advanced Storage - no Orthanc storage directory defined");
    }

    return orthancCoreRootPath_;
  }

  boost::filesystem::path CustomData::GetCurrentWriteRootPath()
  {
    if (IsMultipleStoragesEnabled())
    {
      return GetStorageRootPath(currentWriteStorageId_);
    }
    
    return GetOrthancCoreRootPath();
  }

  CustomData::CustomData() :
    isOwner_(true),
    hasBeenAdopted_(false)
  {
  }

  CustomData CustomData::CreateForMoveStorage(const CustomData& currentCustomData, const std::string& targetStorageId)
  {
    CustomData cd;
    cd.uuid_ = currentCustomData.uuid_;
    cd.path_ = currentCustomData.path_;
    cd.isOwner_ = currentCustomData.isOwner_;
    cd.storageId_ = targetStorageId;

    return cd;
  }

  CustomData CustomData::FromString(const std::string& uuid, 
                                    const void* customDataBuffer,
                                    uint64_t customDataSize)
  {
    CustomData cd;
    cd.uuid_ = uuid;

    if (customDataSize != 0)
    {
      Json::Value v;
      OrthancPlugins::ReadJson(v, customDataBuffer, customDataSize);

      if (v[SERIALIZATION_KEY_VERSION].asInt() == 1)
      {
        cd.isOwner_ = v[SERIALIZATION_KEY_IS_OWNER].asBool();

        if (!cd.isOwner_ && !v.isMember(SERIALIZATION_KEY_PATH))
        {
          throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError, std::string("Advanced Storage - an adopted file has no path ! - ") + uuid);
        }
        
        cd.path_ = Orthanc::SystemToolbox::PathFromUtf8(v[SERIALIZATION_KEY_PATH].asString());
        
        if (v.isMember(SERIALIZATION_KEY_STORAGE_ID))
        {
          cd.storageId_ = v[SERIALIZATION_KEY_STORAGE_ID].asString();
        }
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, std::string("Invalid CustomData version: ") + boost::lexical_cast<std::string>(v[SERIALIZATION_KEY_VERSION].asInt()));
      }
    }

    return cd;
  }

  CustomData CustomData::CreateForAdoption(const boost::filesystem::path& path, bool takeOwnership)
  {
    CustomData cd;
    cd.isOwner_ = takeOwnership;
    // cd.uuid_  // stays empty in this case
    // cd.storageId_ // stays empty in this case
    cd.path_ = path;
    cd.hasBeenAdopted_ = true;

    return cd;
  }


  CustomData CustomData::CreateForWriting(const std::string& uuid,
                                          const boost::filesystem::path& relativePath)
  {
    CustomData cd;
    cd.isOwner_ = true;
    cd.uuid_ = uuid;
    cd.storageId_ = currentWriteStorageId_;
    cd.path_ = relativePath;

    boost::filesystem::path rootPath = CustomData::GetCurrentWriteRootPath();
    boost::filesystem::path absolutePath = rootPath / cd.path_;
    std::string absolutPathUtf8Str = Orthanc::SystemToolbox::PathToUtf8(absolutePath);

    // check that the final path is not 'above' the root path (this could happen if e.g., a PatientName is ../../../../toto)
    // fs::canonical() can not be used for that since the file needs to exist
    // so far, we'll just forbid path containing '..' since they might be suspicious.
    // We can not accept '=' either because it is used in the serialization.
    if (absolutPathUtf8Str.find("..") != std::string::npos || absolutPathUtf8Str.find("=") != std::string::npos)
    {
      if (!otherAttachmentsPrefix_.empty())
      {
        cd.path_ = Orthanc::SystemToolbox::PathFromUtf8(otherAttachmentsPrefix_) / PathGenerator::GetLegacyRelativePath(uuid);
      }
      else
      {
        cd.path_ = PathGenerator::GetLegacyRelativePath(uuid);
      }
      
      boost::filesystem::path absoluteLegacyPath = rootPath / cd.path_;
      LOG(WARNING) << "Advanced Storage - WAS02 - Path is suspicious since it contains '..' or '=': '" << absolutPathUtf8Str << "' will be stored in '" << Orthanc::SystemToolbox::PathToUtf8(absoluteLegacyPath) << "'";
      absolutePath = absoluteLegacyPath;
    }
    else if (absolutPathUtf8Str.size() > maxPathLength_) // check path length !!!!!, if too long, go back to legacy path and issue a warning
    {
      if (!otherAttachmentsPrefix_.empty())
      {
        cd.path_ = Orthanc::SystemToolbox::PathFromUtf8(otherAttachmentsPrefix_) / PathGenerator::GetLegacyRelativePath(uuid);
      }
      else
      {
        cd.path_ = PathGenerator::GetLegacyRelativePath(uuid);
      }

      boost::filesystem::path absoluteLegacyPath = rootPath / cd.path_;
      LOG(WARNING) << "Advanced Storage - WAS01 - Path is too long: '" << absolutPathUtf8Str << "' will be stored in '" << absoluteLegacyPath << "'";
      absolutePath = absoluteLegacyPath;
    }

    return cd;
  }

  boost::filesystem::path CustomData::GetAbsolutePath() const
  {
    if (path_.is_absolute())
    {
      return path_;
    }

    boost::filesystem::path absolutePath;

    if (!storageId_.empty())
    {
      absolutePath = GetStorageRootPath(storageId_);
    }
    else
    {
      absolutePath = GetOrthancCoreRootPath();
    }

    if (!path_.empty())
    {
      absolutePath /= path_;
    }
    else
    {
      absolutePath /= PathGenerator::GetLegacyRelativePath(uuid_);
    }

    absolutePath.make_preferred();

    return absolutePath;
  }

  void CustomData::ToString(std::string& serialized) const
  {
    serialized.clear();

    // if we use defaults, no need to store anything in the metadata, the plugin has the same behavior as the core of Orthanc
    if (PathGenerator::IsDefaultNamingScheme() && !IsMultipleStoragesEnabled() && !hasBeenAdopted_)
    {
      return;
    }

    Json::Value v;
    v[SERIALIZATION_KEY_VERSION] = 1;

    // no need to store the path if we are in the default mode
    // unless it is a file that has been adopted
    if (!PathGenerator::IsDefaultNamingScheme() || hasBeenAdopted_)
    { 
      v[SERIALIZATION_KEY_PATH] = Orthanc::SystemToolbox::PathToUtf8(path_);
    }

    if (IsMultipleStoragesEnabled() && isOwner_ && !hasBeenAdopted_)
    {
      v[SERIALIZATION_KEY_STORAGE_ID] = storageId_;
    }

    v[SERIALIZATION_KEY_IS_OWNER] = isOwner_;

    OrthancPlugins::WriteFastJson(serialized, v);
  }

  bool CustomData::HasStorage(const std::string& storageId)
  {
    return storagesRootPaths_.find(storageId) != storagesRootPaths_.end();
  }

}

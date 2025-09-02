/**
 * Orthanc - A Lightweight, RESTful DICOM Store
 * Copyright (C) 2024-2025 Orthanc Team SRL, Belgium
 * Copyright (C) 2021-2025 Sebastien Jodogne, ICTEAM UCLouvain, Belgium
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

#pragma once

#include <boost/filesystem.hpp>
#include <string.h>

namespace OrthancPlugins
{

  class CustomData
  {
    boost::filesystem::path     path_;
    bool                        isOwner_;
    std::string                 storageId_;
    std::string                 uuid_;
    bool                        hasBeenAdopted_; // internal, not serialized

  protected:
    CustomData();

  public:

    static CustomData FromString(const std::string& uuid,
                                 const void* customDataBuffer,
                                 uint64_t customDataSize);

    static CustomData CreateForWriting(const std::string& uuid,
                                       const boost::filesystem::path& relativePath);

    static CustomData CreateForAdoption(const boost::filesystem::path& path, bool takeOwnership);

    static CustomData CreateForMoveStorage(const CustomData& currentCustomData, const std::string& targetStorageId);

    static void SetMaxPathLength(size_t maxPathLength);

    static void SetCurrentWriteStorageId(const std::string& storageId);

    static void SetOtherAttachmentsPrefix(const std::string& prefix);

    static void SetStorageRootPath(const std::string& storageId, const std::string& rootPath);

    static boost::filesystem::path GetStorageRootPath(const std::string& storageId);

    static bool HasStorage(const std::string& storageId);

    static void SetOrthancCoreRootPath(const std::string& rootPath);

    static boost::filesystem::path GetOrthancCoreRootPath();

    static boost::filesystem::path GetCurrentWriteRootPath();

    void ToString(std::string& serialized) const;

    static bool IsARootPath(const boost::filesystem::path& path);

    boost::filesystem::path GetAbsolutePath() const;

    bool IsRelativePath() const
    {
      return !path_.is_absolute();
    }

    bool IsOwner() const
    {
      return isOwner_;
    }

    const std::string& GetUuid() const
    {
      return uuid_;
    }

    const std::string& GetStorageId() const
    {
      return storageId_;
    }
    
  protected:
    static bool IsMultipleStoragesEnabled();
  };

}

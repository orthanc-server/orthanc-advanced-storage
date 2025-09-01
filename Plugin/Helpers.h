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

#pragma once

#include "../Resources/Orthanc/Plugins/OrthancPluginCppWrapper.h"
#include "CustomData.h"
#include <boost/filesystem.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

namespace OrthancPlugins
{
  CustomData GetAttachmentCustomData(const std::string& attachmentUuid);

  bool UpdateAttachmentCustomData(const std::string& attachmentUuid, const CustomData& customData);

  void RemoveEmptyParentDirectories(const boost::filesystem::path& path);

  void AdoptFile(std::string& instanceId,
                 std::string& attachmentUuid,
                 OrthancPluginStoreStatus& storeStatus,
                 const std::string& path, 
                 bool takeOwnership);

  void AbandonFile(const std::string& path); 

  void MarkAdoptedFileAsDeleted(const std::string& path);

#ifdef _WIN32
  std::wstring Utf8ToWString(const std::string& str);

  std::string WStringToUtf8(const std::wstring& wstr);
#endif

  boost::filesystem::path PathFromUtf8(const std::string& utf8);
  
  std::string PathToUtf8(const boost::filesystem::path& p);
}

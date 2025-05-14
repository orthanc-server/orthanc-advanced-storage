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

namespace fs = boost::filesystem;

namespace OrthancPlugins
{
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
}
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

#include "PathOwner.h"
#include <OrthancException.h>
#include <Logging.h>
#include <SystemToolbox.h>
#include <Toolbox.h>


namespace OrthancPlugins
{
  static const char* SERIALIZATION_KEY_VERSION = "v";
  static const char* SERIALIZATION_KEY_CONTENT_TYPE = "c";
  static const char* SERIALIZATION_KEY_RESOURCE_ID = "r";
  static const char* SERIALIZATION_KEY_RESOURCE_TYPE = "t";


  PathOwner::PathOwner() :
    resourceType_(OrthancPluginResourceType_None),
    contentType_(OrthancPluginContentType_Unknown)
  {
  }
  
  

  PathOwner PathOwner::FromString(const std::string& serialized)
  {
    PathOwner owner;

    if (!serialized.empty())
    {
       Json::Value v;
      OrthancPlugins::ReadJson(v, serialized);

      if (v[SERIALIZATION_KEY_VERSION].asInt() == 1)
      {
        owner.contentType_ = static_cast<OrthancPluginContentType>(v[SERIALIZATION_KEY_CONTENT_TYPE].asInt());
        owner.resourceId_ = v[SERIALIZATION_KEY_RESOURCE_ID].asString();
        owner.resourceType_ = static_cast<OrthancPluginResourceType>(v[SERIALIZATION_KEY_RESOURCE_TYPE].asInt());
      }
      else
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, std::string("Advanced Storage - unknown version found for owner data "));
      }
    }

    return owner;
  }

  PathOwner PathOwner::Create(const std::string& resourceId,
                              OrthancPluginResourceType resourceType,
                              OrthancPluginContentType contentType)
  {
    PathOwner owner;
    owner.resourceId_ = resourceId;
    owner.contentType_ = contentType;
    owner.resourceType_ = resourceType;

    return owner;
  }

  void PathOwner::ToString(std::string& serialized) const
  {
    Json::Value v;
    v[SERIALIZATION_KEY_VERSION] = 1;
    v[SERIALIZATION_KEY_RESOURCE_TYPE] = resourceType_;
    v[SERIALIZATION_KEY_CONTENT_TYPE] = contentType_;
    v[SERIALIZATION_KEY_RESOURCE_ID] = resourceId_;

    OrthancPlugins::WriteFastJson(serialized, v);
  }

  void PathOwner::GetUrlForDeletion(std::string& url) const
  {
    url = "";

    switch (resourceType_)
    {
      case OrthancPluginResourceType_Instance:
        url += "/instances/" + resourceId_;
        break;
      case OrthancPluginResourceType_Series:
        url += "/series/" + resourceId_;
        break;
      case OrthancPluginResourceType_Study:
        url += "/studies/" + resourceId_;
        break;
      case OrthancPluginResourceType_Patient:
        url += "/patients/" + resourceId_;
        break;
      default:
        throw Orthanc::OrthancException(Orthanc::ErrorCode_InternalError);
    }

    if (contentType_ != OrthancPluginContentType_Dicom)
    {
      url += "/attachments/" + boost::lexical_cast<std::string>(contentType_);
    }

  }
}
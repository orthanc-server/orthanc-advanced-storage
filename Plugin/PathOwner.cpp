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

#include "PathOwner.h"
#include <OrthancException.h>
#include <Logging.h>
#include <SystemToolbox.h>
#include <Toolbox.h>


namespace OrthancPlugins
{
  static const char* SERIALIZATION_KEY_VERSION = "v";
  // static const char* SERIALIZATION_KEY_ATTACHMENT_UUID = "u";
  static const char* SERIALIZATION_KEY_CONTENT_TYPE = "c";
  static const char* SERIALIZATION_KEY_RESOURCE_ID = "r";
  static const char* SERIALIZATION_KEY_RESOURCE_TYPE = "t";


  PathOwner::PathOwner() :
    resourceType_(OrthancPluginResourceType_None),
    contentType_(OrthancPluginContentType_Unknown)
  {
  }
  
  
    // std::string                 attachmeUuid_;
    // std::string                 resourceId_;
    // OrthancPluginResourceType   resourceType_;
    // OrthancPluginContentType    contentType_;

  PathOwner PathOwner::FromString(const void* pathOwnerDataBuffer,
                                  uint64_t pathOwnerDataSize)
  {
    PathOwner owner;

    if (pathOwnerDataSize != 0)
    {
      std::map<std::string, std::string> ownerDico;
      std::string ownerDataString(reinterpret_cast<const char*>(pathOwnerDataBuffer), pathOwnerDataSize);

      std::vector<std::string> ownerDataVariables;
      Orthanc::Toolbox::SplitString(ownerDataVariables, ownerDataString, ';');

      for (std::vector<std::string>::const_iterator it = ownerDataVariables.begin(); it != ownerDataVariables.end(); ++it)
      {
        std::vector<std::string> ownerDataVariable;
        Orthanc::Toolbox::SplitString(ownerDataVariable, *it, '=');

        if (ownerDataVariable.size() == 2)
        {
          ownerDico[ownerDataVariable[0]] = ownerDataVariable[1];
        }        
      }

      if (ownerDico.find(SERIALIZATION_KEY_VERSION) == ownerDico.end())
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, std::string("Advanced Storage - no version found for owner data "));
      }

      if (ownerDico[SERIALIZATION_KEY_VERSION] != "1")
      {
        throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange, std::string("Advanced Storage - unknown version found for owner data "));
      }

      if (ownerDico.find(SERIALIZATION_KEY_CONTENT_TYPE) != ownerDico.end())
      {
        owner.contentType_ = static_cast<OrthancPluginContentType>(boost::lexical_cast<unsigned int>(ownerDico[SERIALIZATION_KEY_CONTENT_TYPE]));
      }

      if (ownerDico.find(SERIALIZATION_KEY_RESOURCE_TYPE) != ownerDico.end())
      {
        owner.resourceType_ = static_cast<OrthancPluginResourceType>(boost::lexical_cast<unsigned int>(ownerDico[SERIALIZATION_KEY_RESOURCE_TYPE]));
      }

      if (ownerDico.find(SERIALIZATION_KEY_RESOURCE_ID) != ownerDico.end())
      {
        owner.resourceId_ = ownerDico[SERIALIZATION_KEY_RESOURCE_ID];
      }

    }

    return owner;
  }

  PathOwner PathOwner::Create(//const std::string& attachmentUuid,
                              const std::string& resourceId,
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
    std::vector<std::string> ownerDataVariables;
    ownerDataVariables.push_back(std::string(SERIALIZATION_KEY_VERSION) + "=1");

    ownerDataVariables.push_back(std::string(SERIALIZATION_KEY_RESOURCE_TYPE) + "=" + boost::lexical_cast<std::string>(resourceType_));
    ownerDataVariables.push_back(std::string(SERIALIZATION_KEY_CONTENT_TYPE) + "=" + boost::lexical_cast<std::string>(contentType_));
    ownerDataVariables.push_back(std::string(SERIALIZATION_KEY_RESOURCE_ID) + "=" + resourceId_);

    Orthanc::Toolbox::JoinStrings(serialized, ownerDataVariables, ";");
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
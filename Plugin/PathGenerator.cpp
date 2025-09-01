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

#include <Compatibility.h>
#include <OrthancException.h>
#include <Logging.h>
#include <SystemToolbox.h>
#include <Toolbox.h>
#include <DicomFormat/DicomInstanceHasher.h>


#include "PathGenerator.h"
#include "Helpers.h"

namespace OrthancPlugins
{
	static std::string namingScheme_;
	std::string PathGenerator::otherAttachmentsPrefix_;

  void PathGenerator::SetOtherAttachmentsPrefix(const std::string& prefix)
  {
    otherAttachmentsPrefix_ = prefix;
  }

	void PathGenerator::SetNamingScheme(const std::string& namingScheme, bool isOverwriteInstances)
	{
		namingScheme_ = namingScheme;

		if (namingScheme_ != "OrthancDefault")
		{
			// when using a custom scheme, to avoid collisions, you must include, at least the attachment UUID
			// or each of the DICOM IDs or the orthancInstanceID
			if (!isOverwriteInstances)
			{
        if (namingScheme_.find("{UUID}") == std::string::npos)
        {
				  throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "To avoid files from being overwritten, your naming scheme shall alway contain the {UUID} when the OverwriteInstances Orthanc configuration is set to true.");
        }
      }
      else
      {
        if (namingScheme_.find("{UUID}") != std::string::npos || namingScheme_.find("{OrthancInstanceID}") != std::string::npos)
				{
          return;
				}

				if ((namingScheme_.find("PatientID") == std::string::npos)
          || (namingScheme_.find("StudyInstanceUID") == std::string::npos)
          || (namingScheme_.find("SeriesInstanceUID") == std::string::npos)
          || (namingScheme_.find("SOPInstanceUID") == std::string::npos))
				{
					throw Orthanc::OrthancException(Orthanc::ErrorCode_BadFileFormat, "To avoid files from being overwritten, your naming scheme shall alway contain either the {UUID} or 4 DICOM identifiers ({PatientID}, {StudyInstanceUID}, {SeriesInstanceUID}, {SOPInstanceUID}) or the Orthanc Instance ID ({OrthancInstanceID}).");
				}
      }
		}
	}

	bool PathGenerator::IsDefaultNamingScheme()
	{
		return namingScheme_ == "OrthancDefault";
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


	void AddIntDicomTagToPath(boost::filesystem::path& path, const Json::Value& tags, const char* tagName, size_t zeroPaddingWidth = 0, const char* defaultValue = NULL)
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


	boost::filesystem::path PathGenerator::GetRelativePathFromTags(const Json::Value& tags, const char* uuid, OrthancPluginContentType type, bool isCompressed)
	{
		boost::filesystem::path path;

		if (!tags.isNull())
		{ 
      // If, at some point, we enable saving other attachments using tags, we must re-think the duplicate files avoidance scheme:
      // E.g: using the 4 DICOM IDs or the OrthancInstanceID in the path is not sufficient anymore to avoid duplicates.
      if (type != OrthancPluginContentType_Dicom)
      {
    		return GetLegacyRelativePath(uuid);
      }

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

				path /= OrthancPlugins::PathFromUtf8(folderName);
			}

			return path;
		}
    else if (type != OrthancPluginContentType_Dicom && !otherAttachmentsPrefix_.empty())
    {
      return boost::filesystem::path(otherAttachmentsPrefix_) / GetLegacyRelativePath(uuid);
    }

		return GetLegacyRelativePath(uuid);
	}


	boost::filesystem::path PathGenerator::GetLegacyRelativePath(const std::string& uuid)
	{
		if (!Orthanc::Toolbox::IsUuid(uuid))
		{
			throw Orthanc::OrthancException(Orthanc::ErrorCode_ParameterOutOfRange);
		}

		boost::filesystem::path path;

		path /= std::string(&uuid[0], &uuid[2]);
		path /= std::string(&uuid[2], &uuid[4]);
		path /= uuid;

	#if BOOST_HAS_FILESYSTEM_V3 == 1
		path.make_preferred();
	#endif

		return path;
	}

}
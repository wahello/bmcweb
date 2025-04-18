// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors
#pragma once

#include "bmcweb_config.h"

#include "app.hpp"
#include "async_resp.hpp"
#include "dbus_singleton.hpp"
#include "dbus_utility.hpp"
#include "error_messages.hpp"
#include "http/parsing.hpp"
#include "http_request.hpp"
#include "http_response.hpp"
#include "io_context_singleton.hpp"
#include "logging.hpp"
#include "privileges.hpp"
#include "query.hpp"
#include "registries/privilege_registry.hpp"
#include "utility.hpp"
#include "utils/dbus_utils.hpp"
#include "utils/json_utils.hpp"
#include "utils/time_utils.hpp"

#include <systemd/sd-bus.h>

#include <boost/asio/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/result.hpp>
#include <boost/url/format.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/url.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/bus/match.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/message/native_types.hpp>
#include <sdbusplus/unpack_properties.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace redfish
{
namespace certs
{
constexpr const char* certInstallIntf = "xyz.openbmc_project.Certs.Install";
constexpr const char* certReplaceIntf = "xyz.openbmc_project.Certs.Replace";
constexpr const char* objDeleteIntf = "xyz.openbmc_project.Object.Delete";
constexpr const char* certPropIntf = "xyz.openbmc_project.Certs.Certificate";
constexpr const char* dbusPropIntf = "org.freedesktop.DBus.Properties";
constexpr const char* dbusObjManagerIntf = "org.freedesktop.DBus.ObjectManager";
constexpr const char* httpsServiceName =
    "xyz.openbmc_project.Certs.Manager.Server.Https";
constexpr const char* ldapServiceName =
    "xyz.openbmc_project.Certs.Manager.Client.Ldap";
constexpr const char* authorityServiceName =
    "xyz.openbmc_project.Certs.Manager.Authority.Truststore";
constexpr const char* baseObjectPath = "/xyz/openbmc_project/certs";
constexpr const char* httpsObjectPath =
    "/xyz/openbmc_project/certs/server/https";
constexpr const char* ldapObjectPath = "/xyz/openbmc_project/certs/client/ldap";
constexpr const char* authorityObjectPath =
    "/xyz/openbmc_project/certs/authority/truststore";
} // namespace certs

/**
 * The Certificate schema defines a Certificate Service which represents the
 * actions available to manage certificates and links to where certificates
 * are installed.
 */

inline std::string getCertificateFromReqBody(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const crow::Request& req)
{
    nlohmann::json reqJson;
    JsonParseResult ret = parseRequestAsJson(req, reqJson);
    if (ret != JsonParseResult::Success)
    {
        // We did not receive JSON request, proceed as it is RAW data
        return req.body();
    }

    std::string certificate;
    std::optional<std::string> certificateType = "PEM";

    if (!json_util::readJsonPatch(             //
            req, asyncResp->res,               //
            "CertificateString", certificate,  //
            "CertificateType", certificateType //
            ))
    {
        BMCWEB_LOG_ERROR("Required parameters are missing");
        messages::internalError(asyncResp->res);
        return {};
    }

    if (*certificateType != "PEM")
    {
        messages::propertyValueNotInList(asyncResp->res, *certificateType,
                                         "CertificateType");
        return {};
    }

    return certificate;
}

/**
 * Class to create a temporary certificate file for uploading to system
 */
class CertificateFile
{
  public:
    CertificateFile() = delete;
    CertificateFile(const CertificateFile&) = delete;
    CertificateFile& operator=(const CertificateFile&) = delete;
    CertificateFile(CertificateFile&&) = delete;
    CertificateFile& operator=(CertificateFile&&) = delete;
    explicit CertificateFile(const std::string& certString)
    {
        std::array<char, 18> dirTemplate = {'/', 't', 'm', 'p', '/', 'C',
                                            'e', 'r', 't', 's', '.', 'X',
                                            'X', 'X', 'X', 'X', 'X', '\0'};
        // NOLINTNEXTLINE(misc-include-cleaner)
        char* tempDirectory = mkdtemp(dirTemplate.data());
        if (tempDirectory != nullptr)
        {
            certDirectory = tempDirectory;
            certificateFile = certDirectory / "cert.pem";
            std::ofstream out(certificateFile,
                              std::ofstream::out | std::ofstream::binary |
                                  std::ofstream::trunc);
            out << certString;
            out.close();
            BMCWEB_LOG_DEBUG("Creating certificate file{}",
                             certificateFile.string());
        }
    }
    ~CertificateFile()
    {
        if (std::filesystem::exists(certDirectory))
        {
            BMCWEB_LOG_DEBUG("Removing certificate file{}",
                             certificateFile.string());
            std::error_code ec;
            std::filesystem::remove_all(certDirectory, ec);
            if (ec)
            {
                BMCWEB_LOG_ERROR("Failed to remove temp directory{}",
                                 certDirectory.string());
            }
        }
    }
    std::string getCertFilePath()
    {
        return certificateFile;
    }

  private:
    std::filesystem::path certificateFile;
    std::filesystem::path certDirectory;
};

/**
 * @brief Parse and update Certificate Issue/Subject property
 *
 * @param[in] asyncResp Shared pointer to the response message
 * @param[in] str  Issuer/Subject value in key=value pairs
 * @param[in] type Issuer/Subject
 * @return None
 */
inline void updateCertIssuerOrSubject(nlohmann::json& out,
                                      std::string_view value)
{
    // example: O=openbmc-project.xyz,CN=localhost
    std::string_view::iterator i = value.begin();
    while (i != value.end())
    {
        std::string_view::iterator tokenBegin = i;
        while (i != value.end() && *i != '=')
        {
            std::advance(i, 1);
        }
        if (i == value.end())
        {
            break;
        }
        std::string_view key(tokenBegin, static_cast<size_t>(i - tokenBegin));
        std::advance(i, 1);
        tokenBegin = i;
        while (i != value.end() && *i != ',')
        {
            std::advance(i, 1);
        }
        std::string_view val(tokenBegin, static_cast<size_t>(i - tokenBegin));
        if (key == "L")
        {
            out["City"] = val;
        }
        else if (key == "CN")
        {
            out["CommonName"] = val;
        }
        else if (key == "C")
        {
            out["Country"] = val;
        }
        else if (key == "O")
        {
            out["Organization"] = val;
        }
        else if (key == "OU")
        {
            out["OrganizationalUnit"] = val;
        }
        else if (key == "ST")
        {
            out["State"] = val;
        }
        // skip comma character
        if (i != value.end())
        {
            std::advance(i, 1);
        }
    }
}

/**
 * @brief Retrieve the installed certificate list
 *
 * @param[in] asyncResp Shared pointer to the response message
 * @param[in] basePath DBus object path to search
 * @param[in] listPtr Json pointer to the list in asyncResp
 * @param[in] countPtr Json pointer to the count in asyncResp
 * @return None
 */
inline void getCertificateList(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& basePath, const nlohmann::json::json_pointer& listPtr,
    const nlohmann::json::json_pointer& countPtr)
{
    constexpr std::array<std::string_view, 1> interfaces = {
        certs::certPropIntf};
    dbus::utility::getSubTreePaths(
        basePath, 0, interfaces,
        [asyncResp, listPtr, countPtr](
            const boost::system::error_code& ec,
            const dbus::utility::MapperGetSubTreePathsResponse& certPaths) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("Certificate collection query failed: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            nlohmann::json& links = asyncResp->res.jsonValue[listPtr];
            links = nlohmann::json::array();
            for (const auto& certPath : certPaths)
            {
                sdbusplus::message::object_path objPath(certPath);
                std::string certId = objPath.filename();
                if (certId.empty())
                {
                    BMCWEB_LOG_ERROR("Invalid certificate objPath {}",
                                     certPath);
                    continue;
                }

                boost::urls::url certURL;
                if (objPath.parent_path() == certs::httpsObjectPath)
                {
                    certURL = boost::urls::format(
                        "/redfish/v1/Managers/{}/NetworkProtocol/HTTPS/Certificates/{}",
                        BMCWEB_REDFISH_MANAGER_URI_NAME, certId);
                }
                else if (objPath.parent_path() == certs::ldapObjectPath)
                {
                    certURL = boost::urls::format(
                        "/redfish/v1/AccountService/LDAP/Certificates/{}",
                        certId);
                }
                else if (objPath.parent_path() == certs::authorityObjectPath)
                {
                    certURL = boost::urls::format(
                        "/redfish/v1/Managers/{}/Truststore/Certificates/{}",
                        BMCWEB_REDFISH_MANAGER_URI_NAME, certId);
                }
                else
                {
                    continue;
                }

                nlohmann::json::object_t link;
                link["@odata.id"] = certURL;
                links.emplace_back(std::move(link));
            }

            asyncResp->res.jsonValue[countPtr] = links.size();
        });
}

/**
 * @brief Retrieve the certificates properties and append to the response
 * message
 *
 * @param[in] asyncResp Shared pointer to the response message
 * @param[in] objectPath  Path of the D-Bus service object
 * @param[in] certId  Id of the certificate
 * @param[in] certURL  URL of the certificate object
 * @param[in] name  name of the certificate
 * @return None
 */
inline void getCertificateProperties(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& objectPath, const std::string& service,
    const std::string& certId, const boost::urls::url& certURL,
    const std::string& name)
{
    BMCWEB_LOG_DEBUG("getCertificateProperties Path={} certId={} certURl={}",
                     objectPath, certId, certURL);
    dbus::utility::getAllProperties(
        service, objectPath, certs::certPropIntf,
        [asyncResp, certURL, certId,
         name](const boost::system::error_code& ec,
               const dbus::utility::DBusPropertiesMap& properties) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
                messages::resourceNotFound(asyncResp->res, "Certificate",
                                           certId);
                return;
            }

            const std::string* certificateString = nullptr;
            const std::vector<std::string>* keyUsage = nullptr;
            const std::string* issuer = nullptr;
            const std::string* subject = nullptr;
            const uint64_t* validNotAfter = nullptr;
            const uint64_t* validNotBefore = nullptr;

            const bool success = sdbusplus::unpackPropertiesNoThrow(
                dbus_utils::UnpackErrorPrinter(), properties,
                "CertificateString", certificateString, "KeyUsage", keyUsage,
                "Issuer", issuer, "Subject", subject, "ValidNotAfter",
                validNotAfter, "ValidNotBefore", validNotBefore);

            if (!success)
            {
                messages::internalError(asyncResp->res);
                return;
            }

            asyncResp->res.jsonValue["@odata.id"] = certURL;
            asyncResp->res.jsonValue["@odata.type"] =
                "#Certificate.v1_0_0.Certificate";
            asyncResp->res.jsonValue["Id"] = certId;
            asyncResp->res.jsonValue["Name"] = name;
            asyncResp->res.jsonValue["Description"] = name;
            asyncResp->res.jsonValue["CertificateString"] = "";
            asyncResp->res.jsonValue["KeyUsage"] = nlohmann::json::array();

            if (certificateString != nullptr)
            {
                asyncResp->res.jsonValue["CertificateString"] =
                    *certificateString;
            }

            if (keyUsage != nullptr)
            {
                asyncResp->res.jsonValue["KeyUsage"] = *keyUsage;
            }

            if (issuer != nullptr)
            {
                updateCertIssuerOrSubject(asyncResp->res.jsonValue["Issuer"],
                                          *issuer);
            }

            if (subject != nullptr)
            {
                updateCertIssuerOrSubject(asyncResp->res.jsonValue["Subject"],
                                          *subject);
            }

            if (validNotAfter != nullptr)
            {
                asyncResp->res.jsonValue["ValidNotAfter"] =
                    redfish::time_utils::getDateTimeUint(*validNotAfter);
            }

            if (validNotBefore != nullptr)
            {
                asyncResp->res.jsonValue["ValidNotBefore"] =
                    redfish::time_utils::getDateTimeUint(*validNotBefore);
            }

            asyncResp->res.addHeader(
                boost::beast::http::field::location,
                std::string_view(certURL.data(), certURL.size()));
        });
}

inline void deleteCertificate(
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& service,
    const sdbusplus::message::object_path& objectPath)
{
    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp,
         id{objectPath.filename()}](const boost::system::error_code& ec) {
            if (ec)
            {
                messages::resourceNotFound(asyncResp->res, "Certificate", id);
                return;
            }
            BMCWEB_LOG_INFO("Certificate deleted");
            asyncResp->res.result(boost::beast::http::status::no_content);
        },
        service, objectPath, certs::objDeleteIntf, "Delete");
}

inline void handleCertificateServiceGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (req.session == nullptr)
    {
        messages::internalError(asyncResp->res);
        return;
    }

    asyncResp->res.jsonValue["@odata.type"] =
        "#CertificateService.v1_0_0.CertificateService";
    asyncResp->res.jsonValue["@odata.id"] = "/redfish/v1/CertificateService";
    asyncResp->res.jsonValue["Id"] = "CertificateService";
    asyncResp->res.jsonValue["Name"] = "Certificate Service";
    asyncResp->res.jsonValue["Description"] =
        "Actions available to manage certificates";
    // /redfish/v1/CertificateService/CertificateLocations is something
    // only ConfigureManager can access then only display when the user
    // has permissions ConfigureManager
    Privileges effectiveUserPrivileges =
        redfish::getUserPrivileges(*req.session);
    if (isOperationAllowedWithPrivileges({{"ConfigureManager"}},
                                         effectiveUserPrivileges))
    {
        asyncResp->res.jsonValue["CertificateLocations"]["@odata.id"] =
            "/redfish/v1/CertificateService/CertificateLocations";
    }
    nlohmann::json& actions = asyncResp->res.jsonValue["Actions"];
    nlohmann::json& replace = actions["#CertificateService.ReplaceCertificate"];
    replace["target"] =
        "/redfish/v1/CertificateService/Actions/CertificateService.ReplaceCertificate";
    nlohmann::json::array_t allowed;
    allowed.emplace_back("PEM");
    replace["CertificateType@Redfish.AllowableValues"] = std::move(allowed);
    actions["#CertificateService.GenerateCSR"]["target"] =
        "/redfish/v1/CertificateService/Actions/CertificateService.GenerateCSR";
}

inline void handleCertificateLocationsGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/CertificateService/CertificateLocations";
    asyncResp->res.jsonValue["@odata.type"] =
        "#CertificateLocations.v1_0_0.CertificateLocations";
    asyncResp->res.jsonValue["Name"] = "Certificate Locations";
    asyncResp->res.jsonValue["Id"] = "CertificateLocations";
    asyncResp->res.jsonValue["Description"] =
        "Defines a resource that an administrator can use in order to "
        "locate all certificates installed on a given service";

    getCertificateList(asyncResp, certs::baseObjectPath,
                       "/Links/Certificates"_json_pointer,
                       "/Links/Certificates@odata.count"_json_pointer);
}

inline void handleError(const std::string_view dbusErrorName,
                        const std::string& id, const std::string& certificate,
                        const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (dbusErrorName == "org.freedesktop.DBus.Error.UnknownObject")
    {
        messages::resourceNotFound(asyncResp->res, "Certificate", id);
    }
    else if (dbusErrorName ==
             "xyz.openbmc_project.Certs.Error.InvalidCertificate")
    {
        messages::propertyValueIncorrect(asyncResp->res, "Certificate",
                                         certificate);
    }
    else
    {
        messages::internalError(asyncResp->res);
    }
}

inline void handleReplaceCertificateAction(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    std::string certificate;
    std::string certURI;
    std::optional<std::string> certificateType = "PEM";

    if (!json_util::readJsonAction(             //
            req, asyncResp->res,                //
            "CertificateString", certificate,   //
            "CertificateType", certificateType, //
            "CertificateUri/@odata.id", certURI //
            ))
    {
        BMCWEB_LOG_ERROR("Required parameters are missing");
        return;
    }

    if (!certificateType)
    {
        // should never happen, but it never hurts to be paranoid.
        return;
    }
    if (certificateType != "PEM")
    {
        messages::actionParameterNotSupported(asyncResp->res, "CertificateType",
                                              "ReplaceCertificate");
        return;
    }

    BMCWEB_LOG_INFO("Certificate URI to replace: {}", certURI);

    boost::system::result<boost::urls::url> parsedUrl =
        boost::urls::parse_relative_ref(certURI);
    if (!parsedUrl)
    {
        messages::actionParameterValueFormatError(
            asyncResp->res, certURI, "CertificateUri", "ReplaceCertificate");
        return;
    }

    std::string id;
    sdbusplus::message::object_path objectPath;
    std::string name;
    std::string service;
    if (crow::utility::readUrlSegments(*parsedUrl, "redfish", "v1", "Managers",
                                       "bmc", "NetworkProtocol", "HTTPS",
                                       "Certificates", std::ref(id)))
    {
        objectPath = sdbusplus::message::object_path(certs::httpsObjectPath) /
                     id;
        name = "HTTPS certificate";
        service = certs::httpsServiceName;
    }
    else if (crow::utility::readUrlSegments(*parsedUrl, "redfish", "v1",
                                            "AccountService", "LDAP",
                                            "Certificates", std::ref(id)))
    {
        objectPath = sdbusplus::message::object_path(certs::ldapObjectPath) /
                     id;
        name = "LDAP certificate";
        service = certs::ldapServiceName;
    }
    else if (crow::utility::readUrlSegments(*parsedUrl, "redfish", "v1",
                                            "Managers", "bmc", "Truststore",
                                            "Certificates", std::ref(id)))
    {
        objectPath =
            sdbusplus::message::object_path(certs::authorityObjectPath) / id;
        name = "TrustStore certificate";
        service = certs::authorityServiceName;
    }
    else
    {
        messages::actionParameterNotSupported(asyncResp->res, "CertificateUri",
                                              "ReplaceCertificate");
        return;
    }

    std::shared_ptr<CertificateFile> certFile =
        std::make_shared<CertificateFile>(certificate);
    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp, certFile, objectPath, service, url{*parsedUrl}, id, name,
         certificate](const boost::system::error_code& ec,
                      sdbusplus::message_t& m) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
                const sd_bus_error* dbusError = m.get_error();
                if ((dbusError != nullptr) && (dbusError->name != nullptr))
                {
                    handleError(dbusError->name, id, certificate, asyncResp);
                }
                else
                {
                    messages::internalError(asyncResp->res);
                }
                return;
            }
            getCertificateProperties(asyncResp, objectPath, service, id, url,
                                     name);
            BMCWEB_LOG_DEBUG("HTTPS certificate install file={}",
                             certFile->getCertFilePath());
        },
        service, objectPath, certs::certReplaceIntf, "Replace",
        certFile->getCertFilePath());
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static std::unique_ptr<sdbusplus::bus::match_t> csrMatcher;
/**
 * @brief Read data from CSR D-bus object and set to response
 *
 * @param[in] asyncResp Shared pointer to the response message
 * @param[in] certURI Link to certificate collection URI
 * @param[in] service D-Bus service name
 * @param[in] certObjPath certificate D-Bus object path
 * @param[in] csrObjPath CSR D-Bus object path
 * @return None
 */
inline void getCSR(const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
                   const std::string& certURI, const std::string& service,
                   const std::string& certObjPath,
                   const std::string& csrObjPath)
{
    BMCWEB_LOG_DEBUG("getCSR CertObjectPath{} CSRObjectPath={} service={}",
                     certObjPath, csrObjPath, service);
    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp,
         certURI](const boost::system::error_code& ec, const std::string& csr) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }
            if (csr.empty())
            {
                BMCWEB_LOG_ERROR("CSR read is empty");
                messages::internalError(asyncResp->res);
                return;
            }
            asyncResp->res.jsonValue["CSRString"] = csr;
            asyncResp->res.jsonValue["CertificateCollection"]["@odata.id"] =
                certURI;
        },
        service, csrObjPath, "xyz.openbmc_project.Certs.CSR", "CSR");
}

inline void handleGenerateCSRAction(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    static const int rsaKeyBitLength = 2048;

    // Required parameters
    std::string city;
    std::string commonName;
    std::string country;
    std::string organization;
    std::string organizationalUnit;
    std::string state;
    std::string certURI;

    // Optional parameters
    std::optional<std::vector<std::string>> optAlternativeNames =
        std::vector<std::string>();
    std::optional<std::string> optContactPerson = "";
    std::optional<std::string> optChallengePassword = "";
    std::optional<std::string> optEmail = "";
    std::optional<std::string> optGivenName = "";
    std::optional<std::string> optInitials = "";
    std::optional<int64_t> optKeyBitLength = rsaKeyBitLength;
    std::optional<std::string> optKeyCurveId = "secp384r1";
    std::optional<std::string> optKeyPairAlgorithm = "EC";
    std::optional<std::vector<std::string>> optKeyUsage =
        std::vector<std::string>();
    std::optional<std::string> optSurname = "";
    std::optional<std::string> optUnstructuredName = "";
    if (!json_util::readJsonAction(                     //
            req, asyncResp->res,                        //
            "AlternativeNames", optAlternativeNames,    //
            "CertificateCollection/@odata.id", certURI, //
            "ChallengePassword", optChallengePassword,  //
            "City", city,                               //
            "CommonName", commonName,                   //
            "ContactPerson", optContactPerson,          //
            "Country", country,                         //
            "Email", optEmail,                          //
            "GivenName", optGivenName,                  //
            "Initials", optInitials,                    //
            "KeyBitLength", optKeyBitLength,            //
            "KeyCurveId", optKeyCurveId,                //
            "KeyPairAlgorithm", optKeyPairAlgorithm,    //
            "KeyUsage", optKeyUsage,                    //
            "Organization", organization,               //
            "OrganizationalUnit", organizationalUnit,   //
            "State", state,                             //
            "Surname", optSurname,                      //
            "UnstructuredName", optUnstructuredName     //
            ))
    {
        return;
    }

    // bmcweb has no way to store or decode a private key challenge
    // password, which will likely cause bmcweb to crash on startup
    // if this is not set on a post so not allowing the user to set
    // value
    if (!optChallengePassword->empty())
    {
        messages::actionParameterNotSupported(asyncResp->res, "GenerateCSR",
                                              "ChallengePassword");
        return;
    }

    std::string objectPath;
    std::string service;
    if (certURI.starts_with(std::format(
            "/redfish/v1/Managers/{}/NetworkProtocol/HTTPS/Certificates",
            BMCWEB_REDFISH_MANAGER_URI_NAME)))
    {
        objectPath = certs::httpsObjectPath;
        service = certs::httpsServiceName;
    }
    else if (certURI.starts_with(
                 "/redfish/v1/AccountService/LDAP/Certificates"))
    {
        objectPath = certs::ldapObjectPath;
        service = certs::ldapServiceName;
    }
    else
    {
        messages::actionParameterNotSupported(
            asyncResp->res, "CertificateCollection", "GenerateCSR");
        return;
    }

    // supporting only EC and RSA algorithm
    if (*optKeyPairAlgorithm != "EC" && *optKeyPairAlgorithm != "RSA")
    {
        messages::actionParameterNotSupported(
            asyncResp->res, "KeyPairAlgorithm", "GenerateCSR");
        return;
    }

    // supporting only 2048 key bit length for RSA algorithm due to
    // time consumed in generating private key
    if (*optKeyPairAlgorithm == "RSA" && *optKeyBitLength != rsaKeyBitLength)
    {
        messages::propertyValueNotInList(asyncResp->res, *optKeyBitLength,
                                         "KeyBitLength");
        return;
    }

    // validate KeyUsage supporting only 1 type based on URL
    if (certURI.starts_with(std::format(
            "/redfish/v1/Managers/{}/NetworkProtocol/HTTPS/Certificates",
            BMCWEB_REDFISH_MANAGER_URI_NAME)))
    {
        if (optKeyUsage->empty())
        {
            optKeyUsage->emplace_back("ServerAuthentication");
        }
        else if (optKeyUsage->size() == 1)
        {
            if ((*optKeyUsage)[0] != "ServerAuthentication")
            {
                messages::propertyValueNotInList(asyncResp->res,
                                                 (*optKeyUsage)[0], "KeyUsage");
                return;
            }
        }
        else
        {
            messages::actionParameterNotSupported(asyncResp->res, "KeyUsage",
                                                  "GenerateCSR");
            return;
        }
    }
    else if (certURI.starts_with(
                 "/redfish/v1/AccountService/LDAP/Certificates"))
    {
        if (optKeyUsage->empty())
        {
            optKeyUsage->emplace_back("ClientAuthentication");
        }
        else if (optKeyUsage->size() == 1)
        {
            if ((*optKeyUsage)[0] != "ClientAuthentication")
            {
                messages::propertyValueNotInList(asyncResp->res,
                                                 (*optKeyUsage)[0], "KeyUsage");
                return;
            }
        }
        else
        {
            messages::actionParameterNotSupported(asyncResp->res, "KeyUsage",
                                                  "GenerateCSR");
            return;
        }
    }

    // Only allow one CSR matcher at a time so setting retry
    // time-out and timer expiry to 10 seconds for now.
    static const int timeOut = 10;
    if (csrMatcher)
    {
        messages::serviceTemporarilyUnavailable(asyncResp->res,
                                                std::to_string(timeOut));
        return;
    }

    // Make this static so it survives outside this method
    static boost::asio::steady_timer timeout(getIoContext());
    timeout.expires_after(std::chrono::seconds(timeOut));
    timeout.async_wait([asyncResp](const boost::system::error_code& ec) {
        csrMatcher = nullptr;
        if (ec)
        {
            // operation_aborted is expected if timer is canceled
            // before completion.
            if (ec != boost::asio::error::operation_aborted)
            {
                BMCWEB_LOG_ERROR("Async_wait failed {}", ec);
            }
            return;
        }
        BMCWEB_LOG_ERROR("Timed out waiting for Generating CSR");
        messages::internalError(asyncResp->res);
    });

    // create a matcher to wait on CSR object
    BMCWEB_LOG_DEBUG("create matcher with path {}", objectPath);
    std::string match("type='signal',"
                      "interface='org.freedesktop.DBus.ObjectManager',"
                      "path='" +
                      objectPath +
                      "',"
                      "member='InterfacesAdded'");
    csrMatcher = std::make_unique<sdbusplus::bus::match_t>(
        *crow::connections::systemBus, match,
        [asyncResp, service, objectPath, certURI](sdbusplus::message_t& m) {
            timeout.cancel();
            if (m.is_method_error())
            {
                BMCWEB_LOG_ERROR("Dbus method error!!!");
                messages::internalError(asyncResp->res);
                return;
            }

            dbus::utility::DBusInterfacesMap interfacesProperties;

            sdbusplus::message::object_path csrObjectPath;
            m.read(csrObjectPath, interfacesProperties);
            BMCWEB_LOG_DEBUG("CSR object added{}", csrObjectPath.str);
            for (const auto& interface : interfacesProperties)
            {
                if (interface.first == "xyz.openbmc_project.Certs.CSR")
                {
                    getCSR(asyncResp, certURI, service, objectPath,
                           csrObjectPath.str);
                    break;
                }
            }
        });
    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp](const boost::system::error_code& ec, const std::string&) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec.message());
                messages::internalError(asyncResp->res);
                return;
            }
        },
        service, objectPath, "xyz.openbmc_project.Certs.CSR.Create",
        "GenerateCSR", *optAlternativeNames, *optChallengePassword, city,
        commonName, *optContactPerson, country, *optEmail, *optGivenName,
        *optInitials, *optKeyBitLength, *optKeyCurveId, *optKeyPairAlgorithm,
        *optKeyUsage, organization, organizationalUnit, state, *optSurname,
        *optUnstructuredName);
}

inline void requestRoutesCertificateService(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/CertificateService/")
        .privileges(redfish::privileges::getCertificateService)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleCertificateServiceGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/CertificateService/CertificateLocations/")
        .privileges(redfish::privileges::getCertificateLocations)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleCertificateLocationsGet, std::ref(app)));

    BMCWEB_ROUTE(
        app,
        "/redfish/v1/CertificateService/Actions/CertificateService.ReplaceCertificate/")
        .privileges(redfish::privileges::postCertificateService)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleReplaceCertificateAction, std::ref(app)));

    BMCWEB_ROUTE(
        app,
        "/redfish/v1/CertificateService/Actions/CertificateService.GenerateCSR/")
        .privileges(redfish::privileges::postCertificateService)
        .methods(boost::beast::http::verb::post)(
            std::bind_front(handleGenerateCSRAction, std::ref(app)));
} // requestRoutesCertificateService

inline void handleHTTPSCertificateCollectionGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    asyncResp->res.jsonValue["@odata.id"] = boost::urls::format(
        "/redfish/v1/Managers/{}/NetworkProtocol/HTTPS/Certificates",
        BMCWEB_REDFISH_MANAGER_URI_NAME);
    asyncResp->res.jsonValue["@odata.type"] =
        "#CertificateCollection.CertificateCollection";
    asyncResp->res.jsonValue["Name"] = "HTTPS Certificates Collection";
    asyncResp->res.jsonValue["Description"] =
        "A Collection of HTTPS certificate instances";

    getCertificateList(asyncResp, certs::httpsObjectPath,
                       "/Members"_json_pointer,
                       "/Members@odata.count"_json_pointer);
}

inline void handleHTTPSCertificateCollectionPost(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    BMCWEB_LOG_DEBUG("HTTPSCertificateCollection::doPost");

    asyncResp->res.jsonValue["Name"] = "HTTPS Certificate";
    asyncResp->res.jsonValue["Description"] = "HTTPS Certificate";

    std::string certHttpBody = getCertificateFromReqBody(asyncResp, req);

    if (certHttpBody.empty())
    {
        BMCWEB_LOG_ERROR("Cannot get certificate from request body.");
        messages::unrecognizedRequestBody(asyncResp->res);
        return;
    }

    std::shared_ptr<CertificateFile> certFile =
        std::make_shared<CertificateFile>(certHttpBody);

    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp, certFile](const boost::system::error_code& ec,
                              const std::string& objectPath) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            sdbusplus::message::object_path path(objectPath);
            std::string certId = path.filename();
            const boost::urls::url certURL = boost::urls::format(
                "/redfish/v1/Managers/{}/NetworkProtocol/HTTPS/Certificates/{}",
                BMCWEB_REDFISH_MANAGER_URI_NAME, certId);
            getCertificateProperties(asyncResp, objectPath,
                                     certs::httpsServiceName, certId, certURL,
                                     "HTTPS Certificate");
            BMCWEB_LOG_DEBUG("HTTPS certificate install file={}",
                             certFile->getCertFilePath());
        },
        certs::httpsServiceName, certs::httpsObjectPath, certs::certInstallIntf,
        "Install", certFile->getCertFilePath());
}

inline void handleHTTPSCertificateGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId, const std::string& certId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    BMCWEB_LOG_DEBUG("HTTPS Certificate ID={}", certId);
    const boost::urls::url certURL = boost::urls::format(
        "/redfish/v1/Managers/{}/NetworkProtocol/HTTPS/Certificates/{}",
        BMCWEB_REDFISH_MANAGER_URI_NAME, certId);
    std::string objPath =
        sdbusplus::message::object_path(certs::httpsObjectPath) / certId;
    getCertificateProperties(asyncResp, objPath, certs::httpsServiceName,
                             certId, certURL, "HTTPS Certificate");
}

inline void requestRoutesHTTPSCertificate(App& app)
{
    BMCWEB_ROUTE(
        app, "/redfish/v1/Managers/<str>/NetworkProtocol/HTTPS/Certificates/")
        .privileges(redfish::privileges::getCertificateCollection)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleHTTPSCertificateCollectionGet, std::ref(app)));

    BMCWEB_ROUTE(
        app, "/redfish/v1/Managers/<str>/NetworkProtocol/HTTPS/Certificates/")
        .privileges(redfish::privileges::postCertificateCollection)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleHTTPSCertificateCollectionPost, std::ref(app)));

    BMCWEB_ROUTE(
        app,
        "/redfish/v1/Managers/<str>/NetworkProtocol/HTTPS/Certificates/<str>/")
        .privileges(redfish::privileges::getCertificate)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleHTTPSCertificateGet, std::ref(app)));
}

inline void handleLDAPCertificateCollectionGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    asyncResp->res.jsonValue["@odata.id"] =
        "/redfish/v1/AccountService/LDAP/Certificates";
    asyncResp->res.jsonValue["@odata.type"] =
        "#CertificateCollection.CertificateCollection";
    asyncResp->res.jsonValue["Name"] = "LDAP Certificates Collection";
    asyncResp->res.jsonValue["Description"] =
        "A Collection of LDAP certificate instances";

    getCertificateList(asyncResp, certs::ldapObjectPath,
                       "/Members"_json_pointer,
                       "/Members@odata.count"_json_pointer);
}

inline void handleLDAPCertificateCollectionPost(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }
    std::string certHttpBody = getCertificateFromReqBody(asyncResp, req);

    if (certHttpBody.empty())
    {
        BMCWEB_LOG_ERROR("Cannot get certificate from request body.");
        messages::unrecognizedRequestBody(asyncResp->res);
        return;
    }

    std::shared_ptr<CertificateFile> certFile =
        std::make_shared<CertificateFile>(certHttpBody);

    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp, certFile](const boost::system::error_code& ec,
                              const std::string& objectPath) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            sdbusplus::message::object_path path(objectPath);
            std::string certId = path.filename();
            const boost::urls::url certURL = boost::urls::format(
                "/redfish/v1/AccountService/LDAP/Certificates/{}", certId);
            getCertificateProperties(asyncResp, objectPath,
                                     certs::ldapServiceName, certId, certURL,
                                     "LDAP Certificate");
            BMCWEB_LOG_DEBUG("LDAP certificate install file={}",
                             certFile->getCertFilePath());
        },
        certs::ldapServiceName, certs::ldapObjectPath, certs::certInstallIntf,
        "Install", certFile->getCertFilePath());
}

inline void handleLDAPCertificateGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp, const std::string& id)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    BMCWEB_LOG_DEBUG("LDAP Certificate ID={}", id);
    const boost::urls::url certURL = boost::urls::format(
        "/redfish/v1/AccountService/LDAP/Certificates/{}", id);
    std::string objPath =
        sdbusplus::message::object_path(certs::ldapObjectPath) / id;
    getCertificateProperties(asyncResp, objPath, certs::ldapServiceName, id,
                             certURL, "LDAP Certificate");
}

inline void handleLDAPCertificateDelete(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp, const std::string& id)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    BMCWEB_LOG_DEBUG("Delete LDAP Certificate ID={}", id);
    std::string objPath =
        sdbusplus::message::object_path(certs::ldapObjectPath) / id;

    deleteCertificate(asyncResp, certs::ldapServiceName, objPath);
}

inline void requestRoutesLDAPCertificate(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/AccountService/LDAP/Certificates/")
        .privileges(redfish::privileges::getCertificateCollection)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLDAPCertificateCollectionGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/AccountService/LDAP/Certificates/")
        .privileges(redfish::privileges::postCertificateCollection)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleLDAPCertificateCollectionPost, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/AccountService/LDAP/Certificates/<str>/")
        .privileges(redfish::privileges::getCertificate)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleLDAPCertificateGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/AccountService/LDAP/Certificates/<str>/")
        .privileges(redfish::privileges::deleteCertificate)
        .methods(boost::beast::http::verb::delete_)(
            std::bind_front(handleLDAPCertificateDelete, std::ref(app)));
} // requestRoutesLDAPCertificate

inline void handleTrustStoreCertificateCollectionGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    asyncResp->res.jsonValue["@odata.id"] =
        boost::urls::format("/redfish/v1/Managers/{}/Truststore/Certificates/",
                            BMCWEB_REDFISH_MANAGER_URI_NAME);
    asyncResp->res.jsonValue["@odata.type"] =
        "#CertificateCollection.CertificateCollection";
    asyncResp->res.jsonValue["Name"] = "TrustStore Certificates Collection";
    asyncResp->res.jsonValue["Description"] =
        "A Collection of TrustStore certificate instances";

    getCertificateList(asyncResp, certs::authorityObjectPath,
                       "/Members"_json_pointer,
                       "/Members@odata.count"_json_pointer);
}

inline void handleTrustStoreCertificateCollectionPost(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    std::string certHttpBody = getCertificateFromReqBody(asyncResp, req);

    if (certHttpBody.empty())
    {
        BMCWEB_LOG_ERROR("Cannot get certificate from request body.");
        messages::unrecognizedRequestBody(asyncResp->res);
        return;
    }

    std::shared_ptr<CertificateFile> certFile =
        std::make_shared<CertificateFile>(certHttpBody);
    dbus::utility::async_method_call(
        asyncResp,
        [asyncResp, certFile](const boost::system::error_code& ec,
                              const std::string& objectPath) {
            if (ec)
            {
                BMCWEB_LOG_ERROR("DBUS response error: {}", ec);
                messages::internalError(asyncResp->res);
                return;
            }

            sdbusplus::message::object_path path(objectPath);
            std::string certId = path.filename();
            const boost::urls::url certURL = boost::urls::format(
                "/redfish/v1/Managers/{}/Truststore/Certificates/{}",
                BMCWEB_REDFISH_MANAGER_URI_NAME, certId);
            getCertificateProperties(asyncResp, objectPath,
                                     certs::authorityServiceName, certId,
                                     certURL, "TrustStore Certificate");
            BMCWEB_LOG_DEBUG("TrustStore certificate install file={}",
                             certFile->getCertFilePath());
        },
        certs::authorityServiceName, certs::authorityObjectPath,
        certs::certInstallIntf, "Install", certFile->getCertFilePath());
}

inline void handleTrustStoreCertificateGet(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId, const std::string& certId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    BMCWEB_LOG_DEBUG("Truststore Certificate ID={}", certId);
    const boost::urls::url certURL = boost::urls::format(
        "/redfish/v1/Managers/{}/Truststore/Certificates/{}",
        BMCWEB_REDFISH_MANAGER_URI_NAME, certId);
    std::string objPath =
        sdbusplus::message::object_path(certs::authorityObjectPath) / certId;
    getCertificateProperties(asyncResp, objPath, certs::authorityServiceName,
                             certId, certURL, "TrustStore Certificate");
}

inline void handleTrustStoreCertificateDelete(
    App& app, const crow::Request& req,
    const std::shared_ptr<bmcweb::AsyncResp>& asyncResp,
    const std::string& managerId, const std::string& certId)
{
    if (!redfish::setUpRedfishRoute(app, req, asyncResp))
    {
        return;
    }

    if (managerId != BMCWEB_REDFISH_MANAGER_URI_NAME)
    {
        messages::resourceNotFound(asyncResp->res, "Manager", managerId);
        return;
    }

    BMCWEB_LOG_DEBUG("Delete TrustStore Certificate ID={}", certId);
    std::string objPath =
        sdbusplus::message::object_path(certs::authorityObjectPath) / certId;

    deleteCertificate(asyncResp, certs::authorityServiceName, objPath);
}

inline void requestRoutesTrustStoreCertificate(App& app)
{
    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/Truststore/Certificates/")
        .privileges(redfish::privileges::getCertificate)
        .methods(boost::beast::http::verb::get)(std::bind_front(
            handleTrustStoreCertificateCollectionGet, std::ref(app)));

    BMCWEB_ROUTE(app, "/redfish/v1/Managers/<str>/Truststore/Certificates/")
        .privileges(redfish::privileges::postCertificateCollection)
        .methods(boost::beast::http::verb::post)(std::bind_front(
            handleTrustStoreCertificateCollectionPost, std::ref(app)));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/<str>/Truststore/Certificates/<str>/")
        .privileges(redfish::privileges::getCertificate)
        .methods(boost::beast::http::verb::get)(
            std::bind_front(handleTrustStoreCertificateGet, std::ref(app)));

    BMCWEB_ROUTE(app,
                 "/redfish/v1/Managers/<str>/Truststore/Certificates/<str>/")
        .privileges(redfish::privileges::deleteCertificate)
        .methods(boost::beast::http::verb::delete_)(
            std::bind_front(handleTrustStoreCertificateDelete, std::ref(app)));
} // requestRoutesTrustStoreCertificate
} // namespace redfish

/*
** Copyright 2021 Bloomberg Finance L.P.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
#ifndef BLOOMBERG_AMQPPROX_AUTHRESPONSEDATA
#define BLOOMBERG_AMQPPROX_AUTHRESPONSEDATA

#include <string>
#include <string_view>

namespace Bloomberg {
namespace amqpprox {

/**
 * \brief Provide a class to hold response data for authn/authz operations
 */
class AuthResponseData {
  public:
    enum class AuthResult { ALLOW, DENY };

  private:
    AuthResult  d_authResult;
    std::string d_reason;
    std::string d_authMechanism;
    std::string d_credentials;

  public:
    explicit AuthResponseData(const AuthResult &authResult,
                              std::string_view  reason        = "",
                              std::string_view  authMechanism = "",
                              std::string_view  credentials   = "");

    inline const AuthResult  getAuthResult() const;
    inline const std::string getReason() const;
    inline const std::string getAuthMechanism() const;
    inline const std::string getCredentials() const;
};

inline const AuthResponseData::AuthResult
AuthResponseData::getAuthResult() const
{
    return d_authResult;
}

inline const std::string AuthResponseData::getReason() const
{
    return d_reason;
}

inline const std::string AuthResponseData::getAuthMechanism() const
{
    return d_authMechanism;
}

inline const std::string AuthResponseData::getCredentials() const
{
    return d_credentials;
}

}
}

#endif

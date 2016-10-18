/**
 * @file handlers.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "json_parse_utils.h"

extern "C" {
#include <pjsip.h>
#include <pjlib-util.h>
#include <pjlib.h>
}

#include "handlers.h"
#include "log.h"
#include "subscriber_data_manager.h"
#include "ifchandler.h"
#include "registration_utils.h"
#include "stack.h"
#include "pjutils.h"
#include "sproutsasevent.h"
#include "uri_classifier.h"

static bool sdm_access_common(SubscriberDataManager::AoRPair** aor_pair,
                              bool& previous_aor_pair_alloced,
                              std::string aor_id,
                              SubscriberDataManager* current_sdm,
                              SubscriberDataManager* remote_sdm,
                              SubscriberDataManager::AoRPair** previous_aor_pair,
                              SAS::TrailId trail)
{
  // Find the current bindings for the AoR.
  delete *aor_pair;
  *aor_pair = current_sdm->get_aor_data(aor_id, trail);
  TRC_DEBUG("Retrieved AoR data %p", *aor_pair);

  if ((*aor_pair == NULL) ||
      ((*aor_pair)->get_current() == NULL))
  {
    // Failed to get data for the AoR because there is no connection
    // to the store.
    TRC_ERROR("Failed to get AoR binding for %s from store", aor_id.c_str());
    return false;
  }

  // If we don't have any bindings, try the backup AoR and/or store.
  //LCOV_EXCL_START
  if ((*aor_pair)->get_current()->bindings().empty())
  {
    if ((*previous_aor_pair == NULL) &&
        (remote_sdm != NULL) &&
        (remote_sdm->has_servers()))
    {
      *previous_aor_pair = remote_sdm->get_aor_data(aor_id, trail);
      previous_aor_pair_alloced = true;
    }

    if ((*previous_aor_pair != NULL) &&
        ((*previous_aor_pair)->get_current() != NULL) &&
        (!(*previous_aor_pair)->get_current()->bindings().empty()))
    {
      for (SubscriberDataManager::AoR::Bindings::const_iterator i =
             (*previous_aor_pair)->get_current()->bindings().begin();
           i != (*previous_aor_pair)->get_current()->bindings().end();
           ++i)
      {
        SubscriberDataManager::AoR::Binding* src = i->second;
        SubscriberDataManager::AoR::Binding* dst =
           (*aor_pair)->get_current()->get_binding(i->first);
        *dst = *src;
      }

      for (SubscriberDataManager::AoR::Subscriptions::const_iterator i =
             (*previous_aor_pair)->get_current()->subscriptions().begin();
           i != (*previous_aor_pair)->get_current()->subscriptions().end();
           ++i)
      {
        SubscriberDataManager::AoR::Subscription* src = i->second;
        SubscriberDataManager::AoR::Subscription* dst =
           (*aor_pair)->get_current()->get_subscription(i->first);
        *dst = *src;
      }
    }
  }
  //LCOV_EXCL_STOP

  return true;
}

static bool get_reg_data(HSSConnection* hss,
                         std::string aor_id,
                         std::vector<std::string>& irs_impus,
                         std::map<std::string, Ifcs> ifc_map,
                         SAS::TrailId trail)
{
  std::string state;
  HTTPCode http_code = hss->get_registration_data(aor_id,
                                                  state,
                                                  ifc_map,
                                                  irs_impus,
                                                  trail);

  if ((http_code != HTTP_OK) || irs_impus.empty())
  {
    // We were unable to determine the set of IMPUs for this AoR.  Push the AoR
    // we have into the IRS list so that we have at least one IMPU we can issue
    // NOTIFYs for.
    TRC_WARNING("Unable to get Implicit Registration Set for %s: %d", aor_id.c_str(), http_code);
    irs_impus.clear();
    irs_impus.push_back(aor_id);
  }

  return (http_code == HTTP_OK);
}

static void report_sip_all_register_marker(SAS::TrailId trail, std::string uri_str)
{
  // Parse the SIP URI and get the username from it.
  pj_pool_t* tmp_pool = pj_pool_create(&stack_data.cp.factory, "handlers", 1024, 512, NULL);
  pjsip_uri* uri = PJUtils::uri_from_string(uri_str, tmp_pool);

  if (uri != NULL)
  {
    pj_str_t user = PJUtils::user_from_uri(uri);

    // Create and report the marker.
    SAS::Marker sip_all_register(trail, MARKER_ID_SIP_ALL_REGISTER, 1u);
    sip_all_register.add_var_param(PJUtils::strip_uri_scheme(uri_str));
    // Add the DN parameter. If the user part is not numeric just log it in
    // its entirety.
    sip_all_register.add_var_param(URIClassifier::is_user_numeric(user) ?
                                   PJUtils::remove_visual_separators(user) :
                                   PJUtils::pj_str_to_string(&user));
    SAS::report_marker(sip_all_register);
  }
  else
  {
    TRC_WARNING("Could not raise SAS REGISTER marker for unparseable URI '%s'", uri_str.c_str());
  }

  // Remember to release the temporary pool.
  pj_pool_release(tmp_pool);
}

//LCOV_EXCL_START - don't want to actually run the handlers in the UT
void AoRTimeoutTask::run()
{
  if (_req.method() != htp_method_POST)
  {
    send_http_reply(HTTP_BADMETHOD);
    delete this;
    return;
  }

  HTTPCode rc = parse_response(_req.get_rx_body());

  if (rc != HTTP_OK)
  {
    TRC_DEBUG("Unable to parse response from Chronos");
    send_http_reply(rc);
    delete this;
    return;
  }

  send_http_reply(HTTP_OK);

  SAS::Marker start_marker(trail(), MARKER_ID_START, 1u);
  SAS::report_marker(start_marker);

  handle_response();

  SAS::Marker end_marker(trail(), MARKER_ID_END, 1u);
  SAS::report_marker(end_marker);

  delete this;
}

void AuthTimeoutTask::run()
{
  if (_req.method() != htp_method_POST)
  {
    send_http_reply(HTTP_BADMETHOD);
    delete this;
    return;
  }

  SAS::Marker start_marker(trail(), MARKER_ID_START, 1u);
  SAS::report_marker(start_marker);

  HTTPCode rc = handle_response(_req.get_rx_body());

  SAS::Marker end_marker(trail(), MARKER_ID_END, 1u);
  SAS::report_marker(end_marker);

  if (rc != HTTP_OK)
  {
    TRC_DEBUG("Unable to handle callback from Chronos");
    send_http_reply(rc);
    delete this;
    return;
  }

  send_http_reply(HTTP_OK);
  delete this;
}
//LCOV_EXCL_STOP

void DeregistrationTask::run()
{
  // HTTP method must be a DELETE
  if (_req.method() != htp_method_DELETE)
  {
    TRC_WARNING("HTTP method isn't delete");
    send_http_reply(HTTP_BADMETHOD);
    delete this;
    return;
  }

  // Mandatory query parameter 'send-notifications' that must be true or false
  _notify = _req.param("send-notifications");

  if (_notify != "true" && _notify != "false")
  {
    TRC_WARNING("Mandatory send-notifications param is missing or invalid, send 400");
    send_http_reply(HTTP_BAD_REQUEST);
    delete this;
    return;
  }

  // Parse the JSON body
  HTTPCode rc = parse_request(_req.get_rx_body());

  if (rc != HTTP_OK)
  {
    TRC_WARNING("Request body is invalid, send %d", rc);
    send_http_reply(rc);
    delete this;
    return;
  }

  rc = handle_request();

  send_http_reply(rc);
  delete this;
}

void AoRTimeoutTask::handle_response()
{
  bool all_bindings_expired = false;

  // Determine the set of IMPUs in the Implicit Registration Set
  std::vector<std::string> irs_impus;
  std::map<std::string, Ifcs> ifc_map;
  get_reg_data(_cfg->_hss, _aor_id, irs_impus, ifc_map, trail());

  SubscriberDataManager::AoRPair* aor_pair = set_aor_data(_cfg->_sdm,
                                                          _aor_id,
                                                          irs_impus,
                                                          NULL,
                                                          _cfg->_remote_sdm,
                                                          all_bindings_expired);

  if (aor_pair != NULL)
  {
    // If we have a remote store, try to store this there too.  We don't worry
    // about failures in this case.
    // LCOV_EXCL_START
    if ((_cfg->_remote_sdm != NULL) && (_cfg->_remote_sdm->has_servers()))
    {
      bool ignored;
      SubscriberDataManager::AoRPair* remote_aor_pair =
                                         set_aor_data(_cfg->_remote_sdm,
                                                      _aor_id,
                                                      irs_impus,
                                                      aor_pair,
                                                      NULL,
                                                      ignored);
      delete remote_aor_pair;
    }
    // LCOV_EXCL_STOP

    if (all_bindings_expired)
    {
      TRC_DEBUG("All bindings have expired based on a Chronos callback - triggering deregistration at the HSS");
      SAS::Event event(trail(), SASEvent::REGISTRATION_EXPIRED, 0);
      event.add_var_param(_aor_id);
      SAS::report_event(event);

      _cfg->_hss->update_registration_state(_aor_id, "", HSSConnection::DEREG_TIMEOUT, trail());
    }
    else
    {
      SAS::Event event(trail(), SASEvent::SOME_BINDINGS_EXPIRED, 0);
      event.add_var_param(_aor_id);
      SAS::report_event(event);
    }
  }
  else
  {
    // We couldn't update the SubscriberDataManager but there is nothing else we can do to
    // recover from this.
    TRC_INFO("Could not update SubscriberDataManager on registration timeout for AoR: %s",
             _aor_id.c_str());
  }

  delete aor_pair;
  report_sip_all_register_marker(trail(), _aor_id);
}

SubscriberDataManager::AoRPair* AoRTimeoutTask::set_aor_data(
                          SubscriberDataManager* current_sdm,
                          std::string aor_id,
                          std::vector<std::string> irs_impus,
                          SubscriberDataManager::AoRPair* previous_aor_pair,
                          SubscriberDataManager* remote_sdm,
                          bool& all_bindings_expired)
{
  SubscriberDataManager::AoRPair* aor_pair = NULL;
  bool previous_aor_pair_alloced = false;
  Store::Status set_rc;

  do
  {
    if (!sdm_access_common(&aor_pair,
                           previous_aor_pair_alloced,
                           aor_id,
                           current_sdm,
                           remote_sdm,
                           &previous_aor_pair,
                           trail()))
    {
      break;
    }

    set_rc = current_sdm->set_aor_data(aor_id,
                                       irs_impus,
                                       aor_pair,
                                       trail(),
                                       all_bindings_expired);
    if (set_rc != Store::OK)
    {
      delete aor_pair; aor_pair = NULL;
    }
  }
  while (set_rc == Store::DATA_CONTENTION);

  // If we allocated the AoR, tidy up.
  // LCOV_EXCL_START
  if (previous_aor_pair_alloced)
  {
    delete previous_aor_pair;
  }
  // LCOV_EXCL_STOP

  return aor_pair;
}

// Retrieve the aor ID from the opaque data
HTTPCode AoRTimeoutTask::parse_response(std::string body)
{
  rapidjson::Document doc;
  std::string json_str = body;
  doc.Parse<0>(json_str.c_str());

  if (doc.HasParseError())
  {
    TRC_DEBUG("Failed to parse opaque data as JSON: %s\nError: %s",
              json_str.c_str(),
              rapidjson::GetParseError_En(doc.GetParseError()));
    return HTTP_BAD_REQUEST;
  }

  try
  {
    JSON_GET_STRING_MEMBER(doc, "aor_id", _aor_id);
  }
  catch (JsonFormatError err)
  {
    TRC_DEBUG("Badly formed opaque data (missing aor_id)");
    return HTTP_BAD_REQUEST;
  }
  TRC_DEBUG("Handling timer pop for AoR id: %s", _aor_id.c_str());
  return HTTP_OK;
}

// Retrieve the aors and any private IDs from the request body
HTTPCode DeregistrationTask::parse_request(std::string body)
{
  rapidjson::Document doc;
  doc.Parse<0>(body.c_str());

  if (doc.HasParseError())
  {
    TRC_INFO("Failed to parse data as JSON: %s\nError: %s",
             body.c_str(),
             rapidjson::GetParseError_En(doc.GetParseError()));
    return HTTP_BAD_REQUEST;
  }

  try
  {
    JSON_ASSERT_CONTAINS(doc, "registrations");
    JSON_ASSERT_ARRAY(doc["registrations"]);
    const rapidjson::Value& reg_arr = doc["registrations"];

    for (rapidjson::Value::ConstValueIterator reg_it = reg_arr.Begin();
         reg_it != reg_arr.End();
         ++reg_it)
    {
      try
      {
        std::string primary_impu;
        std::string impi = "";
        JSON_GET_STRING_MEMBER(*reg_it, "primary-impu", primary_impu);

        if (((*reg_it).HasMember("impi")) &&
            ((*reg_it)["impi"].IsString()))
        {
          impi = (*reg_it)["impi"].GetString();
        }

        _bindings.insert(std::make_pair(primary_impu, impi));
      }
      catch (JsonFormatError err)
      {
        TRC_WARNING("Invalid JSON - registration doesn't contain primary-impu");
        return HTTP_BAD_REQUEST;
      }
    }
  }
  catch (JsonFormatError err)
  {
    TRC_INFO("Registrations not available in JSON");
    return HTTP_BAD_REQUEST;
  }

  TRC_DEBUG("HTTP request successfully parsed");
  return HTTP_OK;
}

HTTPCode DeregistrationTask::handle_request()
{
  std::set<std::string> impis_to_delete;

  for (std::map<std::string, std::string>::iterator it=_bindings.begin();
       it!=_bindings.end();
       ++it)
  {
    SubscriberDataManager::AoRPair* aor_pair =
      deregister_bindings(_cfg->_sdm,
                          it->first,
                          it->second,
                          NULL,
                          _cfg->_remote_sdm,
                          impis_to_delete);

    // LCOV_EXCL_START
    if ((aor_pair != NULL) &&
        (aor_pair->get_current() != NULL))
    {
      // If we have a remote store, try to store this there too.  We don't worry
      // about failures in this case.
      if (_cfg->_remote_sdm != NULL)
      {
        SubscriberDataManager::AoRPair* remote_aor_pair =
          deregister_bindings(_cfg->_remote_sdm,
                              it->first,
                              it->second,
                              aor_pair,
                              NULL,
                              impis_to_delete);
        delete remote_aor_pair;
      }
    }
    // LCOV_EXCL_STOP
    else
    {
      // Can't connect to memcached, return 500. If this isn't the first AoR being edited
      // then this will lead to an inconsistency between the HSS and Sprout, as
      // Sprout will have changed some of the AoRs, but HSS will believe they all failed.
      // Sprout accepts changes to AoRs that don't exist though.
      TRC_WARNING("Unable to connect to memcached for AoR %s", it->first.c_str());

      delete aor_pair;
      return HTTP_SERVER_ERROR;
    }

    delete aor_pair;
  }

  // Delete IMPIs from the store.
  for(std::set<std::string>::iterator impi = impis_to_delete.begin();
      impi != impis_to_delete.end();
      ++impi)
  {
    TRC_DEBUG("Delete %s from the IMPI store", impi->c_str());

    Store::Status store_rc = Store::OK;
    ImpiStore::Impi* impi_obj = NULL;

    do
    {
      // Free any IMPI we had from the last loop iteration.
      delete impi_obj; impi_obj = NULL;

      impi_obj = _cfg->_impi_store->get_impi(*impi, _trail);

      if (impi_obj != NULL)
      {
        store_rc = _cfg->_impi_store->delete_impi(impi_obj, _trail);
      }
    }
    while ((impi_obj != NULL) && (store_rc == Store::DATA_CONTENTION));

    delete impi_obj; impi_obj = NULL;
  }

  return HTTP_OK;
}

SubscriberDataManager::AoRPair* DeregistrationTask::deregister_bindings(
                                        SubscriberDataManager* current_sdm,
                                        std::string aor_id,
                                        std::string private_id,
                                        SubscriberDataManager::AoRPair* previous_aor_pair,
                                        SubscriberDataManager* remote_sdm,
                                        std::set<std::string>& impis_to_delete)
{
  SubscriberDataManager::AoRPair* aor_pair = NULL;
  bool previous_aor_pair_alloced = false;
  bool all_bindings_expired = false;
  bool got_ifcs;
  Store::Status set_rc;
  std::vector<std::string> impis_to_dereg;

  // Get registration data
  std::vector<std::string> irs_impus;
  std::map<std::string, Ifcs> ifc_map;
  got_ifcs = get_reg_data(_cfg->_hss, aor_id, irs_impus, ifc_map, trail());

  do
  {
    if (!sdm_access_common(&aor_pair,
                           previous_aor_pair_alloced,
                           aor_id,
                           current_sdm,
                           remote_sdm,
                           &previous_aor_pair,
                           trail()))
    {
      break;
    }

    std::vector<std::string> binding_ids;

    for (SubscriberDataManager::AoR::Bindings::const_iterator i =
           aor_pair->get_current()->bindings().begin();
         i != aor_pair->get_current()->bindings().end();
         ++i)
    {
      // Get a list of the bindings to iterate over
      binding_ids.push_back(i->first);
    }

    for (std::vector<std::string>::const_iterator i = binding_ids.begin();
         i != binding_ids.end();
         ++i)
    {
      std::string b_id = *i;
      SubscriberDataManager::AoR::Binding* b =
                                  aor_pair->get_current()->get_binding(b_id);

      if (private_id.empty() || private_id == b->_private_id)
      {
        if (!b->_private_id.empty())
        {
          // Record the IMPIs that we need to delete as a result of deleting
          // this binding.
          impis_to_delete.insert(b->_private_id);
        }
        aor_pair->get_current()->remove_binding(b_id);
      }
    }

    set_rc = current_sdm->set_aor_data(aor_id,
                                       irs_impus,
                                       aor_pair,
                                       trail(),
                                       all_bindings_expired);
    if (set_rc != Store::OK)
    {
      delete aor_pair; aor_pair = NULL;
    }
  }
  while (set_rc == Store::DATA_CONTENTION);

  if (private_id == "")
  {
    // Deregister with any application servers
    TRC_INFO("ID %s", aor_id.c_str());

    if (got_ifcs)
    {
      RegistrationUtils::deregister_with_application_servers(ifc_map[aor_id],
                                                             current_sdm,
                                                             aor_id,
                                                             trail());
    }
  }

  // If we allocated the AoR, tidy up.
  if (previous_aor_pair_alloced)
  {
    delete previous_aor_pair; //LCOV_EXCL_LINE
  }

  return aor_pair;
}

HTTPCode AuthTimeoutTask::handle_response(std::string body)
{
  rapidjson::Document doc;
  std::string json_str = body;
  doc.Parse<0>(json_str.c_str());

  if (doc.HasParseError())
  {
    TRC_INFO("Failed to parse opaque data as JSON: %s\nError: %s",
             json_str.c_str(),
             rapidjson::GetParseError_En(doc.GetParseError()));
    return HTTP_BAD_REQUEST;
  }

  try
  {
    JSON_GET_STRING_MEMBER(doc, "impu", _impu);
    JSON_GET_STRING_MEMBER(doc, "impi", _impi);
    JSON_GET_STRING_MEMBER(doc, "nonce", _nonce);
    report_sip_all_register_marker(trail(), _impu);
  }
  catch (JsonFormatError err)
  {
    TRC_INFO("Badly formed opaque data (missing impu, impi or nonce");
    return HTTP_BAD_REQUEST;
  }

  bool success = false;
  ImpiStore::Impi* impi = _cfg->_impi_store->get_impi_with_nonce(_impi, _nonce, trail());
  ImpiStore::AuthChallenge* auth_challenge = NULL;
  if (impi != NULL)
  {
    auth_challenge = impi->get_auth_challenge(_nonce);
  }
  if (auth_challenge != NULL)
  {
    // Use the original REGISTER's branch parameter for SAS
    // correlation
    correlate_trail_to_challenge(auth_challenge, trail());

    // If authentication completed, we'll have incremented the nonce count.
    // If not, authentication has timed out.
    if (auth_challenge->nonce_count == ImpiStore::AuthChallenge::INITIAL_NONCE_COUNT)
    {
      TRC_DEBUG("AV for %s:%s has timed out", _impi.c_str(), _nonce.c_str());

      // The AUTHENTICATION_TIMEOUT SAR is idempotent, so there's no
      // problem if Chronos' timer pops twice (e.g. if we have high
      // latency and these operations take more than 2 seconds).

      // If either of these operations fail, we return a 500 Internal
      // Server Error - this will trigger Chronos to try a different
      // Sprout, which may have better connectivity to Homestead or Memcached.
      HTTPCode hss_query = _cfg->_hss->update_registration_state(_impu, _impi, HSSConnection::AUTH_TIMEOUT, trail());

      if (hss_query == HTTP_OK)
      {
        success = true;
      }
    }
    else
    {
      SAS::Event event(trail(), SASEvent::AUTHENTICATION_TIMER_POP_IGNORED, 0);
      SAS::report_event(event);

      TRC_DEBUG("Tombstone record indicates Authentication Vector has been used successfully - ignoring timer pop");
      success = true;
    }
  }
  else
  {
    TRC_WARNING("Could not find AV for %s:%s when checking authentication timeout", _impi.c_str(), _nonce.c_str()); // LCOV_EXCL_LINE
  }
  delete impi;

  return success ? HTTP_OK : HTTP_SERVER_ERROR;
}
